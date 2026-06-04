# CLAUDE.md — Guild II Renaissance Ultrawide HUD Fix

Instructions for Claude working in this repo. Keep this concise; deep
reverse-engineering detail lives in `docs/REVERSE_ENGINEERING.md`.

## What this is

An in-place **`d3d9.dll` proxy** that fixes two HUD panels at ultrawide (21:9)
for *The Guild II Renaissance* (`GuildII.exe`, D3D9/Gamebryo, 32-bit). It touches
no game/script/GUI files; drop `d3d9.dll` + `uw_fix.ini` next to `GuildII.exe`.

## Layout

- `src/uwfix.c` — core: `DllMain`, worker thread, `uw_fix.ini` config, runtime
  signature scanning, the two code caves, and `bp_game_flush()` (the game-thread
  `SetValueInt` for the bottom-right panel).
- `src/d3d9_proxy.c` — d3d9 forwarding + `CreateDevice`/`Present` vtable hooks
  that drive the game-thread pump.
- `Makefile` → `build/d3d9.dll`. `dist/uw_fix.ini` is the shipped default.
- `scripts/deploy.sh` deploys via `$GUILD2_LIVE_DIR`.
- `reference/lua/` — earlier Lua fix, reference only (proves the `ABS_X` lever).
- `.github/workflows/release.yml` — manual release build.

## The two fixes (summary)

- **CharacterPanel** (`uw_fix.ini` `CharacterPanel=1`): per-frame code cave on the
  head render-rect store `[obj+0xcc]`; adds `canvasWidth − 1024` to X.
- **ButtonPanelRight** (`ButtonPanelRight=1`): capture the panel node in a cave
  (gated `W=162,H=210`), then call engine `SetValueInt(node,"ABS_X",screenW-162)`
  / `"ABS_Y",screenH-210` — fired from the hooked `Present` so it runs on the
  game thread (no renderer race). Decoupled from CharacterPanel.

## Build / deploy / verify

```sh
make                              # -> build/d3d9.dll  (needs gcc-mingw-w64-i686)
source scripts/env.example.sh     # set GUILD2_LIVE_DIR first
make deploy                       # copy d3d9.dll + uw_fix.ini into the game
```

Verify by launching the game at ultrawide: both panels snap to the right edge.
Set `Logging=1` (and `Verbose=1`) in the live `uw_fix.ini` to debug; the log
(`ultrawide_fix.log`) shows the version banner, hook installs, and the
`bp SetValueInt … (game-thread)` line.

## Gotchas

- HUD coords are **physical pixels**, not a 1024 virtual canvas.
- Hook sites are found by **byte signature** at runtime — never hardcode build
  addresses; if a signature stops matching, the game build changed (re-scan).
- `SetValueInt` must run on the **game thread** (Present pump) — do not move it to
  the worker thread.
- Editor/clang "windows.h not found" diagnostics are **false positives**; only the
  mingw build (`make`) matters.
- See `docs/REVERSE_ENGINEERING.md` for the dead-ends list before trying a "new"
  approach (resolver-arg rewrite, raw rect poke, `UI_SetPosition`, `.gui` edits,
  winmm/dbghelp — all tried and failed).
