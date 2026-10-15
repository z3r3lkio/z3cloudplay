#!/usr/bin/env bash
# Package the selected direct Hui consumer locally, without console discovery, upload or launch.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
environment=${Z3CLOUDPLAY_PS5_ENV:-"$root/scripts/with-env.sh"}
deps=${Z3CLOUDPLAY_PS5_DEPS_DIR:-"$root/deps"}
hui=${Z3CLOUDPLAY_HUI_SOURCE_DIR:-"$root/third_party/ps5-homebrew-ui"}
simulation=${Z3CLOUDPLAY_UI_SIMULATION:-OFF}
native_access=${Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS:-ON}
prospero_source=${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR:-"$root/third_party/prospero-win"}
helper_build="$root/third_party/PS5-Lapy-JB-Daemon/build/owned_root_helper-FAKE77031"
lapy_helper=${Z3CLOUDPLAY_LAPY_HELPER_ELF:-"$helper_build/lapy.elf"}
lapy_manifest=${Z3CLOUDPLAY_LAPY_HELPER_MANIFEST:-"$helper_build/lapy-manifest.json"}
title_remover=${Z3CLOUDPLAY_TITLE_REMOVER_ELF:-"$root/build/title-remover/title-remover.elf"}
case "$simulation" in
 ON|on|1|true) simulation=ON ;;
 OFF|off|0|false|'') simulation=OFF ;;
 *) echo "Z3CLOUDPLAY_UI_SIMULATION must be ON or OFF" >&2; exit 2 ;;
esac
case "$native_access" in
 ON|on|1|true) native_access=ON ;;
 OFF|off|0|false) native_access=OFF ;;
 *) echo "Z3CLOUDPLAY_NATIVE_RUNTIME_ACCESS must be ON or OFF" >&2; exit 2 ;;
esac
[[ -f "$environment" ]] || { echo "Missing explicit PS5 environment: $environment" >&2; exit 1; }
# PS5 CMake consumes the pre-baked atlas directory directly. Refresh it from the pinned submodule
# so catalog, font, license or baker changes cannot leave stale package inputs.
Z3CLOUDPLAY_HUI_SOURCE_DIR="$hui" "$root/scripts/build-homebrew.sh" --prepare-fonts-only
if [[ $native_access == ON && -z ${Z3CLOUDPLAY_TITLE_REMOVER_ELF:-} ]]; then
 "$root/scripts/build-title-remover.sh"
fi
if [[ ! -f "$root/scripts/network-deps/.deps/ps5/lib/libcurl.a" ]]; then
 "$root/scripts/network-deps/prepare-ps5-deps.sh"
fi
bash "$environment" bash -c 'exec "$PS5_PAYLOAD_SDK/bin/prospero-cmake" "$@"' bash \
 -G Ninja -S "$root" -B "$root/build/homebrew-ps5" -DCMAKE_BUILD_TYPE=Release \
 -DZ3CLOUDPLAY_PLATFORM_PS5=ON -DZ3CLOUDPLAY_PS5_TITLE_ID=FAKE77031 \
 -DZ3CLOUDPLAY_UI_SIMULATION="$simulation" \
 -DZ3CLOUDPLAY_NATIVE_RUNTIME_ACCESS="$native_access" -DZ3CLOUDPLAY_PROSPERO_SOURCE_DIR="$prospero_source" \
 -DZ3CLOUDPLAY_LAPY_HELPER_ELF="$lapy_helper" -DZ3CLOUDPLAY_LAPY_HELPER_MANIFEST="$lapy_manifest" \
 -DZ3CLOUDPLAY_TITLE_REMOVER_ELF="$title_remover" \
 -DZ3CLOUDPLAY_PS5_DEPS_DIR="$deps" -DZ3CLOUDPLAY_HUI_SOURCE_DIR="$hui"
bash "$environment" cmake --build "$root/build/homebrew-ps5" --target z3cloudplay-homebrew-ps5-package
echo "Selected Hui package (not deployed): $root/build/homebrew-ps5/dist/FAKE77031"
