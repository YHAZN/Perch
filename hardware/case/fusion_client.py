"""Call the official local Fusion MCP endpoint; Fusion must be running.

Uses the standard library only. Every invocation negotiates a fresh MCP session.
"""
import json
import urllib.error
import urllib.request


class FusionClient:
    def __init__(self):
        self.headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        self.sequence = 0
        self.request("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                    "clientInfo": {"name": "Enclosure workspace", "version": "1"}})
        self.request("notifications/initialized", notification=True)

    def request(self, method, params=None, notification=False):
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notification:
            self.sequence += 1
            message["id"] = self.sequence
        request = urllib.request.Request("http://127.0.0.1:27182/mcp", data=json.dumps(message).encode(), headers=self.headers)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                session = response.headers.get("Mcp-Session-Id")
                if session:
                    self.headers["Mcp-Session-Id"] = session
                raw = response.read().decode()
        except urllib.error.HTTPError as error:
            raise RuntimeError(error.read().decode()) from error
        if not raw:
            return None
        if raw.startswith("event:") or raw.startswith("data:"):
            events = [line[5:].strip() for line in raw.splitlines() if line.startswith("data:")]
            raw = events[-1]
        result = json.loads(raw)
        if "error" in result:
            raise RuntimeError(result["error"])
        return result.get("result")

    def call(self, name, arguments):
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(result)
        for block in result.get("content", []):
            if block.get("type") != "text":
                continue
            try:
                payload = json.loads(block["text"])
            except (json.JSONDecodeError, KeyError):
                continue
            if payload.get("success") is False:
                raise RuntimeError(payload)
        return result


if __name__ == "__main__":
    import argparse
    import sys
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool")
    parser.add_argument("arguments", help="JSON object")
    args = parser.parse_args()
    print(json.dumps(FusionClient().call(args.tool, json.loads(args.arguments)), ensure_ascii=True))
