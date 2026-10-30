"""Trust a device over physical USB once; save its wireless identity locally, never in Git."""

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    args = parser.parse_args()
    try:
        with server.device_connection(args.port) as connection:
            time.sleep(0.5)
            connection.reset_input_buffer()
            connection.write(b"_")
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                line = connection.readline()
                if not line.startswith(b"LINK_IDENTITY "):
                    continue
                identity = json.loads(line[len(b"LINK_IDENTITY "):])
                fingerprint = hashlib.sha256(identity["certificate"].encode()).hexdigest()[:12]
                folder = ROOT / "keys"
                folder.mkdir(exist_ok=True)
                path = folder / ("device-link-" + fingerprint + ".json")
                path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
                print("Trusted device:", identity["url"])
                print("Local identity:", path)
                return
            raise RuntimeError("No pairing response from the device")
    finally:
        if server.CONNECTION:
            server.CONNECTION.close()


if __name__ == "__main__":
    main()
