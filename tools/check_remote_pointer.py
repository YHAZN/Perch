"""Listen for Windows Raw Input from Perch while running two click-free pad sweeps.

Requires the local mirror and Remote > Trackpad. Moves the PC pointer briefly;
never injects host input or presses a mouse button. HID events come from Perch.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import threading
import time

from shots import post


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True, help='Perch Bluetooth address for device filtering')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    address = args.address.replace(':', '').upper()
    assert len(address) == 12 and all(v in '0123456789ABCDEF' for v in address)
    user = c.WinDLL('user32', use_last_error=True)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    callback_type = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)

    class WindowClass(c.Structure):
        _fields_ = [('style', w.UINT), ('proc', callback_type), ('cls_extra', c.c_int), ('wnd_extra', c.c_int),
                    ('instance', w.HINSTANCE), ('icon', w.HANDLE), ('cursor', w.HANDLE), ('background', w.HANDLE),
                    ('menu', w.LPCWSTR), ('name', w.LPCWSTR)]

    class InputDevice(c.Structure):
        _fields_ = [('page', w.USHORT), ('usage', w.USHORT), ('flags', w.DWORD), ('window', w.HWND)]

    class Header(c.Structure):
        _fields_ = [('kind', w.DWORD), ('size', w.DWORD), ('device', w.HANDLE), ('param', w.WPARAM)]

    class Mouse(c.Structure):
        _fields_ = [('flags', w.USHORT), ('padding', w.USHORT), ('buttons', w.DWORD), ('raw_buttons', w.DWORD),
                    ('x', w.LONG), ('y', w.LONG), ('extra', w.DWORD)]

    user.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.DefWindowProcW.restype = c.c_ssize_t
    user.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, c.c_int, c.c_int, c.c_int,
                                   c.c_int, w.HWND, w.HMENU, w.HINSTANCE, c.c_void_p]
    user.CreateWindowExW.restype = w.HWND
    user.GetRawInputData.argtypes = [w.HANDLE, w.UINT, c.c_void_p, c.POINTER(w.UINT), w.UINT]
    user.GetRawInputDeviceInfoW.argtypes = [w.HANDLE, w.UINT, c.c_void_p, c.POINTER(w.UINT)]
    kernel.GetModuleHandleW.restype = w.HMODULE
    events, errors, devices = [], [], {}

    @callback_type
    def receive(hwnd, message, wp, lp):
        if message == 0xFF:
            size = w.UINT()
            user.GetRawInputData(lp, 0x10000003, None, c.byref(size), c.sizeof(Header))
            data = c.create_string_buffer(size.value)
            if user.GetRawInputData(lp, 0x10000003, data, c.byref(size), c.sizeof(Header)) == size.value:
                h = Header.from_buffer_copy(data)
                if h.kind == 0:
                    if h.device not in devices:
                        length = w.UINT()
                        user.GetRawInputDeviceInfoW(h.device, 0x20000007, None, c.byref(length))
                        name = c.create_unicode_buffer(length.value + 1)
                        user.GetRawInputDeviceInfoW(h.device, 0x20000007, name, c.byref(length))
                        devices[h.device] = address in name.value.upper()
                    if devices[h.device]:
                        mouse = Mouse.from_buffer_copy(data, c.sizeof(Header))
                        events.append({'dx': mouse.x, 'dy': mouse.y, 'button_flags': mouse.buttons & 0xFFFF})
        return user.DefWindowProcW(hwnd, message, wp, lp)

    instance = kernel.GetModuleHandleW(None)
    name = 'PerchPointerDiagnostic'
    wc = WindowClass(0, receive, 0, 0, instance, None, None, None, None, name)
    assert user.RegisterClassW(c.byref(wc)), c.get_last_error()
    hwnd = user.CreateWindowExW(0, name, '', 0, 0, 0, 0, 0, w.HWND(-3), None, instance, None)
    assert hwnd, c.get_last_error()
    device = InputDevice(1, 2, 0x100, hwnd)  # mouse / background input sink
    assert user.RegisterRawInputDevices(c.byref(device), 1, c.sizeof(device)), c.get_last_error()
    done = threading.Event()

    def sweeps():
        try:
            post('/api/drag', {'x1': 80, 'y1': 110, 'x2': 120, 'y2': 120, 'ms': 320})
            post('/api/drag', {'x1': 120, 'y1': 120, 'x2': 80, 'y2': 110, 'ms': 320})
        except Exception as error:
            errors.append(str(error))
        finally:
            done.set()

    threading.Thread(target=sweeps, daemon=True).start()
    message = w.MSG()
    deadline = time.monotonic() + 20
    finished = None
    while time.monotonic() < deadline:
        while user.PeekMessageW(c.byref(message), None, 0, 0, 1):
            user.TranslateMessage(c.byref(message))
            user.DispatchMessageW(c.byref(message))
        if done.is_set():
            finished = finished or time.monotonic()
            if time.monotonic() - finished > .5:
                break
        time.sleep(.005)
    report = {'events': events, 'errors': errors, 'button_events': sum(bool(e['button_flags']) for e in events)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    assert done.is_set() and not errors, report
    assert any(e['dx'] > 0 for e in events) and any(e['dx'] < 0 for e in events), 'No bidirectional mouse input from Perch'
    assert report['button_events'] == 0, 'Unexpected mouse buttons during click-free sweeps'
    print('PASS:', len(events), 'Perch mouse events received by Windows; both directions; zero button events')


if __name__ == '__main__':
    main()
