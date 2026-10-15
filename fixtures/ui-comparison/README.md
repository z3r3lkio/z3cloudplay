# Frozen Playden UI comparison fixture v1

This package is renderer independent. Both candidates consume these exact bytes. All accounts, games, volumes, jobs, profiles and history are simulated. No network fetch occurs during runs. No volume label is a filesystem destination.

`catalog.json` contains native JSON records with stable game IDs 900001–900024, nullable metadata (null means unknown, never zero), asset keys, organization, account, installation, volume, job and profile records. Byte fields use integer bytes. Times are fixed Unix seconds. Art state is ready/delayed/failed; no logo intentionally exercises title fallback. Source is Steam/Windows only. Fixtures contain no credentials. Optional metadata remains unknown in UI.

`scenarios.json` gives scenario IDs, expected outcomes, manual clock increments, state invariants and common captures. Captures use clock 1760000000, named surface, selected game and two native framebuffer resolutions. Implementations may use their own action syntax but must document the replay and resulting route/focus/job assertions. Shared IDs are the matching key. Default job-1 starts at 43% (461708984/1073741824 bytes), followed by two queued jobs. Screenshot mode never advances the clock. Verification takes two ticks. Pause/resume, retry, cancellation and interrupted restore are explicit branches.

Keep `../library.json` unchanged for the 1000-game stress test. Generate title placeholders from stable ID colors without changing poster cell dimensions. CJK and Arabic strings are capability tests; preserve the original text and report shaping/fallback gaps.

Fonts: pinned reference Barlow and Barlow Condensed with accompanying OFL; Noto Sans CJK SC and Noto Sans Arabic under included OFL notices. Both fallback files are explicitly common. Noto Arabic is variable: a renderer unable to load/sh​ape it must report that gap. Assets are frozen by hash even though the upstream font download path uses a moving branch. Art is the eight existing synthetic posters and hero, never commercial CDN art. Glyphs are shared vector geometry; rasterizing them at final output scale is permitted.

Expected recovery: cancelled QR ignores late approval; unavailable/readonly storage blocks submission; disconnect pauses/fails transfer without inventing completion; failed writes do not pretend persistence succeeded; account switch/sign-out clears visible cache; reset preserves installs/saves/history; collection deletion removes no games. Every surface shows UI simulation. Destructive confirmations initially focus Cancel and consume only a subsequent activation.

## SHA-256 verification

README is excluded to avoid a self-reference. All other package files are listed below. `asset-manifest.json` repeats hashes and byte lengths for distributable assets. Verify with Python hashlib or shasum -a 256 before candidate execution.

```text
c78e18012482333c139391fc4c1d26bfa099b547a287ef165d51c2cc5d147e4a  asset-manifest.json
cc9e9d19e68d375254aae2fb700e93d3267f7a130425365698e0eef72d063b8a  assets/art/hero.png
afd2a68c3549257a1f781dc7475858792ce1132b0951ea06f6f3831a829a25fd  assets/art/poster1.png
d4296677cfff516600d06faa426d2203d6265cdb15f8eafbaa4716a0c3759efd  assets/art/poster2.png
028885697eb454dbc3f66e10cef786f878e2b5ad37d456b9106c7dfbaea55bdc  assets/art/poster3.png
803147d90e9c2da2afb8f49427e90ed1d25da54b61a03c49a193d3a4c9c1de8a  assets/art/poster4.png
cb4e976cac8c05d13575bb038e79c9a5dee968242356c614c6c142980c29b0d3  assets/art/poster5.png
6276023cb82fff5bd086ca780fb5e078acebaefbbbf2a56665d40c2afe56d3f6  assets/art/poster6.png
71d7385939c1102eaf4790831fad2dceae6730bc67999ffff43ec362b525e1d6  assets/art/poster7.png
8807fb95bfb149b487146c4722bad990da210a6ea4aa5935c6d85a23ff7eb2d1  assets/art/poster8.png
f8906f762cb73dca441da034bc363b2d8e2e68bc10d5c05e58717646c20cc4b4  assets/fonts/Barlow-Medium.ttf
95aa02c7c43096e0dd44d787ba6216864a67157e402adab59b35572e0c1577ea  assets/fonts/Barlow-Regular.ttf
86577cb32f8abe3673db53ca0f4221e6856751a4f6730c867e00f720f8bb1fc5  assets/fonts/Barlow-SemiBold.ttf
262bd143292ce479ee0cd09a42b47ab173fca8e9c6eb5ed0b5c8a845bc371d17  assets/fonts/BarlowCondensed-Medium.ttf
7b619d14bc2327509a9ef32b0890f709626f7ecc9ff61191c2a4314c5499d2d9  assets/fonts/BarlowCondensed-SemiBold.ttf
63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e  assets/fonts/NotoSansArabic.ttf
2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b  assets/fonts/NotoSansCJKsc-Regular.otf
186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f  assets/fonts/barlow-OFL.txt
186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f  assets/fonts/barlowcondensed-OFL.txt
07fc70bfeb985cc1a87a8587d0a0c80bab11c86c9dc3fd95b6f0cb332f983e96  assets/fonts/noto-arabic-OFL.txt
6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2  assets/fonts/noto-cjk-LICENSE.txt
f047617196db73415f9e463a58aadddefdabb5594478c0fc8fbcdfd6dcf9791a  assets/glyphs/circle.svg
ac924a11802096d5f3014c476888c25860508fadc15b14e1552a29a6982eb766  assets/glyphs/cross.svg
f59115ef2bef1d83eb62bed42348ade4e3c7cf3f08e60410a0f67d24563554f5  assets/glyphs/square.svg
d19d5e8485ed5fb985edbc8bf58a56bfcbf11ef5eae3b3acccc4b496df73bceb  assets/glyphs/triangle.svg
f9c86f04a68f151539fe175b90340361e7e0fd145054141e1d634782a0cff03b  catalog.json
2c01f435c3014aacc11a0047979d9aeeb2134e410d325d3236b8864cdda528df  scenarios.json
```
