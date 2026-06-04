# The Guild II Renaissance — Ultrawide HUD Fix

> **Note:** This project was built almost entirely by AI (Claude), with very
> little effort from me. Treat it accordingly and use at your own risk.
>
> **Compatibility:** Built and tested against **game version 4.64**. It may not
> work on other versions (the engine hooks are found by signature scanning, but
> are not guaranteed to match a different build).
>
> **Multiplayer:** Not tested in multiplayer. It may not work, and may require
> all players to run the same version of the mod (or none at all).

An in-place `d3d9.dll` proxy that fixes the HUD at ultrawide (21:9) resolutions
(2560×1080, 3440×1440, 5120×1440, …) for **The Guild II Renaissance**
(`GuildII.exe`). No game, script, or GUI files are modified — drop in two files
and go. Works on vanilla **and** the MegaModpack, and survives game reinstalls
(just redeploy the DLL).

## What it fixes

At ultrawide aspect ratios two HUD panels are placed with coordinates tuned for
a ~1024×768 layout and end up stranded near the middle/left instead of the
screen edges:

| Panel | Problem | Fix |
|---|---|---|
| **Bottom-right `ButtonPanelRight`** (Map / Politics / Wealth / Dynasty / Diary / Achievements) | sits ~17% from the left | snapped to the bottom-right corner |
| **Top-right `CharactersPanel`** (3D character portrait) | head rendered off to the left | snapped to the right edge |

## Install

1. Build (or download a release) to get **`d3d9.dll`** and **`uw_fix.ini`**.
2. Copy both into the game folder, next to `GuildII.exe`.
   - Steam: `…/steamapps/common/The Guild 2 Renaissance/`
3. Launch the game normally.

That's it — **no `WINEDLLOVERRIDES`, no renaming any files**. The game is D3D9 and
Proton already loads `d3d9` as native, so the proxy is picked up app-locally
(ReShade-style). The DLL forwards all real rendering to the system `d3d9`.

> Steam Workshop note: a Workshop mod can't deliver a DLL (the launcher only
> deploys asset folders), so this is a manual, off-Workshop install. It layers
> on top of any active mod without conflict.

## Configuration — `uw_fix.ini`

Place next to `GuildII.exe`. Delete it to use defaults (both fixes on, logging
off). `1` = on, `0` = off.

| Key | Default | Meaning |
|---|---|---|
| `CharacterPanel` | `1` | Fix the top-right 3D character panel |
| `ButtonPanelRight` | `1` | Fix the bottom-right button panel |
| `Logging` | `0` | Write `ultrawide_fix.log` to the game folder |
| `Verbose` | `0` | Extra diagnostic dumps (needs `Logging=1`) |

The fixes self-disable at non-ultrawide widths, so leaving them on is safe.

## Build from source

Requires the 32-bit MinGW-w64 cross-compiler.

```sh
# Debian/Ubuntu: sudo apt-get install gcc-mingw-w64-i686
make                 # -> build/d3d9.dll
```

### Deploy to your game

```sh
source scripts/env.example.sh   # after editing GUILD2_LIVE_DIR to your game path
make deploy                     # builds + copies d3d9.dll and uw_fix.ini into the game dir
```

Environment variables (see `scripts/env.example.sh`):

- `GUILD2_LIVE_DIR` — the live game dir you run from (contains `GuildII.exe`). Used by `make deploy`.
- `GUILD2_ORIG_DIR` — a pristine copy of the game files, for diffing/RE only (optional).

## Releases (CI)

Releases are built by a **manually triggered** GitHub Actions workflow
(`.github/workflows/release.yml`):

1. Actions → **Release** → **Run workflow**.
2. Pick the **branch** (the "Use workflow from" dropdown) and enter a **version**
   (e.g. `v1.0.0`).
3. The workflow cross-compiles `d3d9.dll` and publishes a release tagged with
   that version, attaching `d3d9.dll` and `uw_fix.ini`.

## How it works (short version)

The DLL runs a small worker at startup plus two engine hooks, located at runtime
by **byte-signature scanning** (so it isn't tied to one game build):

- **CharacterPanel** — a per-frame code cave on the head's render-rect store adds
  `canvasWidth − 1024` to its X, right-anchoring it every frame.
- **ButtonPanelRight** — replicates the proven Lua fix by calling the engine's
  `SetValueInt("ABS_X", screenW − W)` on the panel node. That call is made on the
  **game thread** from a hooked D3D9 `Present`, so it never races the renderer.

Full reverse-engineering notes, addresses, and dead-ends are in
[`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).

## License

Public domain ([UNLICENSE](UNLICENSE)).
