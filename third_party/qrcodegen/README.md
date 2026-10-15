Nayuki QR Code generator, C implementation.

- Upstream: https://github.com/nayuki/QR-Code-generator
- Pinned commit: `3c6d0b3cefb4e049dc337e82237c9644399716a8`
- Files: `c/qrcodegen.h`, `c/qrcodegen.c`
- License: MIT; see `LICENSE`.

The C implementation is used because it has no locale, stream, filesystem or
platform dependencies and therefore also fits the PS5 libc subset.
