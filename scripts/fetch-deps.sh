#!/usr/bin/env bash
# Download the prebuilt toolchain pieces the PS5 build uses into deps/, checking every archive and
# the files the build links against the SHA-256 values in dependencies.lock.json.
# Safe to re-run: a component whose stamp matches its pinned hash is skipped.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps=${Z3CLOUDPLAY_DEPS_DIR:-"$root/deps"}
downloads="$deps/downloads"
mkdir -p "$downloads"

lock() { python3 -c 'import json,sys
value=json.load(open(sys.argv[1]))
for key in sys.argv[2:]: value=value[key]
print(value)' "$root/dependencies.lock.json" "$@"; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

# fetch <url> <sha256> <file name>: download once into deps/downloads and verify.
fetch() {
    local url=$1 expected=$2 file="$downloads/$3"
    if [[ ! -f $file || $(sha256 "$file") != "$expected" ]]; then
        echo "Downloading $url"
        if ! curl -fsSL --retry 3 -o "$file.part" "$url"; then
            rm -f "$file.part"
            echo "Dependency download failed: $url" >&2
            echo "Supply the SHA-256 pinned archive at $file; unverified substitutes are rejected." >&2
            exit 1
        fi
        local downloaded_hash; downloaded_hash=$(sha256 "$file.part")
        if [[ "$downloaded_hash" != "$expected" ]]; then
            rm -f "$file.part"
            echo "Downloaded dependency SHA-256 mismatch: $url" >&2
            exit 1
        fi
        mv "$file.part" "$file"
    fi
    local actual; actual=$(sha256 "$file")
    [[ $actual == "$expected" ]] || { echo "$3: SHA-256 $actual, expected $expected" >&2; exit 1; }
}

# fresh <directory> <stamp>: true when the directory was unpacked from the archive with this hash.
fresh() { [[ -f $1/.z3cloudplay-source-sha256 && $(<"$1/.z3cloudplay-source-sha256") == "$2" ]]; }
stamp() { printf '%s\n' "$2" > "$1/.z3cloudplay-source-sha256"; }

# unpack_sdk <directory> <url> <sha256> <archive name>: one ps5-payload-dev SDK release.
unpack_sdk() {
    local target=$1 url=$2 expected=$3 name=$4
    fresh "$target" "$expected" && return
    fetch "$url" "$expected" "$name"
    local work; work=$(mktemp -d "$deps/.unpack.XXXXXX")
    unzip -q "$downloads/$name" -d "$work"
    rm -rf "$target"
    mv "$work/ps5-payload-sdk" "$target"
    rmdir "$work"
    stamp "$target" "$expected"
    echo "Ready: $target"
}

unpack_sdk "$deps/ps5-payload-sdk" "$(lock ps5_payload_sdk url)" "$(lock ps5_payload_sdk sha256)" \
    "ps5-payload-sdk-$(lock ps5_payload_sdk version).zip"
# The Lapy helper is built against the older SDK its foundation pins.
unpack_sdk "$deps/ps5-payload-sdk-helper" "$(lock prospero_launcher_helper payload_sdk url)" \
    "$(lock prospero_launcher_helper payload_sdk sha256)" \
    "ps5-payload-sdk-$(lock prospero_launcher_helper payload_sdk version).zip"

gl_version=$(lock ps5_opengl version)
gl_sha=$(lock ps5_opengl sdk_sha256)
if ! fresh "$deps/ps5-opengl" "$gl_sha"; then
    fetch "$(lock ps5_opengl sdk_url)" "$gl_sha" "ps5-opengl-sdk-$gl_version.tar.gz"
    rm -rf "$deps/ps5-opengl"
    mkdir -p "$deps/ps5-opengl"
    tar -xzf "$downloads/ps5-opengl-sdk-$gl_version.tar.gz" -C "$deps/ps5-opengl"
    stamp "$deps/ps5-opengl" "$gl_sha"
fi
library="$deps/ps5-opengl/ps5-opengl-sdk-$gl_version/sdk/lib/libPS5OpenGL.a"
[[ $(sha256 "$library") == "$(lock ps5_opengl libPS5OpenGL_sha256)" ]] || { echo "libPS5OpenGL.a hash mismatch" >&2; exit 1; }
echo "Ready: $deps/ps5-opengl"

rt_sha=$(lock compiler_rt_builtins archive_sha256)
if ! fresh "$deps/compiler-rt" "$rt_sha"; then
    fetch "$(lock compiler_rt_builtins url)" "$rt_sha" "libclang-rt-18-dev.deb"
    work=$(mktemp -d "$deps/.unpack.XXXXXX")
    member=$(lock compiler_rt_builtins member)
    # macOS (bsd)tar reads the .deb (ar) container directly; GNU tar can't, so use ar there.
    if [[ $(uname) == Darwin ]]; then
        tar -xf "$downloads/libclang-rt-18-dev.deb" -C "$work" data.tar.zst
    else
        (cd "$work" && ar x "$downloads/libclang-rt-18-dev.deb" data.tar.zst)
    fi
    tar -xf "$work/data.tar.zst" -C "$work" "./$member"
    rm -rf "$deps/compiler-rt"
    mkdir -p "$deps/compiler-rt"
    mv "$work/$member" "$deps/compiler-rt/"
    rm -rf "$work"
    stamp "$deps/compiler-rt" "$rt_sha"
fi
[[ $(sha256 "$deps/compiler-rt/libclang_rt.builtins-x86_64.a") == "$(lock compiler_rt_builtins sha256)" ]] \
    || { echo "compiler-rt builtins hash mismatch" >&2; exit 1; }
echo "Ready: $deps/compiler-rt"
