#!/bin/sh
# Builds runtime/steamworks/steam.dll from steam.c (needs i686-w64-mingw32-gcc). Reproducible:
# no timestamp, stripped; update kLegacySteamSha256 in src/runtime/steamworks.h after a change.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../runtime/steamworks/steam.dll"
i686-w64-mingw32-gcc -O2 -ffreestanding -fno-tree-loop-distribute-patterns -shared -nostdlib -s -Wl,--no-insert-timestamp -Wl,--enable-stdcall-fixup \
  -Wl,-e,_DllMain@12 -o "$out" "$here/steam.c" "$here/steam.def"
shasum -a 256 "$out"
