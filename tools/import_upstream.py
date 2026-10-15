#!/usr/bin/env python3
"""Import upstream PlayDen code and apply z3cloudplay textual branding.
Preserves third-party licenses and upstream attribution.
Does not claim to encrypt Steam session tokens.
"""
from pathlib import Path
import shutil, sys
source=Path(sys.argv[1]).resolve()
dest=Path(sys.argv[2]).resolve()
if not (source/"CMakeLists.txt").is_file(): raise SystemExit("Source not present")
for item in source.iterdir():
    if item.name==".git": continue
    target=dest/item.name
    if item.is_dir():
        shutil.copytree(item,target,symlinks=True,dirs_exist_ok=True)
    else: shutil.copy2(item,target)
# Modify first-party code and docs only. Do not rewrite third party or fixtures.
skip={"third_party","fixtures","runtime","assets"}
suffixes={".cpp",".cc",".c",".h",".hpp",".cmake",".md",".json",".txt",".yml",".yaml",".py",".sh",".in"}
for p in dest.rglob("*"):
    if not p.is_file() or any(part in skip for part in p.relative_to(dest).parts): continue
    if p.suffix.lower() not in suffixes and p.name not in {"CMakeLists.txt","CMakePresets.json",".gitmodules"}: continue
    try: contents=p.read_text(encoding="utf-8")
    except (UnicodeDecodeError,OSError): continue
    modified=contents.replace("Z3CLOUDPLAY","Z3CLOUDPLAY").replace("Z3CloudPlay","Z3CloudPlay").replace("z3cloudplay","z3cloudplay")
    if modified != contents: p.write_text(modified,encoding="utf-8")
readme=dest/"README.md"
body=readme.read_text()
body=body.replace("# Z3CloudPlay for PS5","# z3cloudplay for PS5")
readme.write_text("""<!-- z3cloudplay is derived from leolobato/z3cloudplay-ps5 (GPL-3.0).
Original project: https://github.com/leolobato/z3cloudplay-ps5
This is an independent, unofficial adaptation, not endorsed by Valve or the original author.
IMPORTANT: session encryption remains pending; current imported session data is NOT encrypted.
-->
""" + body)
print("Imported upstream source with textual rebranding. Encryption NOT implemented.")
