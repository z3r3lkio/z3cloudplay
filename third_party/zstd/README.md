# zstd (decompression only)

- Source: https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
- Version: v1.5.6
- Tarball SHA-256: 8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
- License: BSD (the `LICENSE` file from the tarball; SHA-256
  7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8). The tarball's GPLv2 `COPYING` is not copied.
- Copied unmodified from the tarball:
  - `lib/zstd.h`, `lib/zstd_errors.h` -> `zstd.h`, `zstd_errors.h`
  - `lib/common/*` -> `common/`
  - `lib/decompress/*.c|*.h` -> `decompress/` (`huf_decompress_amd64.S` is intentionally excluded)
- Build with `ZSTD_DISABLE_ASM=1` and `ZSTD_LEGACY_SUPPORT=0`. Compiled sources: `common/{debug,entropy_common,error_private,
  fse_decompress,xxhash,zstd_common}.c` and `decompress/{huf_decompress,zstd_ddict,zstd_decompress,zstd_decompress_block}.c`
  (`common/pool.c` and `common/threading.c` are not needed without multithreading).
- Used by `src/steam/backend/depot_chunk.cpp` for Steam "VSZ" (zstd) depot chunks.
