"""Run developer commands or retrieve logs over the device's trusted HTTPS link."""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
from wireless import WirelessConnection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--host", help="Override the device DHCP address, retaining certificate verification")
    parser.add_argument("--command")
    parser.add_argument("--logs", action="store_true")
    parser.add_argument("--output", type=Path, help="Write the raw response, including binary transfers")
    parser.add_argument("--export", help="App record: photo,ID; screen,ID; thumb,ID; chat,ID; answer,ID; audio,ID; or list")
    args = parser.parse_args()
    connection = WirelessConnection(args.identity, args.host)
    try:
        if args.command is not None:
            command = args.command + ("\n" if len(args.command) > 1 else "")
            connection.write(command.encode("ascii"))
            data = connection.response_bytes()
            if args.output:
                args.output.write_bytes(data)
                print("Saved", len(data), "bytes to", args.output)
            else:
                print(data.decode("utf-8", "replace"), end="")
        if args.export:
            if not args.output:
                raise SystemExit("--export requires --output")
            connection.write(("J" + args.export + "\n").encode("ascii"))
            header, data = connection.response_bytes().split(b"\n", 1)
            fields = header.split()
            if fields[0] != b"DATA_BEGIN":
                raise RuntimeError("Export unavailable")
            length = int(fields[1])
            if data[length:].strip() != b"DATA_END":
                raise RuntimeError("Export transfer incomplete")
            args.output.write_bytes(data[:length])
            print("Exported", length, "bytes to", args.output)
        if args.logs:
            print(connection.logs(), end="")
    finally:
        connection.close()


if __name__ == "__main__":
    main()
