"""Start the local screen mirror using a paired Wi-Fi connection, without a console window."""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URL = "http://127.0.0.1:8765"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--host")
    args = parser.parse_args()
    identity = args.identity
    if identity is None:
        identities = list((ROOT / "keys").glob("device-link-*.json"))
        if len(identities) != 1:
            raise SystemExit("Select the device with --identity keys/device-link-....json")
        identity = identities[0]
    identity = identity.resolve(strict=True)
    with socket.socket() as probe:
        if probe.connect_ex(("127.0.0.1", 8765)) == 0:
            with urllib.request.urlopen(URL + "/api/health", timeout=3) as response:
                if json.load(response).get("transport") != "wifi":
                    raise SystemExit("Port 8765 is already in use. Stop that mirror before switching transport.")
            print("Wireless mirror already running:", URL)
            return
    captures = ROOT / "captures"
    captures.mkdir(exist_ok=True)
    command = [sys.executable, str(ROOT / "simulator" / "server.py"), "--wireless-identity", str(identity)]
    if args.host:
        command.extend(["--host", args.host])
    with (captures / "wireless-mirror.log").open("ab") as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    for _ in range(30):
        if process.poll() is not None:
            raise SystemExit("Mirror stopped; inspect captures/wireless-mirror.log")
        try:
            with urllib.request.urlopen(URL + "/api/health", timeout=1) as response:
                assert json.load(response)["transport"] == "wifi"
            (captures / "wireless-mirror.pid").write_text(str(process.pid))
            print("Wireless mirror:", URL)
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit("Mirror did not become ready")


if __name__ == "__main__":
    main()
