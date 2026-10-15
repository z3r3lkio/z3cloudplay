#!/usr/bin/env bash
# Kit H: host (PE) Wine 490f6d5 + the series of prospero-win feat/upstream-2026-10-10, as tools/build_host_wine.sh including
# its wine/ps5 staging, configured with the prior neutral build's flags, through prefix-map wrappers. Same as kit D otherwise.
set -euxo pipefail
task=${Z3CLOUDPLAY_KIT_WORK:-$HOME/z3cloudplay-kit}
root=$task/kit-h/source
logs=$task/kit-h/logs
export PATH=$task/wrappers:$PATH LLVM_CONFIG=/usr/bin/llvm-config-18 LC_ALL=C TZ=UTC
work=$root/.deps/wine-host
tree=$work/source
commit=$(sed -n 's/^WINE_COMMIT=//p' "$root/tools/build_wine_ps5.sh")
ordered=$(sh "$root/tools/build_wine_ps5.sh" --check-patches)
mkdir -p "$work"
[ -d "$tree/.git" ] || git clone -q --no-checkout "$root/.deps/wine/source" "$tree"
git -C "$tree" checkout -q --force "$commit"
git -C "$tree" clean -q -fdx
for patch in $ordered; do git -C "$tree" apply --index "$root/wine/patches/$patch"; done
python3 "$root/tools/stage_vk_batch.py" --source "$tree" --repo "$root"
cp "$root/wine/ps5/time/pw_qpc_clock.h" "$tree/dlls/ntdll/pw_qpc_clock.h"
cp "$root/wine/ps5/input/pw_key_shared.h" "$tree/dlls/win32u/pw_key_shared.h"
build=$work/build
rm -rf "$build"; mkdir -p "$build"; cd "$build"
"$tree/configure" --prefix=/usr --enable-archs=i386,x86_64 --disable-tests \
    CFLAGS='-g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' CXXFLAGS='-g -O2' > "$logs/host-configure.log" 2>&1
make -j14 > "$logs/host-make.log" 2>&1
rm -rf "$work/install"
make -j14 install DESTDIR="$work/install" > "$logs/host-install.log" 2>&1
lib=$work/install/usr/lib/wine
find "$lib/i386-windows" -type f -exec /usr/bin/i686-w64-mingw32-strip -s {} +
find "$lib/x86_64-windows" -type f -exec /usr/bin/x86_64-w64-mingw32-strip -s {} +
echo 'HOST-DONE'
