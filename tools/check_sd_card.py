"""Check microSD; optionally write a scratch file or capture and reload a photo."""

import argparse
import base64
import hashlib
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
import server
from check_power_status import settled_screen


def diagnostic(command, prefix, timeout=15):
    with server.device_lock(), server.device_connection(server.PORT) as connection:
        time.sleep(0.5)
        connection.reset_input_buffer()
        connection.write(command.encode("ascii"))
        deadline = time.monotonic() + timeout
        lines = []
        while time.monotonic() < deadline:
            line = connection.readline().decode("utf-8", "replace").strip()
            if line.startswith(("SD_CARD ", "SD_TEST ", "PHOTO_STORAGE ", "SD_WRITE ")):
                print(line)
                lines.append(line)
            if line.startswith(prefix):
                return line, lines
    raise RuntimeError("No diagnostic response: " + prefix)


def jpeg():
    for attempt in range(3):
        try:
            result = server.exchange("o", capture=True)
            return base64.b64decode(result["image"].split(",", 1)[1])
        except RuntimeError as error:
            if attempt == 2 or not any(message in str(error) for message in
                                       ("payload stalled", "transfer ending", "incomplete JPEG")):
                raise
            # Original export is read-only: retry the transfer, never the shutter.
            print("Retrying original export:", error)
            time.sleep(1)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--write-test", action="store_true", help="Create, verify and remove one scratch file")
    parser.add_argument("--photo-test", action="store_true", help="Save a new camera photo, restart, compare its original")
    parser.add_argument("--existing-photo", action="store_true", help="Verify the newest original without taking another photo")
    parser.add_argument("--migrate", action="store_true", help="Verify and move existing photo files to SD")
    parser.add_argument("--wireless-identity")
    parser.add_argument("--host")
    args = parser.parse_args()
    server.PORT = args.port
    server.WIRELESS_IDENTITY = args.wireless_identity
    server.WIRELESS_HOST = args.host
    try:
        report, _ = diagnostic(")", "SD_CARD ")
        assert "ready=1" in report and "mounted=1" in report, "Card must be mounted"
        if args.migrate:
            line, _ = diagnostic("]", "SD_MIGRATE ", 120)
            assert "busy" not in line, "Migration requires an idle device"
            print(line)
        if args.write_test:
            result, lines = diagnostic("&", "SD_TEST result=", 30)
            assert result.endswith("PASS") and any("cleanup=PASS" in line for line in lines), lines
        if args.photo_test or args.existing_photo:
            if args.photo_test:
                result = settled_screen("a")
                assert result["view"] == "photo", result["view"]
                for line in result.get("diagnostics", "").splitlines():
                    if line.startswith(("PHOTO_SAVED ", "SD_WRITE ", "STORAGE_ERROR ")):
                        print(line)
            with server.device_lock(), server.device_connection(args.port) as connection:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    line = connection.readline().decode("utf-8", "replace").strip()
                    if line.startswith(("PHOTO_SAVED ", "SD_WRITE ", "STORAGE_ERROR ")):
                        print(line)
            _, lines = diagnostic(")", "SD_CARD ")
            assert any("backend=sd" in line for line in lines), "Newest original is not on SD"
            before = jpeg()
            captures = ROOT / "captures"
            captures.mkdir(exist_ok=True)
            (captures / "sd-photo-before.jpg").write_bytes(before)
            # Persisted 40 MHz is the normal display speed, also restored after reset.
            with server.device_lock(), server.device_connection(args.port) as connection:
                connection.write(b"Y40\n")
                time.sleep(0.5)
            time.sleep(6)
            _, lines = diagnostic(")", "SD_CARD ")
            assert any("backend=sd" in line for line in lines), "Newest original is not on SD"
            after = jpeg()
            (captures / "sd-photo-after.jpg").write_bytes(after)
            assert before == after, "Original changed or could not reload after restart"
            print("PASS photo survives restart:", len(after), "bytes, SHA256", hashlib.sha256(after).hexdigest())
            server.exchange("h", screen=True)
    finally:
        if server.CONNECTION:
            server.CONNECTION.close()


if __name__ == "__main__":
    main()
