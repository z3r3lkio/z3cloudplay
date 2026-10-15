# LZMA SDK (LZMA1 decoder only)

- Source: https://github.com/ip7z/7zip, tag `24.09`, directory `C/`
  (fetched as `https://raw.githubusercontent.com/ip7z/7zip/24.09/C/<file>`)
- License: public domain (see LICENSE)
- Copied files (unmodified) and SHA-256:
  - `LzmaDec.c`  b9ca2b8707400347c75d6fef288d1354a3b4cf93e8da42f26207b8bd4c4d5e59
  - `LzmaDec.h`  3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266
  - `7zTypes.h`  5de943c886d5d7bbcf16203e9b292a9e7be327b43fe3dfd9f4ad07d6d0870ef6
  - `Precomp.h`  fea249c753dfbbdd69d9397e91497205452b784279f70cad4658dc567cd2f85f
  - `Compiler.h` 959dac87ac7109502f6467e910d0722b932176e243153bf8c663479aa8c29b20
- No other headers are needed (`CpuArch.h` is only referenced in a comment).
- Used by `src/steam/backend/depot_chunk.cpp` for Steam "VZ" (LZMA1) depot chunks.
