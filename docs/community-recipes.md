# Community recipes

A community recipe tells Z3CloudPlay how to start one Steam game on the PS5. It is a
Z3CloudPlay recipe (the same JSON as `runtime/recipes/*.json`) with a few more fields
that say who tested it, where and how well. Z3CloudPlay reads the recipe part with
`ParseGameRecipe` and the whole file with `ParseCommunityRecipe`
(`src/runtime/game_settings.h`).

## Make one

1. Get the game to start from **Game settings** (program, arguments, library
   overrides, other rows).
2. Set the game's rating and note under **Compatibility and note**.
3. In **Game settings > Advanced**, choose **Export as recipe**. Z3CloudPlay writes
   `recipe-<app id>.json` next to its logs (`/app0/logs` on the console, `logs/`
   in the data folder on a Mac).
4. Fill in `community.author`, check the file, and send it as a pull request to
   Z3CloudPlay's repository.

## Format (format_version 1)

```json
{
  "recipe_id": "community-1592280",
  "recipe_version": 1,
  "app_id": 1592280,
  "name": "Selaco",
  "launch_entries": [
    {"id": "program", "label": "SELACO.exe", "executable": "SELACO.exe", "working_directory": "", "arguments": ""}
  ],
  "arguments": "",
  "resolution_arguments": "",
  "dll_overrides": "",
  "architecture": "pe64",
  "runtime": "wine-wow64",
  "prefix": "game",
  "graphics": "opengl",
  "detected": "opengl graphics=opengl",
  "defaults": {
    "resolution": "1280x720"
  },
  "locked": [],
  "steam_appid": 1592280,
  "executable": "SELACO.exe",
  "community": {
    "format_version": 1,
    "min_kit": "kit-h",
    "tested_firmware": "13.60",
    "tested_date": "2026-10-10",
    "status": "playable",
    "notes": "Menus need the mouse preset.",
    "author": "",
    "base": {"repository": "https://github.com/…/prospero-win-profiles", "path": "profiles/selaco.profile", "revision": "abc123"}
  }
}
```

Recipe fields:

| Field | Meaning |
|---|---|
| `launch_entries` | The program that starts, relative to the game folder; its `working_directory` (empty: the program's folder) and its own `arguments`. |
| `arguments` | Arguments the game needs. `{resolution}` is replaced by `resolution_arguments`, with `{w}` and `{h}` from the Resolution setting. |
| `dll_overrides` | The full Wine library override list, like `d3d9,dxgi=n;dinput8=n,b`. Nothing is added to it. |
| `architecture` | `pe32` or `pe64`, from the program's PE header. |
| `graphics` | `auto`, `gdi`, `dxvk` or `opengl`. |
| `defaults` | Game settings by setting id (`SettingsCatalogue()`), only values that differ from Z3CloudPlay's default. |
| `locked` | Setting ids the player must choose "Change anyway" to change. |

Community fields:

| Field | Meaning |
|---|---|
| `steam_appid`, `executable` | Identity: must match `app_id` and a launch entry's `executable`. |
| `community.min_kit` | The runtime kit it was tested with. Older kits may lack what it needs. |
| `community.tested_firmware`, `tested_date` | PS5 system software and day of the test. |
| `community.status` | `works`, `playable`, `broken` or `untested`. |
| `community.notes`, `author` | Free text. |
| `community.base` | Optional. A prospero-win-profiles profile (repository, path, revision) that Z3CloudPlay uses as the base; this file's values apply on top. |

Rules: `format_version` is 1; unknown fields are ignored; a file with another
`status` or a `steam_appid` that does not match `app_id` is refused. The license
for contributed recipes is not decided yet; do not accept
contributions until it is.

Applying community recipes from the preset chooser is planned.
