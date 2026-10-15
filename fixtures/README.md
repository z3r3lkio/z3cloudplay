# Representative library fixture

`library.json` contains exactly 1,000 deterministic synthetic game records. IDs, metadata, missing artwork, and invalid artwork sources exist only for development performance runs and never represent Steam ownership or installation state.

Names intentionally cover ASCII, Latin Extended, Greek, and Cyrillic code points.

The simulation build (`PLAYDEN_UI_SIMULATION=ON`) packages this file with the PS5 title and reads it in simulation mode; normal builds do not use it. The synthetic artwork it shows lives in `ui-comparison/assets/art/`.
