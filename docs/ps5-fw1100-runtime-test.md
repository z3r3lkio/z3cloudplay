# PS5 firmware 11.00 — compatibility kit diagnostic

Status: **experimental, no game-runtime compatibility verified**.

Test console: PS5 11.00 + kstuff + ShadowMount+ + ELF Loader.
Launcher: z3cloudplay FAKE77031.

## Facts verified in source
- `runtime/kit_manifest.cpp` describes the manifest firmware list as informational: any console firmware is admitted if the kit validates.
- `runtime/kit_trust.h` expects kit root `/data/z3cloudplay/kits/hl2-fw1360-dev1`.
- `ps5.cpp` logs `console_firmware`, `kit_admission`, and the kit admission reason to `runtime.log`.
- Original published kit H r8 was built for 13.60 and uses upstream Playden naming. Compatibility and internal absolute paths on 11.00 are **unknown**.

## Safe initial trial
1. Back up all z3cloudplay app data and existing runtime folders; do not overwrite a working kit.
2. On a PC, download the original `playden-kit-h-r8.zip` from the upstream v0.2.0 release.
3. Inspect the ZIP for `hl2-fw1360-dev1/manifest.json`, `kit_id` and the recorded firmware. Do not edit the manifest or any hashes.
4. Upload using `tools/console_kit_upload.py` from this repository, setting `--remote-parent /data/z3cloudplay/kits`. Only use `--replace` after explicit backup.
5. Restart z3cloudplay, open Settings > Runtime status, and collect `/data/homebrew/FAKE77031/logs/runtime.log`.
6. Verify whether admission succeeds; even if it does, do not assume game execution works. Check PRX loading and title publication logs before testing a game.

## Known limits
- A verified kit proves an intact package, **not compatibility with firmware 11.00**.
- An upstream kit may contain compiled-in paths pointing to `/data/playden`; changing manifest strings cannot fix binaries. Inspect those paths before installing.
- The current firmware-independent admission permits attempted loading, so avoid modifying privileged runtime binaries without a validated recovery route.
- Never expose Steam `session.v2`, authentication tokens, passphrase or account logs while sharing diagnostics.
