#!/usr/bin/env bash
# Explicit offline packaging only: no Wine build, downloads or console access.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${Z3CLOUDPLAY_PROSPERO_SOURCE_DIR:-"$root/third_party/prospero-win"}
python_bin=${Z3CLOUDPLAY_PYTHON:-python3}
source_args=(--source "$source_dir")
release=0
if [[ ${1:-} == --release ]]; then
 source_args+=(--release)
 release=1
 shift
fi
# Development kits may carry compiled host build paths; a release kit never does.
for argument in "$@"; do
 if [[ $release == 1 && $argument == --development-build-paths ]]; then
  echo 'A release kit cannot use --development-build-paths.' >&2
  exit 2
 fi
done
if [[ $# -eq 0 ]]; then
 echo 'Usage: build-prospero-kit.sh [--release] package --spec SPEC --out KIT' >&2
 echo '       build-prospero-kit.sh [--release] validate KIT' >&2
 exit 2
fi
"$python_bin" "$root/tools/prospero_source.py" "${source_args[@]}"
exec "$python_bin" "$root/tools/package_prospero_kit.py" --source "$source_dir" "$@"
