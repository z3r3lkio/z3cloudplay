#!/usr/bin/env python3
"""Check the frozen common package without rewriting it."""
import hashlib
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
package = root / "fixtures/ui-comparison"
frozen_commit = "2d161059540c1c73c61cfb2ffebb8a0e4efcecd1"
failures = []
checked = 0
for digest, name in re.findall(r"^([0-9a-f]{64})  (.+)$", (package / "README.md").read_text(), re.M):
    actual = hashlib.sha256((package / name).read_bytes()).hexdigest()
    if digest != actual:
        failures.append(f"Hash mismatch: {name}")
    checked += 1
for name in subprocess.check_output(["git", "ls-tree", "-r", "--name-only", frozen_commit, "fixtures/ui-comparison"], cwd=root, text=True).splitlines():
    frozen = subprocess.check_output(["git", "show", f"{frozen_commit}:{name}"], cwd=root)
    if frozen != (root / name).read_bytes():
        failures.append(f"Frozen commit differs: {name}")
if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print(f"PASS: {checked} hashes; all package files identical to Agent A's {frozen_commit}")
