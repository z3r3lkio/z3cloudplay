# Kit H build scripts (record)

These are the scripts that built runtime kit H, the kit this release uses. They are kept as a record. **not** a turnkey build yet: read this first.

## What they need

- **A Linux x86-64 machine or VM** (the maintainer uses an Ubuntu VM). Wine, Mesa and the PS5 libraries do not build
  on macOS. The kit is packaged and uploaded from the Mac.
- LLVM 18, both MinGW cross compilers (`i686-w64-mingw32`, `x86_64-w64-mingw32`), llvm-mingw for Mesa, and the PS5
  payload SDK.
- Inputs from earlier kits that are **not in this repository**: kit A's template bottle (prepared from a fresh
  Half-Life 2 install), kit F's runtime file list, kit D's Wine source checkout and the prospero-win title foundation.
- The Z3CloudPlay checkout shared between the Mac and the VM at the same path, so the VM scripts can write into its
  `build/` folder.

## Settings

The scripts read these environment variables. The defaults fit a plain checkout.

| Variable | Used by | Default |
| --- | --- | --- |
| `Z3CLOUDPLAY_KIT_WORK` | VM scripts: the build folder for Wine and its logs | `~/z3cloudplay-kit` |
| `PROSPERO_WIN_SOURCE` | `setup_vm.sh`: the prospero-win repository to clone | `https://github.com/leolobato/prospero-win.git` |
| `PROSPERO_FREETYPE_TARBALL` | `build_cross.sh`: the FreeType 2.13.3 source tarball | none, required |
| `Z3CLOUDPLAY_KIT_DIR` | `make_shell.sh`, `make_spec.py`, `stage_kit_h.py`: where kit H is staged | `build/kit-h` |
| `Z3CLOUDPLAY_KIT_FROM` | `update_r4.py`, `update_r5.py`, `update_r6.py`, `update_r8.py`: the kit whose inputs were copied (its paths in the spec are rewritten) | `build/kit-h-r3`, `build/kit-h-r4`, `build/kit-h-r5`, `build/kit-h-r6` |
| `Z3CLOUDPLAY_DOTNET_DIR` | `update_r6.py`: the prefixes before and after Wine Mono's support MSI install, and the MSI's tables | `$Z3CLOUDPLAY_KIT_DIR/dotnet` |
| `Z3CLOUDPLAY_RUNTIME_REVISION` | `update_r5.py`, `update_r8.py`: the prospero-win commit the runtime and its pin come from | none, required |
| `Z3CLOUDPLAY_CROSS_PRX` | `update_r8.py`: the cross build's `prx/sce_module` folder | `$Z3CLOUDPLAY_KIT_WORK/wine-ps5/prx/sce_module` |
| `Z3CLOUDPLAY_WINE_MONO_ARCHIVE` | `update_r4.py`: the Wine Mono 11.3.0 archive (its SHA-256 is checked) | none, required |
| `Z3CLOUDPLAY_BUILD` | `stage_kit_h.py`: the build folder that holds kits A and F | `build/` |
| `Z3CLOUDPLAY_KIT_F` | `make_spec.py`: kit F's build, whose spec kit H starts from | `build/kit-f` |
| `Z3CLOUDPLAY_MESA_ARTIFACTS` | `stage_kit_h.py`: the output of `tools/build_mesa_zink.sh` | `~/mesa-zink/work/artifacts` |
| `PS5_NATIVE_FOUNDATION` | `make_shell.sh`: the prospero-foundation checkout | `third_party/prospero-foundation` |

## What does the work

The scripts are glue. The real build steps are prospero-win's tools, in this order:

