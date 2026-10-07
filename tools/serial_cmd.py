"""Send device commands over USB serial and print the board's text output (frames removed).

Usage: python tools/serial_cmd.py [--port COM5] [--wait 8] [--grep WORD ...] CMD [CMD ...]
Each CMD is sent as-is ("\\n" allowed); "sleep:N" pauses N seconds between commands.
The USB bridge must not be running (it holds the port).
"""

import argparse
import re
import sys
import time

import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default=None)
parser.add_argument('--wait', type=float, default=6)
parser.add_argument('--grep', nargs='*', default=None)
parser.add_argument('commands', nargs='*')
args = parser.parse_args()

# Set the control lines before opening: opening with RTS asserted resets the ESP32-S3.
port = serial.Serial()
if not args.port:  # the first Espressif USB device (VID 0x303A), whichever COM number it got
    from serial.tools import list_ports

    found = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if not found:
        raise SystemExit('No XIAO found over USB.')
    args.port = found[0]
port.port = args.port
port.baudrate = 115200
port.timeout = 0.1
port.dtr = True
port.rts = False
port.open()
time.sleep(0.2)
port.reset_input_buffer()
out = b''
for command in args.commands:
    if command.startswith('sleep:'):
        end = time.time() + float(command[6:])
        while time.time() < end:
            out += port.read(65536)
        continue
    port.write(command.encode().decode('unicode_escape').encode('latin-1'))
end = time.time() + args.wait
while time.time() < end:
    out += port.read(65536)
port.close()
text = re.sub(r'SCREEN_BEGIN.*?SCREEN_END', '[frame]', out.decode('ascii', 'replace'), flags=re.S)
text = re.sub(r'JPEG_BEGIN.*?JPEG_END', '[jpeg]', text, flags=re.S)
for line in text.splitlines():
    line = ''.join(c if 32 <= ord(c) < 127 else '?' for c in line).rstrip()
    if not line:
        continue
    if args.grep is None or any(word in line for word in args.grep):
        print(line)
