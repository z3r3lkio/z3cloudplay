#!/usr/bin/env bash
# Kit H's any-title shell (title-any-id, game-root-unowned) built from the prospero-win and Lapy submodule HEADs
# (prospero-win feat/upstream-2026-10-10). Same steps as kit-c/scripts/make_kit_unowned.sh, without packaging.
# Z3CLOUDPLAY_KIT_DIR: where the kit is staged (default build/kit-h). PS5_NATIVE_FOUNDATION: the prospero-foundation checkout
# (default third_party/prospero-foundation).
set -euo pipefail
R=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
here=${Z3CLOUDPLAY_KIT_DIR:-$R/build/kit-h}
foundation=${PS5_NATIVE_FOUNDATION:-$R/third_party/prospero-foundation}
pw=$R/third_party/prospero-win
lapy=$R/third_party/PS5-Lapy-JB-Daemon
rev=$(git -C "$pw" rev-parse HEAD)
[[ -z $(git -C "$pw" status --porcelain) && -z $(git -C "$lapy" status --porcelain) ]] || { echo "dirty worktree" >&2; exit 2; }
title=$here/inputs/titles/any-${rev:0:7}
[[ ! -e $title ]] || { echo "$title exists" >&2; exit 2; }
mkdir -p "$here/inputs/titles" "$here/logs"
(cd "$lapy" && PS5_PAYLOAD_SDK=$R/deps/ps5-payload-sdk-helper "$R/scripts/with-env.sh" python3 tools/build_owned_daemon.py \
    --sdk "$R/deps/ps5-payload-sdk-helper" --logging-client "$pw/native/ps5log" --elf-helper \
    --title-rule FAKE78 --allow-list /data/z3cloudplay/title-rule/allow-list) > "$here/logs/helper.log"
helper=$lapy/build/owned_root_helper-FAKE78-rule
(cd "$pw" && PS5_NATIVE_FOUNDATION=$foundation \
    PW_FOUNDATION_READY=1 PW_ANY_TITLE=1 PW_TITLE_ID=FAKE78000 PW_OUTPUT_SUFFIX=-kit-h \
    PW_LAPY_HELPER_ELF=$helper/lapy.elf PW_LAPY_HELPER_MANIFEST=$helper/lapy-manifest.json \
    "$R/scripts/with-env.sh" bash tools/build_native.sh) > "$here/logs/title.log" 2>&1
cp -R "$pw/dist/FAKE78000-kit-h" "$title"
cp "$lapy/source/lapy_elevation_protocol.h" "$here/inputs/"
echo "shell $title"
