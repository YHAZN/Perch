"""Verify encrypted wireless commands, authorization, mirroring and app-data exports."""

import argparse
import base64
import hashlib
import http.client
import json
import ssl
import sys
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "simulator"))
import server
from wireless import WirelessConnection


def exported(connection, command):
    connection.write(command.encode("ascii") + b"\n")
    raw = connection.response_bytes()
    header, data = raw.split(b"\n", 1)
    fields = header.split()
    assert fields[0] == b"DATA_BEGIN", "No export returned"
    length = int(fields[1])
    assert data[length:].strip() == b"DATA_END", "Export was truncated"
    return data[:length]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--host")
    args = parser.parse_args()
    identity = json.loads(Path(args.identity).read_text())
    connection = WirelessConnection(args.identity, args.host)
    try:
        valid = connection._authorization
        connection._authorization = "Bearer invalid"
        try:
            connection.write(b":")
        except RuntimeError as error:
            assert "401" in str(error), error
        else:
            raise AssertionError("Unauthenticated command was accepted")
        connection._authorization = valid
        print("PASS unauthorized requests rejected")
        try:
            connection.write(b"_")
        except RuntimeError as error:
            assert "400" in str(error), error
        else:
            raise AssertionError("Pairing identity could be exported wirelessly")
        print("PASS pairing credentials remain USB-only")
        catalog = json.loads(exported(connection, "Jlist"))
        print("PASS catalog", len(catalog["photos"]), "photos", len(catalog["chats"]), "chats")
        if catalog["photos"]:
            photo = catalog["photos"][0]
            original = exported(connection, "Jphoto," + str(photo["id"]))
            assert len(original) == photo["bytes"] and original[:2] == b"\xff\xd8" and original[-2:] == b"\xff\xd9"
            thumbnail = exported(connection, "Jthumb," + str(photo["id"]))
            assert len(thumbnail) == 64 * 64 * 2
            screen = exported(connection, "Jscreen," + str(photo["id"]))
            assert screen[:2] == b"\xff\xd8" and screen[-2:] == b"\xff\xd9"
            print("PASS original, thumbnail, screen copy; original SHA256", hashlib.sha256(original).hexdigest())
        if catalog["chats"]:
            json.loads(exported(connection, "Jchat," + str(catalog["chats"][0])))
            print("PASS chat export")
        if catalog["answers"]:
            exported(connection, "Janswer," + str(catalog["answers"][0])).decode("utf-8")
            print("PASS answer export")
        logs = connection.logs()
        assert identity["token"] not in logs and "LINK_IDENTITY " not in logs
        print("PASS logs contain no pairing token")
    finally:
        connection.close()
    server.WIRELESS_IDENTITY = args.identity
    server.WIRELESS_HOST = args.host
    try:
        result = server.exchange("f", screen=True)
        assert result["width"] == 240 and result["height"] == 284
        print("PASS wireless screen checksum", result["checksum"])
    finally:
        if server.CONNECTION:
            server.CONNECTION.close()
    # System CA trust must reject the device's self-signed certificate. The bridge
    # works only because pairing explicitly trusts that individual certificate.
    url = urlsplit(args.host or identity["url"])
    untrusted = http.client.HTTPSConnection(url.hostname, url.port, context=ssl.create_default_context(), timeout=10)
    try:
        try:
            untrusted.request("GET", "/logs")
        except ssl.SSLCertVerificationError:
            print("PASS unpaired TLS identity rejected")
        else:
            raise AssertionError("An unpaired certificate was trusted")
    finally:
        untrusted.close()


if __name__ == "__main__":
    main()
