"""Read-only power/UI checks on a USB-connected XIAO. No cloud requests."""

import argparse
import base64
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
import server


def settled_screen(command):
    try:
        server.exchange(command, screen=True)
    except RuntimeError as error:
        recoverable = ("payload stalled", "transfer incomplete", "checksum", "transfer ending")
        if not any(message in str(error) for message in recoverable):
            raise
        print("Recovering current screen without replaying the action:", error)
    time.sleep(0.4)
    return server.exchange("f", screen=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    args = parser.parse_args()
    server.PORT = args.port
    try:
        with server.device_lock(), server.device_connection(args.port) as connection:
            time.sleep(0.5)
            connection.reset_input_buffer()
            connection.write(b"(")
            deadline = time.monotonic() + 4
            report = ""
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", "replace").strip()
                if line.startswith("POWER "):
                    report = line
                    break
            assert report, "No power diagnostic received"
            print(report)
            values = dict(re.findall(r"(\w+)=(-?[\d.]+)", report))
            if values.get("sense") == "0":
                assert values["level"] == "-1", "Unwired sensing must not invent a percentage"
                assert values["source"] == "0" and values["charge"] == "0", (
                    "USB alone must not infer battery power or charging"
                )
                print("PASS unavailable sensing remains unknown")
        captures = ROOT / "captures"
        captures.mkdir(exist_ok=True)
        screens = [
            ("h", "face", "power-face"),
            ("D", "control", "power-control"),
            ("T120,225\n", "battery", "power-details"),
            ("i", "status", "power-settings"),
        ]
        for command, view, name in screens:
            result = settled_screen(command)
            assert result["view"] == view, (command, result["view"])
            image = base64.b64decode(result["image"].split(",", 1)[1])
            (captures / (name + ".bmp")).write_bytes(image)
            print("PASS", name, "240x284 checksum", result["checksum"])
        result = settled_screen("h")
        assert result["view"] == "face"
        print("PASS return to clock face")
    finally:
        if server.CONNECTION:
            server.CONNECTION.close()


if __name__ == "__main__":
    main()
