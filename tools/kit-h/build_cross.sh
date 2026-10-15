#!/usr/bin/env bash
# Kit H: PS5 cross Wine + PRX link (tools/build_wine_ps5.sh, no OpenGL SDK: upstream #465 removed that path), wowprospero
# (tools/build_wowprospero.sh) and the native WoW64 CPU (tools/build_wow64native.sh, host DLL + PS5 PRX), all from
# prospero-win feat/upstream-2026-10-10. RADV: PS5_Vulkan main 8aee3af (PS5_Mesa 01d7057, includes f2ee389: a
# swapchain that never presented gives the display to the next, which SDL/wined3d GL probe windows need under Zink).
set -euxo pipefail
task=${Z3CLOUDPLAY_KIT_WORK:-$HOME/z3cloudplay-kit}
root=$task/kit-h/source
logs=$task/kit-h/logs
out=$task/kit-h
export PATH=$task/wrappers:$PATH LLVM_CONFIG=/usr/bin/llvm-config-18 TZ=UTC
export LC_ALL=en_US.UTF-8
: "${PROSPERO_FREETYPE_TARBALL:?set it to freetype-2.13.3.tar.xz}"
export PROSPERO_FREETYPE_TARBALL
sdk=$root/.deps/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk
cd "$root"
# Upstream #388: schannel through a static GnuTLS (libgnutls.prx) and a pinned CA bundle.
PS5_PAYLOAD_SDK=$sdk PROSPERO_WINE_PS5_WORK=$out/wine-ps5 sh tools/build_tls_ps5.sh --jobs 14 > "$logs/tls-build.log" 2>&1
sh tools/build_wine_ps5.sh --source "$root/.deps/wine/source" \
    --host-tools "$root/.deps/wine-host/build" --foundation "$root/.deps/ps5-native-app-boilerplate" \
    --prx-foundation "$task/prx-foundation" --radv "$task/radv-g" --jobs 14 \
    --work "$out/wine-ps5" > "$logs/cross-build.log" 2>&1
sh tools/build_wowprospero.sh --build "$root/.deps/wine-host/build" --source "$root/.deps/wine-host/source" --jobs 14 \
    > "$logs/wowprospero-build.log" 2>&1
stubs=$out/wow64native-stubs
rm -rf "$stubs"; mkdir -p "$stubs"
cp "$sdk"/target/lib/*.so "$stubs/"
rm -f "$stubs/libkernel_web.so" "$stubs/libScePosixForWebKit.so"
rm -rf "$out/wow64native"
sh tools/build_wow64native.sh --source "$root/.deps/wine-host/source" --build "$root/.deps/wine-host/build" \
    --output "$out/wow64native" --ps5-sdk "$sdk" --ps5-wine "$out/wine-ps5" \
    --ps5-ntdll "$out/wine-ps5/prx/ntdll.shared.elf" --ps5-foundation "$task/prx-foundation" \
    --ps5-stubs "$stubs" > "$logs/wow64native-build.log" 2>&1
echo 'CROSS-DONE'
