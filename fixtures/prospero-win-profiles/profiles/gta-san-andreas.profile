; Grand Theft Auto: San Andreas (your own copy, PC version 1.0), Direct3D 9
; through DXVK 2.6.2, at the console's 1920x1080 and 60 Hz, with mods that
; make it play well on the DualSense.
;
; How it runs on the PS5 (October 2026): 60 fps in Grove Street and on the
; road, with the DualSense, intro movies and sound. It needs a prospero-win
; runtime with #373 (the game's CPU check stops an older one at startup) and
; #374 (without it, Framerate Vigilante drops the game to about 15 fps).
;
; Setting it up (recipes/gta-san-andreas.yml in prospero-win-profiles does
; all of it):
; - Copy the folder of your installed game (the one that holds gta_sa.exe)
;   into C:\Games\GTASA of a prefix of its own
;   (/data/prospero-win/prefixes/gta-san-andreas/drive_c/Games/GTASA). It has
;   to be version 1.0, which the mods need: gta_sa.exe is 14,383,616 bytes.
;   Steam and Rockstar Games Launcher copies are newer and need downgrading.
; - Put DXVK 2.6.2's 32-bit DLLs in that prefix's C:\windows\syswow64.
; - Install LAV Filters in the prefix (winetricks lavfilters). The game plays
;   its intro movies through DirectShow; Wine's own MPEG decoder needs
;   GStreamer, which the console doesn't have, and without a working one the
;   game waits on the first movie forever.
; - Mods, in the game's scripts folder, loaded by the Ultimate ASI Loader,
;   which replaces vorbisFile.dll (the original is kept as vorbisHooked.dll):
;   - GInput: San Andreas only reads gamepads through DirectInput, which the
;     DualSense doesn't show up as. GInput switches the game to XInput, with
;     rumble. Set PlayStationButtons=1 in GInputSA.ini for PlayStation icons.
;   - SilentPatch, which fixes many of the game's own bugs.
;   - Widescreen Fix: a 16:9 HUD and menus, and the game starts at the
;     screen's 1920x1080 instead of 800x600. Widescreen Frontend adds 16:9
;     loading screens.
;   - Open Limit Adjuster, which bigger mods need.
;   - Framerate Vigilante: the game was made for 30 fps, and at 60 some
;     physics and missions misbehave; this fixes most of it. It's only on
;     MixMods (mixmods.com.br), so download it there. Keep the game's own
;     frame limiter on (the default), as it asks.
;
; The game keeps its settings and saves in
; C:\users\prospero\Documents\GTA San Andreas User Files.
[application]
id = gta-san-andreas
name = GTA San Andreas
executable = C:\Games\GTASA\gta_sa.exe
working_directory = C:\Games\GTASA
dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n
prefix = gta-san-andreas
runtime = wine-wow64
architecture = pe32
graphics = dxvk

[display]
desktop = 1920x1080
scaling = fit

[input]
mode = xinput
