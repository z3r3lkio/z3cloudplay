#!/usr/bin/env bash
# Build the title-scoped Lapy root helper that the PS5 package embeds, from the pinned submodule and the
# helper's own SDK (see prospero_launcher_helper in dependencies.lock.json). Usage: build-lapy-helper.sh [TITLE]
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
title=${1:-FAKE77031}
lapy="$root/third_party/PS5-Lapy-JB-Daemon"
sdk=${Z3CLOUDPLAY_HELPER_SDK:-"$root/deps/ps5-payload-sdk-helper"}
[[ -f "$lapy/tools/build_owned_daemon.py" ]] || { echo "Missing submodule: git submodule update --init third_party/PS5-Lapy-JB-Daemon" >&2; exit 1; }
[[ -x "$sdk/bin/prospero-clang" ]] || { echo "Missing helper SDK at $sdk: run scripts/fetch-deps.sh" >&2; exit 1; }
(cd "$lapy" && PS5_PAYLOAD_SDK="$sdk" "$root/scripts/with-env.sh" python3 tools/build_owned_daemon.py --sdk "$sdk" \
    --logging-client "$root/third_party/prospero-win/native/ps5log" --elf-helper --title "$title")
out="$lapy/build/owned_root_helper-$title"
echo "Helper: $out/lapy.elf ($(shasum -a 256 "$out/lapy.elf" | cut -c1-16)…)"
