#!/usr/bin/env python3
"""Install a bundled Z3CloudPlay launcher and compatibility kit over PS5 FTP.

This tool does NOT register applications in ShadowMount+ and does NOT claim
firmware compatibility for the included (13.60-targeted) runtime kit.
Requires Python 3.10+. Stop Z3CloudPlay before using.
"""
import argparse
import ftplib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

TITLE = "FAKE77031"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="IP address of your PS5 FTP server")
    parser.add_argument("--port", type=int, default=2120)
    parser.add_argument("--kit-only", action="store_true", help="Leave the existing launcher untouched")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    archive = root / "playden-kit-h-r8.zip"
    package = root / TITLE
    uploader = root / "console_kit_upload.py"
    if not archive.is_file() or not uploader.is_file() or (not args.kit_only and not package.is_dir()):
        parser.error("bundle is incomplete: missing kit archive, uploader or launcher folder")
    print("WARNING: kit H r8 targets firmware 13.60; firmware 11.00 is UNTESTED.")
    print("Stop Z3CloudPlay and back up its data. This script will NOT register a title with ShadowMount+.")
    response = input("Type EXPERIMENTAL to proceed: ").strip()
    if response != "EXPERIMENTAL":
        print("Cancelled.")
        return 1
    with tempfile.TemporaryDirectory(prefix="z3cloudplay-kit-") as tmp:
        base = Path(tmp)
        with zipfile.ZipFile(archive) as z:
            manifests = [i for i in z.infolist() if i.filename.endswith("/manifest.json") or i.filename == "manifest.json"]
            if len(manifests) != 1:
                raise ValueError(f"Expected one kit manifest; found {len(manifests)}")
            prefix = manifests[0].filename.removesuffix("manifest.json")
            manifest = json.loads(z.read(manifests[0]))
            if manifest.get("kit_id") != "hl2-fw1360-dev1":
                raise ValueError("Unexpected upstream kit identity")
            for item in z.infolist():
                name = item.filename
                if not name.startswith(prefix):
                    continue
                rel = name[len(prefix):]
                if not rel:
                    continue
                candidate = Path(rel)
                if candidate.is_absolute() or ".." in candidate.parts:
                    raise ValueError(f"Unsafe archive path: {name}")
                dest = base / manifest["kit_id"] / candidate
                if item.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                else:
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(item) as src, dest.open("wb") as dst:
                        import shutil
                        shutil.copyfileobj(src, dst)
                    if rel != "manifest.json":
                        files = {f["path"]: f for f in manifest.get("files", [])}
                        if rel in files:
                            dest.chmod(int(str(files[rel]["mode"]), 8))
        command = [sys.executable, str(uploader), str(base / manifest["kit_id"]),
                   "--host", args.host, "--port", str(args.port)]
        subprocess.run(command, check=True)
    if args.kit_only:
        print("Kit uploaded. Restart Z3CloudPlay; check runtime.log before any execution attempt.")
        return 0
    print("Uploading launcher files. Existing title must already be registered with ShadowMount+.")
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=60)
    ftp.login()
    remote_root = f"/data/homebrew/{TITLE}"
    try:
        ftp.cwd(remote_root)
    except ftplib.all_errors as error:
        ftp.quit()
        raise RuntimeError("Launcher title folder absent: register through ShadowMount+ first") from error
    try:
        for path in sorted(package.rglob("*")):
            relative = path.relative_to(package).as_posix()
            remote = f"{remote_root}/{relative}"
            if path.is_dir():
                try:
                    ftp.mkd(remote)
                except ftplib.error_perm:
                    pass
            else:
                with path.open("rb") as stream:
                    ftp.storbinary(f"STOR {remote}", stream, blocksize=1 << 20)
    finally:
        ftp.quit()
    print("Uploaded kit and launcher. Reopen Z3CloudPlay and inspect runtime.log.")
    print("NOTE: Kit admission does not prove runtime support on PS5 firmware 11.00.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError, ftplib.all_errors) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
