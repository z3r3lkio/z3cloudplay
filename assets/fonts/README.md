# Supplemental launcher fonts

These are post-selection launcher assets, separate from the frozen UI comparison
fixtures. The shared fixture font files and comparison tour remain unchanged.

`NotoSans-Regular.ttf` provides the complete Greek fallback, including the
accented letters absent from the fixture's Noto CJK face. It is distributed
under SIL Open Font License 1.1; see `NotoSans-OFL.txt`. The font was copied
unchanged from the existing local `PS5_RetroArch` repository; the license text
is unchanged, with CRLF line endings normalized to LF:

- Source revision: `8854f31c648c918c7208a404cdb546ae2f6eb610`
- Source font: `handoff/PPSA99002/ui/fonts/NotoSans-Regular.ttf`
- Source license: `handoff/PPSA99002/ui/fonts/NotoSans-OFL.txt`
- Font SHA-256: `b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5`
- Original license SHA-256: `3c05a56499a20ee045a6d36834b6a9e1108f359eede10d7c1613bc4524d01eef`
- Distributed LF license SHA-256: `0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb`

The source commit's font bytes were checked against this vendored copy. The
license credits the Noto Project Authors (2018). No network download, source
font conversion, or fixture modification was performed.

`scripts/build-homebrew.sh --prepare-only` and the host CMake build bake
`noto-greek-fallback.huifont` at 56 px, SDF range 8, atlas width 2048, with the
European range plus the fixture text corpus. Both build paths copy the OFL
license alongside the generated atlas, so it is included in host and PS5
`assets/fonts`. The baked atlas SHA-256 at this revision is
`66f2e156c897eeb4f1831187ba14acf93f45a9a2a1b152bee10207f98e89be8f`.

`homebrew_font_greek` checks every codepoint in fixture game 900003's title
with `Font::has_glyph`, then checks all baked glyph silhouettes against the
source face's ordinary rasterizer. This is glyph coverage, not text shaping.
