; Half-Life 2 (your own copy), Direct3D 9 through DXVK 2.6.2, at the
; console's native 1920x1080. It runs without the Steam client.
;
; Setting it up:
; - Install Half-Life 2 with Steam on your PC (Windows, or Steam under Wine;
;   tested with the 2024-era build that came with The Orange Box). Copy
;   hl2.exe and the bin, platform and hl2 folders from its install folder
;   into C:\Games\HalfLife2 of a prefix of its own on the console
;   (/data/prospero-win/prefixes/half-life-2/drive_c/Games/HalfLife2).
; - Put DXVK 2.6.2's 32-bit DLLs (the release's x32 folder) in that prefix's
;   C:\windows\syswow64. Don't add a dxvk.conf: the default settings are the
;   tested ones.
; - The game starts at its menu. A map takes about 25 seconds to load on
;   the console.
;
; Needs a prospero-win runtime with Wine patch 0730 (prospero-win #289) and
; the WoW64 CPU backend from prospero-win #296. 32-bit DXVK keeps textures in
; 64 MiB Windows file mappings and maps and unmaps windows of them thousands
; of times while a map loads. Without #289, loading the first map freezes and
; closing the game leaves the console stuck on "Closing..." until it is
; restarted; without #296, a map takes about 95 seconds to load and the frame
; rate drops while new textures appear.
;
; For a steady 60 fps the runtime also needs prospero-win #312 and #316
; (October 2026). Without them the game plays at 60 fps but, a couple of
; minutes into the train station, falls to between 1 and 15 fps for up to a
; few minutes, then recovers on its own; with #312 alone it holds 60 with
; brief dips to about 53. With both, an automated run from the train to
; Kleiner's call holds 60 fps throughout. The menu's mouse cursor needs
; prospero-win #305.
;
; Tested settings (Options > Video > Advanced): High texture, model and
; water detail (reflect all), high shadows, 4x MSAA, 16x anisotropic
; filtering and full HDR. With them the game holds 60 fps, the console's
; refresh rate, from the train to the canals, and plays with the DualSense or
; a keyboard and mouse.
[application]
id = half-life-2
name = Half-Life 2
executable = C:\Games\HalfLife2\hl2.exe
working_directory = C:\Games\HalfLife2
arguments = -game hl2 -novid -fullscreen -w 1920 -h 1080
dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n
prefix = half-life-2
runtime = wine-wow64
architecture = pe32
graphics = dxvk

[display]
desktop = 1920x1080

[input]
mode = xinput
