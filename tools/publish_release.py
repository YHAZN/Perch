"""Upload a verified release bundle to GitHub, keeping it a draft unless --publish is set."""

import argparse
import hashlib
import json
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", help="owner/repository")
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--notes", default="Software update.", help="Plain-text release notes")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        raise SystemExit("Invalid repository name")
    manifest_path = ROOT / "dist" / "perch.json"
    manifest = json.loads(manifest_path.read_text())
    version = manifest["version"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise SystemExit("Invalid release version")
    filename = f"perch-{version}.bin"
    image = ROOT / "dist" / filename
    data = image.read_bytes()
    expected_url = f"https://github.com/{args.repository}/releases/download/v{version}/{filename}"
    if (manifest["url"] != expected_url or len(data) != manifest["size"] or
            hashlib.sha256(data).hexdigest() != manifest["sha256"] or not manifest.get("signature")):
        raise SystemExit("Bundle does not match the signed manifest")
    subprocess.run([os.sys.executable, str(ROOT / "tools" / "check_release.py"), str(image)], check=True)
    # Reuse the repository's credential helper without logging credentials or headers.
    result = subprocess.run(
        ["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
        capture_output=True, text=True, check=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "Never"},
    )
    credential = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    token = credential.get("password")
    if not token:
        raise SystemExit("No GitHub credential available")

    def request(url, method="GET", body=None, mime="application/json"):
        headers = {"Authorization": "Bearer " + token, "Accept": "application/vnd.github+json"}
        if isinstance(body, dict):
            body = json.dumps(body).encode()
        if body is not None:
            headers["Content-Type"] = mime
        req = urllib.request.Request(url, data=body, headers=headers, method=method)
        with urllib.request.urlopen(req, timeout=120) as response:
            return json.load(response)

    api = f"https://api.github.com/repos/{args.repository}/releases"
    try:
        request(api + "/tags/v" + version)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    else:
        raise SystemExit("Release already exists; refusing to replace its assets")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    release = request(api, "POST", {
        "tag_name": "v" + version, "target_commitish": revision, "name": version, "draft": True,
        "body": args.notes,
    })
    upload_url = release["upload_url"].split("{", 1)[0]
    for path, mime in ((image, "application/octet-stream"), (manifest_path, "application/json")):
        request(upload_url + "?name=" + urllib.parse.quote(path.name), "POST", path.read_bytes(), mime)
        print("Uploaded", path.name)
    if args.publish:
        release = request(api + "/" + str(release["id"]), "PATCH", {"draft": False})
    print("Published" if args.publish else "Draft", release["html_url"])


if __name__ == "__main__":
    main()
