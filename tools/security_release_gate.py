#!/usr/bin/env python3
"""Security release gate. A failing gate means the project is not ready for release."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
session=(root/"src/steam/session_store.cpp").read_text()
ps5=(root/"src/homebrew/ps5.cpp").read_text()
checks={
    "No plaintext session.v1 write": 'WriteAtomicPrivate(kPath, encoded)' not in session,
    "Secure vault wired into live PS5 session repository": (
        'secure_session_vault' in session or 'SecureSessionRepository' in ps5
    ),
    "Passphrase unlock connected to PS5 user interface": 'SecureSessionUnlock' in ps5,
}
for label, ok in checks.items():
    print(f"{'PASS' if ok else 'BLOCKED'}: {label}")
if not all(checks.values()):
    print("SECURITY RELEASE BLOCKED: z3cloudplay must not be released as credential-safe.")
    sys.exit(1)
