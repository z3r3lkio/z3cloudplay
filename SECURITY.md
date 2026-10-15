# z3cloudplay security status

**Status: development snapshot; NOT READY for use with a valuable Steam account.**

The legacy `SessionStore` still contains code capable of persisting Steam credentials in cleartext as `session.v1`. The PS5 entry point now uses an ephemeral in-memory repository for all its Steam service instances, and attempts to remove legacy files at startup. This change has not been verified on PS5 hardware.

## Implementation progress (October 15, 2026)

A standalone vault has been added under `src/steam/secure_session_vault.{h,cpp}`, with AES-256-GCM, randomly generated salts and nonces, PBKDF2-HMAC-SHA256 (600,000 rounds), authenticated metadata, and generic error responses. A standalone test was added and registered in CMake, but it has **not yet been successfully executed or verified on PS5**. The encrypted vault is **not connected to production PS5 authentication**. The PS5 entry point instead uses RAM-only sessions; the legacy `session.v1` format remains plaintext where the old class is used.

An isolated `SecureSessionRepository` has also been added at
`src/steam/secure_session_repository.{h,cpp}`. It writes a new `session.v2`
encrypted container, has no legacy plaintext fallback, and removes legacy
session files on sign-out. Its passphrase callback must be wired to a masked
PS5 unlock UI before it replaces the production repository. Tests for this
repository passed on Linux (GitHub Actions run 38723730148).

The KDF uses PBKDF2 as a compatibility implementation, **not Argon2id**. This is a staged change, not a security release.

## Encrypted persistence candidate

Branch `feature/persistent-encrypted-sessions` connects `SecureSessionRepository`
to the PS5 entry point, adds an obscured passphrase editor, checks the
passphrase against any existing vault before allowing new authentication,
and retries session restoration after unlocking. The branch has passed
isolated Linux crypto/repository tests and static security guards.

**This is not a PS5-validated release.** The integration needs a native
PS5 build, UI/keyboard testing, and independent security review before
merging into `main`. The `main` branch continues using memory-only
sessions. Do not assume the protected UI prevents process-memory capture
or that deleting legacy files securely erases flash remnants.

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
