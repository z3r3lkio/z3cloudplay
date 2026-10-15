#pragma once
#include <cstdint>
#include <string>
#include "platform/storage.h"

namespace z3cloudplay::security {

// Versioned, passphrase-protected authenticated container.
// No plaintext, passphrase or derived key is persisted by these routines.
// PBKDF2-HMAC-SHA256 is used for portability with the pinned Mbed TLS build.
// A future Argon2id implementation requires a different KDF identifier.
Result<Bytes> SealSession(const Bytes& plaintext, const std::string& passphrase);
Result<Bytes> OpenSession(const Bytes& sealed, const std::string& passphrase);
}  // namespace z3cloudplay::security