| Step | prospero-win tool | Script here |
| --- | --- | --- |
| Check out prospero-win on the VM | — | `setup_vm.sh` |
| Wine for the host (PE modules, winebuild) with the patch series | `tools/build_host_wine.sh` | `build_host.sh` |
| Wine's Unix side for the PS5, linked to RADV | `tools/build_wine_ps5.sh`, `tools/link_radv_prx.sh` | `build_cross.sh` |
| schannel through GnuTLS, with a CA bundle | `tools/build_tls_ps5.sh` | `build_cross.sh` |
| 32-bit support: the WoW64 translator and the native CPU path | `tools/build_wowprospero.sh`, `tools/build_wow64native.sh` | `build_cross.sh` |
| Mesa WGL/Zink for OpenGL games | `tools/build_mesa_zink.sh` | (run on its own) |
| The per-game title shell and root helper (on the Mac) | `tools/build_native.sh`, Lapy `tools/build_owned_daemon.py` | `make_shell.sh` |
| Stage the runtime, the template bottle and Mesa | — | `stage_kit_h.py` |
| Write the kit spec | — | `make_spec.py` |
| Package and validate the kit | `tools/package_z3cloudplay_kit.py` | — |

Kit H is prospero-win rebased onto upstream 644e32e (runtime `bceed2d-kit-h-r1`). Kit H r2 (runtime
`2c4eaa6-kit-h-r2`) adds Wine patch 0170: `build_cross.sh` again, then `update_r2.py` on a copy of kit H's inputs and
spec with the runtime revisions set to 2c4eaa6; only `ntdll.prx` changes. Kit H r3 (runtime `2b91e15-kit-h-r3`)
adds Wine patch 0957 the same way: `build_cross.sh`, then `update_r3.py` on a copy of r2's inputs and
spec with the revisions set to 2b91e15; only `kernelbase.dll` changes, in the runtime and in the template bottle.
Kit H r4 (same runtime as r3) adds Wine Mono 11.3.0 for .NET programs. It needs no VM:
`update_r4.py` on a copy of r3's inputs and spec, with `Z3CLOUDPLAY_WINE_MONO_ARCHIVE` set to the official
`wine-mono-11.3.0-x86.tar.xz`. Mono goes to the kit's `mono/wine-mono-11.3.0`, once per console rather than once per game,
and the template bottle's `user.reg` points Wine at it (`HKCU\Software\Wine\Mono` `RuntimePath`). A bottle made from an
earlier template does not have that value: use Recreate bottle. Kit H r5 (runtime `70bfc92-kit-h-r5`) reads a profile's `[environment]` section and has Wine patch 0958, so Z3CloudPlay's Environment variables and
CPU cores settings reach the game (capability `settings-profile-environment`). Copy r4's inputs (without `titles/`) and
spec, run `make_shell.sh` with `Z3CLOUDPLAY_KIT_DIR` on the prospero-win commit that pins the runtime (the new title shell),
`build_cross.sh` on 70bfc92, then `update_r5.py` in the VM. Only the shell's `eboot.bin` and `ntdll.prx` change; the
template is r4's, so bottles need no Recreate. Kit H r6 (r5's runtime and shell) gives the
template bottle what Wine Mono's support MSI writes when Wine installs it, which kit H bottles never run: the .NET Framework
`InstallRoot` (both registry views) and the other keys of the MSI's Registry table, and `C:\windows\Microsoft.NET`. Without
them mscoree's `LoadLibraryShim` fails (Antichamber). Install the MSI once with kit H's host Wine on an x86_64 Linux host
(the commands are in `update_r6.py`; Rosetta can't run wow64 Wine), then run `update_r6.py` on the Mac on a copy of r5's
inputs and spec. It takes only what the MSI accounts for and checks each file against the MSI's hashes or the runtime's
builtins. Bottles made from an earlier template need Recreate bottle. Kit H r8 (the kit this release uses; runtime
`13fa077-kit-h-r8`; r7 was a development kit and was not released) is r6 with Wine patch 0603: Windows memory queries
report the title's direct-memory budget instead of about zero, so Fallout: New Vegas starts. Build the cross PRXs with
`build_wine_ps5.sh` on 13fa077 into the same `wine-ps5` work folder as before (assert messages keep the source path, so
another folder name changes every PRX), then run `update_r8.py` on a copy of r6's inputs and spec with
`Z3CLOUDPLAY_CROSS_PRX` at that build's `prx/sce_module`. Only `ntdll.prx` changes, so bottles need no Recreate. RADV (`libvulkan.prx`) comes
from [PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan) 8aee3af / [PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa)
01d7057, the same as kit G r2. OpenGL games under Zink need at least PS5_Mesa f2ee389.

## Not done yet

A build from a clean machine, with every input fetched or built by a script, is not written yet.
