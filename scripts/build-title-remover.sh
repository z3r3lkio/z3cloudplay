#!/usr/bin/env bash
# Build Z3CloudPlay's one-shot title remover payload (tools/title-remover) that the PS5 package embeds.
# The build is reproducible: the source is compiled by relative name from its own folder, so the
# ELF does not depend on the checkout path. Its SHA-256 is pinned in dependencies.lock.json.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/build/title-remover"
mkdir -p "$out"
(cd "$root/tools/title-remover" && "$root/scripts/with-env.sh" bash -c \
    'exec "$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -Werror -O2 -ffile-prefix-map="$1"=. -o "$2" main.c \
         -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService' bash "$root" "$out/title-remover.elf")
echo "Title remover: $out/title-remover.elf ($(shasum -a 256 "$out/title-remover.elf" | cut -c1-16)…)"
