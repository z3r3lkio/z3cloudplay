# z3cloudplay credential migration policy

Production PS5 authentication currently uses an **ephemeral RAM-only** credential repository.

At startup, `src/homebrew/ps5.cpp` removes `session.v1` and `session.v1.tmp`, failing startup if removal fails. This prevents application-level reuse of legacy plaintext Steam tokens. Deletion does **not** guarantee physical sanitization of historical flash blocks, backups or snapshots.

The encrypted `session.v2` repository is compiled and separately tested, but not yet wired to the live UI. It requires an input surface that masks passphrases and avoids saving them in diagnostic state. Reusing the visible generic text editor would be unsafe.

Steam QR sign-in is required after restart while ephemeral storage is selected. To release persistent encrypted credentials, complete the PS5 masked unlock flow, integrate it with all Steam services, add migration tests and build/test a PS5 package.

## Checklist
- [x] Encryption primitive tests
- [x] Encrypted repository round-trip tests
- [x] Memory-only repository at the PS5 entry point
- [x] Fail-closed legacy session file removal at startup (source change)
- [ ] Masked PS5 credential UI and integration
- [ ] Build, sign and run PS5 PKG on target hardware
- [ ] Review product artwork and package identity
