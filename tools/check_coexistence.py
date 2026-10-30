"""Bounded, unbilled HTTPS upload tests while Bluetooth stays connected."""

import argparse
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
from wireless import WirelessConnection


def fields(line):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--host")
    parser.add_argument("--sizes", nargs="+", type=int, default=[128, 256, 512], help="Payload sizes in KB")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    assert all(1 <= size <= 512 for size in args.sizes), "Keep the diagnostic bounded"
    connection = WirelessConnection(args.identity, args.host)
    results = []
    try:
        connection.write(b":")
        before = fields(connection.response_bytes().decode())
        assert before["wifi"] == 1 and before["bt_connected"] == 1, "Wi-Fi and Bluetooth must both be connected"
        for size in args.sizes:
            prior = [fields(line)["sequence"] for line in connection.logs().splitlines() if line.startswith("COEX_TEST end ")]
            previous = max(prior, default=0)
            connection.write(("l" + str(size) + "\n").encode())
            connection._connection.close()  # release the control TLS session before the test
            deadline = time.monotonic() + 70
            time.sleep(2)
            while time.monotonic() < deadline:
                try:
                    logs = connection.logs()
                except OSError:
                    time.sleep(1)
                    continue
                records = [fields(line) for line in logs.splitlines() if line.startswith("COEX_TEST end ")]
                records = [record for record in records if record["sequence"] > previous]
                if records:
                    result = records[-1]
                    assert result["status"] in (400, 401, 403), "Transport did not complete with the expected unauthenticated response"
                    assert result["before_connected"] == 1 and result["bt_connected"] == 1 and result["bt_drops"] == 0, "Bluetooth disconnected during the upload"
                    results.append({"kilobytes": size, "camera_mode": before.get("camera_mode"), **result})
                    print("PASS", size, "KB; connected throughout; internal free", result["internal_free"], "largest", result["internal_largest"], flush=True)
                    break
                time.sleep(1)
            else:
                raise RuntimeError("No completed diagnostic result; production fallback remains unchanged")
        output = args.output or ROOT / "captures" / ("coexistence-" + time.strftime("%Y%m%d-%H%M%S") + ".json")
        output.write_text(json.dumps(results, indent=2) + "\n")
        print("Saved", output)
    finally:
        connection.close()


if __name__ == "__main__":
    main()
