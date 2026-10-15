#!/usr/bin/env bash
# Run a command with the PS5 toolchain environment: LLVM 18 first on PATH and PS5_PAYLOAD_SDK set to the
# SDK that scripts/fetch-deps.sh unpacked (override either variable to use another copy).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$root/deps/ps5-payload-sdk}"
if [[ -z "${LLVM_CONFIG:-}" ]]; then
    if command -v brew >/dev/null 2>&1; then
        LLVM_CONFIG="$(brew --prefix llvm@18)/bin/llvm-config"
    else
        LLVM_CONFIG=llvm-config-18
    fi
fi
export LLVM_CONFIG
command -v "$LLVM_CONFIG" >/dev/null 2>&1 || { echo 'Missing LLVM 18: brew install llvm@18, or set LLVM_CONFIG.' >&2; exit 1; }
# The SDK's wrappers resolve LLVM_CONFIG with readlink -f, which turns a bare name into a path in the current directory.
LLVM_CONFIG="$(command -v "$LLVM_CONFIG")"
[[ -x "$PS5_PAYLOAD_SDK/bin/prospero-clang" ]] || { echo "Missing PS5 payload SDK at $PS5_PAYLOAD_SDK: run scripts/fetch-deps.sh." >&2; exit 1; }
export PATH="$("$LLVM_CONFIG" --bindir):$PATH"
exec "$@"
