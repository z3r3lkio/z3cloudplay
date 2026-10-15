# z3cloudplay security status

**Status: development snapshot; NOT READY for use with a valuable Steam account.**

The imported authentication implementation currently persists Steam refresh and access tokens in cleartext in `src/steam/session_store.cpp` as `session.v1`. File permissions and checksums do not provide encryption. The repository's new name does not change this risk.

## Required changes before a security release

1. Introduce a versioned authenticated-encryption container (AES-256-GCM) with a random nonce, authenticated metadata and full verification before parsing.
2. Derive the encryption key from an independently entered passphrase with Argon2id (or a carefully parameterized supported alternative); never hard-code a key or save it beside the ciphertext.
3. Add a PS5-compatible secure unlock prompt for first sign-in and each application restart. Persist only ciphertext, KDF parameters and non-secret metadata.
4. Fail closed when encryption or unlocking is unavailable; never silently fall back to `session.v1`.
5. Migrate legacy plaintext sessions only after explicit unlock, ensure old session files and temporary copies are removed (physical flash sanitization cannot be guaranteed).
6. Add known-answer crypto tests, tamper tests, wrong-key tests, round trips, restoration/renewal tests, and platform integration tests.
7. Review sensitive process memory, logs, elevated helpers and cloud-save paths. Root-level attackers can still steal tokens while the app is unlocked.

Until these controls have been implemented and tested, avoid logging into your main Steam account on a jailbroken console.

Upstream: https://github.com/leolobato/playden-ps5 (GPL-3.0)
