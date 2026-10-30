"""Pinned HTTPS transport for the device command protocol. No automatic action replay."""

import http.client
import io
import json
import ssl
import time
from pathlib import Path
from urllib.parse import urlsplit


class WirelessConnection:
    def __init__(self, identity_path, host=None):
        identity = json.loads(Path(identity_path).read_text(encoding="utf-8"))
        url = urlsplit(host or identity["url"])
        if url.scheme != "https" or not url.hostname or url.username or url.password:
            raise ValueError("The device address must be HTTPS")
        context = ssl.create_default_context(cadata=identity["certificate"])
        context.check_hostname = False  # trust this device's certificate, independent of its DHCP address
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        self._connection = http.client.HTTPSConnection(url.hostname, url.port or 8443, context=context, timeout=120)
        self._authorization = "Bearer " + identity["token"]
        self._buffer = io.BytesIO()
        self.is_open = True
        self.timeout = 0.5
        self.dtr = True
        self.rts = False

    def _request(self, method, path, body=None):
        try:
            if self._connection.sock is None:
                # Retry only TLS establishment, before any command has been transmitted.
                # A closing previous session can briefly occupy the device's socket slot.
                for attempt in range(3):
                    try:
                        self._connection.timeout = 12
                        self._connection.connect()
                        self._connection.sock.settimeout(120)
                        break
                    except ssl.SSLCertVerificationError:
                        raise
                    except OSError:
                        self._connection.close()
                        if attempt == 2:
                            raise
                        time.sleep(0.5)
                self._connection.timeout = 120
            self._connection.request(method, path, body=body, headers={
                "Authorization": self._authorization,
                "Content-Type": "application/octet-stream",
            })
            response = self._connection.getresponse()
            data = response.read()
            if response.status != 200:
                raise RuntimeError("Device returned HTTP " + str(response.status) + ": " + data.decode("utf-8", "replace"))
            return data
        except Exception:
            # A command might have run before a connection failed. Let the caller decide
            # whether retrying is safe; never replay a shutter, update or settings write.
            self._connection.close()
            raise

    def write(self, data):
        self._buffer = io.BytesIO(self._request("POST", "/command", data))
        return len(data)

    def read(self, size=1):
        result = self._buffer.read(size)
        if not result:
            time.sleep(min(self.timeout, 0.05))
        return result

    def readline(self):
        result = self._buffer.readline()
        if not result:
            time.sleep(min(self.timeout, 0.05))
        return result

    def response_bytes(self):
        return self._buffer.read()

    def reset_input_buffer(self):
        self._buffer = io.BytesIO()

    def logs(self):
        return self._request("GET", "/logs").decode("utf-8", "replace")

    def close(self):
        self._connection.close()
        self.is_open = False
