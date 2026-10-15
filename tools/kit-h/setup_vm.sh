#!/usr/bin/env bash
# Kit H: a fresh prospero-win checkout of feat/upstream-2026-10-10 (rebased onto upstream ce890c0) on the Linux VM,
# sharing kit D's Wine source and title foundation. Run on the Linux VM.
# PROSPERO_WIN_SOURCE: the prospero-win repository to clone (default: the Z3CloudPlay fork on GitHub).
set -euxo pipefail
task=${Z3CLOUDPLAY_KIT_WORK:-$HOME/z3cloudplay-kit}
source=${PROSPERO_WIN_SOURCE:-https://github.com/leolobato/prospero-win.git}
root=$task/kit-h/source
mkdir -p "$task/kit-h/logs"
[ -d "$root/.git" ] || git clone -q --no-checkout "$source" "$root"
git -C "$root" fetch -q "$source" bceed2d2f9400cca6a6da30d6088fab1b12cac50
git -C "$root" checkout -q --force bceed2d2f9400cca6a6da30d6088fab1b12cac50
mkdir -p "$root/.deps"
ln -sfn "$task/source/.deps/wine" "$root/.deps/wine"
ln -sfn "$task/source/.deps/ps5-native-app-boilerplate" "$root/.deps/ps5-native-app-boilerplate"
git -C "$root" log --oneline -1
