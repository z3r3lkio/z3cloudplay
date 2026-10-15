# Compatibility

Games tested on a PS5 on firmware 13.60, plus community test reports (linked in the notes) on other firmware. Last updated 2026-10-14.

> [!NOTE]
> This release uses **kit H r8**, where OpenGL games draw through Mesa (Zink on Vulkan). Every game marked **H** was
> checked on kit H (the revision where the column names one). Balatro was not, so its result is from kit C, which drew OpenGL through ps5-opengl instead.

Did you test a game? [Open a test report](../../issues/new?template=test-report.yml), even when it works.

## Status

| Status | Meaning |
| --- | --- |
| ✅ Works | Reaches the menu and plays with the controller, controller preset or USB input noted. |
| 🟡 Partial | Plays, with a problem that gets in the way. |
| ❌ Broken | Starts, but does not reach a playable state. |
| ⛔ Refused | Z3CloudPlay refuses to prepare it and tells you why. For .NET and shared-section refusals, Game settings › Known problems › Try anyway prepares it anyway. |

The **Kit** column is the newest runtime kit the game was checked on. The **Graphics** column is what the game
uses: Direct3D 8–11 goes through DXVK to Vulkan.

## Games

| Game | Status | Kit | Engine · Graphics · CPU | Notes |
| --- | --- | --- | --- | --- |
| [STAR WARS: The Force Unleashed Ultimate Sith Edition](https://store.steampowered.com/app/32430) | ✅ Works | H r6 | D3D9 · DXVK · 32-bit | In the game's own launcher, set Input Device to Xbox360 Controller (`IsKBAndMouse=FALSE`), then start `SWTFU.exe`. Loading a save and walking as Darth Vader with the controller work. Full campaign not tested. |
| [IMMORTALITY](https://store.steampowered.com/app/1350200) | ✅ Works | H r8 | Unity · D3D11 · DXVK · 64-bit | Default configuration. Controller menus, film-grid zoom/navigation, opening clip playback and scrubbing, pause/image mode, and face selection to a second clip verified. Bundled Bink DLLs handle video; this does not verify the missing MF backend. Limited opening test; audio synchronization, later clips and full playthrough untested. |
| [Call of Juarez Gunslinger](https://store.steampowered.com/app/204450) | ✅ Works | H r6 | D3D9 · DXVK · 32-bit | Add `-nologos` in Game settings > Extra arguments. Verified controller menus, Continue into A Fistful of Hot Lead, camera and walking; loading took about two minutes. Default startup aborts on missing `winegstreamer_create_wm_sync_reader` from wmvcore. Movie playback does not work yet; later movies, combat and full playthrough untested. |
| [Machinarium](https://store.steampowered.com/app/40700) | ✅ Works | H r6 | DXVK · 32-bit DX9 | Select Machinarium_old_32bit_DX9.exe in Game settings > Program, then Recreate bottle. Verified menu, opening scene and first junkyard scene with native controller cursor/New Game input; no full playthrough. Current 64-bit executable crashes on a null D3D12 command allocator before Reset; DX9 bypasses it. |
| [Bulletstorm: Full Clip Edition](https://store.steampowered.com/app/501590) | ⚠️ Partial | H r6 | Unreal 3 · DXVK · 64-bit | Missing MF video handler (0x80040154). Disabling StartupMovies/AlwaysLoadedMovies in DefaultEngine.ini reaches the menu with controller input; New Game still crashes on LoadingMovie00.upk. Needs Windows video playback, which does not work yet; gameplay blocked. |
| [Fallout: New Vegas](https://store.steampowered.com/app/22380) | ✅ Works | H r8 | Gamebryo · D3D9 · DXVK · 32-bit | Needs kit H r8: on earlier kits the game saw almost no memory and stopped at start-up. Press Play once with the default program, the game's launcher, so it writes its settings. Then set Game settings › Program = `FalloutNV.exe`. The legal screen, the logos and the main menu (Continue, New, Load) work. Gameplay not tested yet. |
| [DoDonPachi Resurrection](https://store.steampowered.com/app/464450) | ✅ Works | H r9 | — · D3D9 · DXVK · 32-bit | Needs kit H r9: its sprite effects use `sampler_state` blocks and shaders compiled in a pass, which Wine's effect compiler could not build before (Wine patches 0959–0960); earlier kits quit after about 5 seconds. Intro, title screen, menus, the attract demo and a stage with the controller work. |
| [Muchi Muchi Pork! & PinkSweets Boosted](https://store.steampowered.com/app/4229200) | ✅ Works | H r9 | — · D3D11 · DXVK · 64-bit | Title screen, menus and the first Muchi Muchi Pork! stage with the controller. The first start after installing stayed on a black window once; starting it again with Play worked. Japanese text in the window title shows as boxes. |
| [DOOM Eternal](https://store.steampowered.com/app/782330) | ✅ Works | H r9 | id Tech 7 · Vulkan · 64-bit | Set Program to `DOOMEternalx64vk.exe`. Graphics can stay as detected; the game draws with Vulkan directly. |
| [Half-Life 2](https://store.steampowered.com/app/220) | ✅ Works | H | Source · D3D9 · 64-bit | Menu, New Game and the G-Man intro. |
| [Super Meat Boy](https://store.steampowered.com/app/40800) | ✅ Works | H | Tommunism · D3D9 · 32-bit | Intro, menus and play with the D-pad. |
| [FTL: Faster Than Light](https://store.steampowered.com/app/212680) | ✅ Works | H | SIL · OpenGL · 32-bit | Main menu through Zink. Mouse game: use Controller preset = **Mouse** (left stick and Cross) or a USB mouse. Asks for the language on every start. |
| [Super Hexagon](https://store.steampowered.com/app/221640) | ✅ Works | H | — · D3D11 · 32-bit | Menu and game at about 60 fps; the D-pad steers. |
| [Shadow Warrior Classic Redux](https://store.steampowered.com/app/225160) | ✅ Works | H | Build · OpenGL · 32-bit | Main menu at about 60 fps through Zink; the DualSense works. |
| [METAL GEAR RISING: REVENGEANCE](https://store.steampowered.com/app/235460) | ✅ Works | H | Platinum · D3D9 · 32-bit | Plays with the DualSense; Cloud saves restore. A mouse pointer shows in the top-left corner on logos and menus. 26 GB. |
| [Papers, Please](https://store.steampowered.com/app/239030) | ✅ Works | H | Unity · D3D11 · 64-bit | Mouse-only game: use Game settings › Controller preset = **Mouse**, or a USB mouse and keyboard. |
| [Hexcells](https://store.steampowered.com/app/265890) | ✅ Works | H | Unity · D3D11 · 32-bit | Reaches the main menu with the Unity `-force-d3d9` option. |
| [Undertale](https://store.steampowered.com/app/391540) | ✅ Works | H | GameMaker · D3D9 · 32-bit | Use Controller preset = **Arrow keys**. The game's own gamepad support does not see the DualSense yet. |
| [Katana ZERO](https://store.steampowered.com/app/460950) | ✅ Works | H | GameMaker · D3D11 · 32-bit | Plays with the DualSense. |
| [Into the Breach](https://store.steampowered.com/app/590380) | ✅ Works | H | — · OpenGL · 32-bit | Menu, new game and island map with the DualSense, at about 60 fps through Zink. |
| [Loop Hero](https://store.steampowered.com/app/1282730) | ✅ Works | H | GameMaker · D3D11 · 64-bit | The controller drives the menus. |
| [Vampire Survivors](https://store.steampowered.com/app/1794680) | ✅ Works | H | Unity · D3D11 · 64-bit | Plays with the DualSense; Cloud saves restore. A mouse pointer was seen once in the top-left corner. |
| [Sektori](https://store.steampowered.com/app/2105620) | ✅ Works | H | Unity · D3D11 · 64-bit | The DualSense shows up as an Xbox controller (Windows.Gaming.Input). |
| [Pizza Tower](https://store.steampowered.com/app/2231450) | ✅ Works | H | GameMaker · D3D11 · 64-bit | Starts and reaches the menu with the controller. |
| [Slay the Spire 2](https://store.steampowered.com/app/2868840) | ✅ Works | H | Godot (.NET) · OpenGL · 64-bit | Add `--rendering-driver opengl3` in Game settings › Extra arguments: Z3CloudPlay picks Godot's Vulkan renderer, which did not work on kit H. On OpenGL, 60 fps on the menu and in combat. Tested from an exFAT USB drive. |
| [CHRONO TRIGGER](https://store.steampowered.com/app/613830) | ✅ Works | H | Cocos2d · OpenGL · 32-bit | Title, menu, and Resume loads the bookmark; Crono walks. Through Zink. |
| [A Short Hike](https://store.steampowered.com/app/1055540) | ✅ Works | H | Unity (Mono) · D3D11 · 32-bit | Title screen, Continue loads the save, and the hiker walks. Crashed on older kits. |
| [140](https://store.steampowered.com/app/242820) | ✅ Works | H | Unity 5.3 · D3D11 · 32-bit | Title animation, hub world and movement with the stick. Never drew on older kits. |
| [Azure Striker GUNVOLT](https://store.steampowered.com/app/388800) | ✅ Works | H | Inti Creates · D3D9 · 32-bit | In the settings window at start, Circle confirms and Cross cancels. Then title screen and menu with the controller. |
| [Blades of Time](https://store.steampowered.com/app/208670) | ✅ Works | H | Dagor · D3D9 · 32-bit | Intro, main menu and Story with the controller. Z3CloudPlay removes its old Steam DRM (SteamStub 2.1). |
| [BALL x PIT](https://store.steampowered.com/app/2062430) | ✅ Works | H | Unity · D3D11 · 64-bit | Intro and the first cutscene. |
| [The Silent Age](https://store.steampowered.com/app/352520) | ✅ Works | H | Unity · D3D9 · 32-bit | Use Controller preset = **Mouse**: Options confirms the Unity settings window, then the left stick and Cross play. Cloud saves restore. |
| [Future Knight](https://store.steampowered.com/app/4235410) | ✅ Works | H | Unity · D3D11 · 64-bit | Logo and the intro story; Cross advances. |
| [Schildmaid MX](https://store.steampowered.com/app/2154760) | ✅ Works | H | Unity · D3D11 · 64-bit | Logos, then the main menu with the controller. |
| [Redout: Enhanced Edition](https://store.steampowered.com/app/517710) | ✅ Works | H r8 | Unreal 4 · D3D11 · 64-bit | Menus, Quick Race and the garage with the controller. |
| [Project Warlock](https://store.steampowered.com/app/893680) | ✅ Works | H | Unity · D3D11 · 64-bit | Plays the first level with the sticks. The D-pad does not move the menu focus. |
| [STAR WARS Knights of the Old Republic II: The Sith Lords](https://store.steampowered.com/app/208580) | ✅ Works | H | Odyssey · OpenGL · 32-bit | New game, cutscenes and the prologue with the controller. |
| [BULLET SOUL](https://store.steampowered.com/app/544580) | ✅ Works | H | — · D3D9 · 32-bit | Set Game settings › Program = `BulletSoulWin.exe`, then Recreate bottle: Z3CloudPlay picks the launcher, which the controller cannot use. Then it plays with the stick. |
| [REPLACED](https://store.steampowered.com/app/1663850) | ✅ Works | H r8 | Unity · D3D11 · 64-bit | Cloud saves restore; Continue loads the save and the character moves. |
| [SENRAN KAGURA Peach Ball](https://store.steampowered.com/app/1074080) | ✅ Works | H | — · D3D11 · 64-bit | Title menu and the story scenes with the controller. |
| [Brütal Legend](https://store.steampowered.com/app/225260) | ✅ Works | H | Buddha · D3D9 · 32-bit | Plays with the controller. |
| [Star Wars: Battlefront II (Classic, 2005)](https://store.steampowered.com/app/6060) | ✅ Works | H | — · D3D9 · 32-bit | Use Controller preset = **Mouse**: the left stick moves the pointer, Cross clicks, Options is Enter. Menus and the first mission load. |
| [ENSLAVED: Odyssey to the West Premium Edition](https://store.steampowered.com/app/245280) | ✅ Works | H | Unreal 3 · D3D9 · 32-bit | Main menu and New Journey with the controller. Needs kit H r4 (.NET through Wine Mono). |
| [GUN](https://store.steampowered.com/app/2610) | ✅ Works | H | — · D3D9 · 32-bit | Menus and New Game with the controller. Needs kit H r3. |
| [DUSK](https://store.steampowered.com/app/519860) | ✅ Works | H | Unity · D3D11 · 64-bit | Menus with the left stick and Cross; the first level plays. |
| [R.E.P.O.](https://store.steampowered.com/app/3241660) | ✅ Works | H r5 | Unity · 64-bit | Reaches the menu and plays single player with a USB mouse and keyboard; multiplayer does not work. Controller not tested. ([#8](https://github.com/leolobato/z3cloudplay-ps5/issues/8), firmware 9.40) |
| [Portal 2](https://store.steampowered.com/app/620) | ✅ Works | H r8 | Source · D3D9 · 32-bit | Default settings, DualSense; gameplay confirmed. ([#36](https://github.com/leolobato/z3cloudplay-ps5/issues/36), firmware 13.60) |
| [Megabonk](https://store.steampowered.com/app/3405340) | ✅ Works | H r8 | — | DualSense; Max/Ultra graphics reported. ([#30](https://github.com/leolobato/z3cloudplay-ps5/issues/30), firmware 13.60) |
| [American Truck Simulator](https://store.steampowered.com/app/270880) | ✅ Works | H r8 | — | Disable news, World of Trucks and image spotlight. DualSense or USB mouse and keyboard; startup takes over a minute. ([#27](https://github.com/leolobato/z3cloudplay-ps5/issues/27), firmware 9.40) |
| [Fast Food Simulator](https://store.steampowered.com/app/2916430) | ✅ Works | H r8 | — | Single-player with USB mouse and keyboard. Controller and multiplayer untested. ([#26](https://github.com/leolobato/z3cloudplay-ps5/issues/26), firmware 9.40) |
| [Wobbly Life](https://store.steampowered.com/app/1211020) | ✅ Works | H r8 | — | Single-player with DualSense or USB mouse and keyboard; multiplayer does not work. Loading takes about a minute. ([#25](https://github.com/leolobato/z3cloudplay-ps5/issues/25), firmware 9.40) |
| [IT Specialist Simulator](https://store.steampowered.com/app/3266090) | ✅ Works | H r8 | — | USB mouse and keyboard; controller untested. ([#21](https://github.com/leolobato/z3cloudplay-ps5/issues/21), firmware 9.40) |
| [Gas Station Simulator](https://store.steampowered.com/app/1149620) | ✅ Works | H r8 | — | Medium graphics preset, USB mouse and keyboard; controller untested. ([#17](https://github.com/leolobato/z3cloudplay-ps5/issues/17), firmware 9.40) |
| [Teardown](https://store.steampowered.com/app/1167630) | ✅ Works | H r8 | OpenGL | Set Game settings › Graphics = **OpenGL**. USB mouse and keyboard works; controller untested. Tester recommends borderless window, 1920×1080, render scale 75%, and changing the two settings below render scale from High to Low. ([#16](https://github.com/leolobato/z3cloudplay-ps5/issues/16#issuecomment-6169881987), firmware 9.40) |
| [PlateUp!](https://store.steampowered.com/app/1599600) | 🟡 Partial | H r5 | Unity · 64-bit | Reaches the menu at 30+ fps with the DualSense (default and **Arrow keys** presets); the game's FPS limit options in Settings › Graphics reduce lag. Typing the profile name needs a USB keyboard. Loading takes 30–45 seconds. ([#3](https://github.com/leolobato/z3cloudplay-ps5/issues/3), firmware 9.40) |
| [Retro Rewind - Video Store Simulator](https://store.steampowered.com/app/3552140) | 🟡 Partial | H r5 | — | Reaches the menu and plays well with a USB mouse and keyboard; the DualSense does not work. ([#7](https://github.com/leolobato/z3cloudplay-ps5/issues/7), firmware 9.40) |
| [Mullet Mad Jack](https://store.steampowered.com/app/2111190) | 🟡 Partial | H | Unity · D3D11 · 64-bit | Menus work with the controller, but the account's Cloud saves did not show in the game. Turn off Cloud sync for it until this is fixed. |
| [Antichamber](https://store.steampowered.com/app/219890) | 🟡 Partial | H | Unreal 3 · D3D9 · 32-bit | Needs kit H r6. Reaches the first room; the controller is ignored. With Controller preset = **Mouse** the stick turns the view; walking needs a USB keyboard (WASD) or Button mapping. |
| [Return to Castle Wolfenstein](https://store.steampowered.com/app/9010) | 🟡 Partial | H | id Tech 3 · OpenGL · 32-bit | Starts and plays its intro. The game has no controller support: use a USB mouse and keyboard, or Controller preset = **Mouse** (not tested yet). |
| [Fallout 4](https://store.steampowered.com/app/377160) | 🟡 Partial | H r9 | Creation Engine · D3D11 · DXVK · 64-bit | Set Program to `Fallout4.exe`. Plays, but the game's sound effects and voices are silent: they are xWMA, which needs the `winegstreamer` media backend that the kit does not ship yet. The intro videos and the menu music play. |
| [Balatro](https://store.steampowered.com/app/2379780) | ❌ Broken | C | LÖVE · OpenGL · 64-bit | Stops at start-up: its file system (PhysFS) fails to start. |
| [Look Outside](https://store.steampowered.com/app/3373660) | ⛔ Refused | H | RPG Maker (NW.js) · 64-bit | Runs on NW.js (a built-in web browser), which Z3CloudPlay can't start on the PS5 yet. |
| [BLUE REVOLVER](https://store.steampowered.com/app/439490) | ✅ Works | H r6 | LÖVE · OpenGL · 64-bit | Default settings. The intro, the language menu and the story opening work with the controller. The earlier PhysFS start-up failure no longer happens; its cause is unknown. Gameplay not tested on r6. |
| [Fellowship](https://store.steampowered.com/app/2352620) | ❌ Broken | H r5 | Unreal · 64-bit | Shows "The UE-fellowship Game has crashed and will close" (Fatal error) at start-up. ([#2](https://github.com/leolobato/z3cloudplay-ps5/issues/2), firmware 13.60) |
| [BeamNG.drive](https://store.steampowered.com/app/284160) | ❌ Broken | H r8 | D3D11 · DXVK · 64-bit | Launcher reports error 50 (request not supported). Starting `Bin64/BeamNG.drive.x64.exe` directly with Graphics = **DXVK** and `-gfx dx11` returns to the console home screen before drawing a frame. Logs show failed virtual-memory reservations followed by `chrome_elf.dll` initialization failure. ([#23](https://github.com/leolobato/z3cloudplay-ps5/issues/23), firmware 9.40) |
| [Far Cry 3 Blood Dragon](https://store.steampowered.com/app/233270) | ❌ Broken | H | Dunia 2 · D3D11 · 32-bit | Needs Ubisoft Connect. |
| [Bullet Witch](https://store.steampowered.com/app/696180) | ❌ Broken | H | — · D3D9 · 32-bit | Needs PhysX, which the game installs from its redistributables. Z3CloudPlay does not run them yet. |
| [CULTIC](https://store.steampowered.com/app/1684930) | ❌ Broken | H | Unity · D3D11 · 64-bit | Stays on a black loading screen. |
| [Spec Ops: The Line](https://store.steampowered.com/app/50300) | ❌ Broken | H | Unreal 3 · D3D9 · 32-bit | Exits after about 30 seconds without a picture. It probably needs its redistributables. |
| [Singularity](https://store.steampowered.com/app/42670) | ❌ Broken | H | Unreal 3 · D3D9 · 32-bit | Crashes a few seconds after start. |
| [Subnautica 2](https://store.steampowered.com/app/1962700) | ❌ Broken | H r8 | Unreal · 64-bit | Cannot start with `-dx11`; without it, reports DirectX 12 is unsupported. ([#20](https://github.com/leolobato/z3cloudplay-ps5/issues/20), firmware 13.60) |
| [Metal Wolf Chaos XD](https://store.steampowered.com/app/820630) | ❌ Broken | H | — · D3D11 · 32-bit | Crashes about 14 seconds after start, when it opens its first video: Windows video playback (Media Foundation with GStreamer) is not available yet. |
| [Hotline Miami](https://store.steampowered.com/app/219150) | ❌ Broken | H | GameMaker · D3D8 · 32-bit | Steam starts it through a .NET launcher, so it is refused. With Game settings › Program = `HotlineMiami_Original.exe` it starts, but the keyboard and controller do nothing. |
| [STAR WARS Empire at War: Gold Pack](https://store.steampowered.com/app/32470) | ❌ Broken | H r9 | Alamo · D3D9 · DXVK · 64-bit | Set Program to `GameData/StarWarsG.exe` (`runme.exe` only starts a launcher), Graphics to DXVK and Controller preset to **Mouse**. Needs kit H r9 for its built-in effects (Wine patches 0961–0963). With them, the intro and the main menu's 3D scene play and the menus work with the pointer (on the very first start the menu was 4:3 and ignored clicks; later starts were fine). Starting any tutorial crashes while the map loads (NULL read in `ucrtbase.dll`). |
| [Need for Speed Most Wanted](https://store.steampowered.com/app/1262560) | ⛔ Refused | H | Chameleon · D3D11 · 32-bit | Needs the EA app. |

## Kits

| Kit | What changed for games |
| --- | --- |
| H r8 | Games see the console's memory, about 12 GB, and a page file of the same size. Before, they saw almost none, so games that check free memory at start-up stopped (Fallout: New Vegas). No Recreate bottle needed. |
| H r6 | Bottles have the .NET Framework registry entries and files that Wine Mono's installer writes, so .NET lookups of the Framework folder work. Recreate bottle for games set up on an earlier kit. |
| H r4 | Wine Mono ships with the kit, so .NET programs start instead of being refused (ENSLAVED). |
| H r3 | Wine reads the computer name without wineboot, so DirectX version checks through DxDiag pass (GUN). |
| H | prospero-win rebased on newer upstream. 140 and A Short Hike now start. Godot's OpenGL renderer works through Zink, but its Vulkan renderer did not work. Schannel (TLS) through GnuTLS. Upstream's native 32-bit mode for any firmware. |
| G | prospero-win rebased on upstream. OpenGL draws through Mesa Zink instead of ps5-opengl, which fixes CHRONO TRIGGER. Godot games use Vulkan, so Slay the Spire 2 runs at 60 fps. "120 Hz output" and "OpenGL worker thread" are not available. |
| F | The DualSense also shows up as an Xbox 360 HID controller, so games that use DirectInput, Raw Input or Windows.Gaming.Input find it. Shadow Warrior Classic Redux now starts. |
| E | A mouse pointer on a scaled game reaches the whole picture, and the game window no longer flashes. |
| D | Every component rebased on upstream; Wine's heap in direct memory (more free memory for games). |

To see which kit your console has, run the [log script](tools/collect_logs.py): it prints the kit version.

## Notes for all games

- **32-bit games** can run through an x86 translator, which is slower than native. For 32-bit OpenGL games, Mesa
  runs inside the translator too.
- **New games can stay at *Waiting for the PS5*** when ShadowMountPlus stops registering titles. Reload
  ShadowMountPlus.
- **Kernel anti-cheat** games (for example Easy Anti-Cheat or BattlEye in kernel mode) cannot run.
- Games that need a **third-party launcher or account** (EA app, Ubisoft Connect, Rockstar) are not supported yet.
