# Reverse-engineering notes — Guild II Renaissance ultrawide HUD fix

The durable record of how the fix works and why, so the hard-won knowledge
survives and dead-ends aren't relitigated. Target: `GuildII.exe` (D3D9/Gamebryo,
32-bit, ImageBase `0x400000`). The live Steam build differs from older builds, so
all hook sites are found at runtime by **byte-signature scanning** of `.text`
rather than hardcoded addresses.

## Delivery: `d3d9.dll` proxy + runtime signature scanning

- Ship the DLL as **`d3d9.dll`** next to `GuildII.exe`. Proton loads `d3d9` as
  native, so the app-local copy is picked up with **no `WINEDLLOVERRIDES` and no
  renamed files** (ReShade-style). `src/d3d9_proxy.c` forwards
  `Direct3DCreate9`/`Ex` to the real system `d3d9`
  (`LoadLibraryEx(system32\d3d9.dll, LOAD_LIBRARY_SEARCH_SYSTEM32)`) and stubs the
  no-op `D3DPERF_*` profiling exports.
- **Why not winmm/dbghelp** (tried, failed): both are Wine builtins with other
  in-process dependents (dsound/fmod; `sentry.dll`). Stubbing/partially overriding
  them starves the dependents → crash. `d3d9` is Proton-native and a leaf here, so
  no cascade and no override needed.
- **Signature scanning**: `find_text_section` parses the loaded PE; `scan_text`
  finds the Nth match. Hooks are installed at whatever address the signature lands.

## Coordinate model (canvas object)

Canvas singleton reached via a global pointer found by the signature
`A1 <glob> 85 C0 75 ?? 68 28 01 00 00` (the `push 0x128` lazy-getter). Fields,
observed at 5120×1440:

| Off | Value | Meaning |
|---|---|---|
| `+0xDC` | 5120 | physical width |
| `+0xE0` | 1440 | physical height |
| `+0xE4/0xEC` | 1024 | design width |
| `+0xF0` | 768 | design height |
| `+0xF4` | 5.0 | physW/designW |
| `+0xF8` | 1.875 | physH/designH |
| `+0xFC` | 4096 | physW − designW (right-anchor X offset) |
| `+0x100` | 672 | physH − designH (bottom-anchor Y offset) |

**HUD coordinates are physical pixels** (`[canvas+0xDC]`), not a 1024 virtual
canvas (an earlier theory that was wrong). Panels carry 1024-era pixel values, so
they sit near the left at ultrawide. Fullscreen on a wide desktop locks the render
width to the desktop width.

The engine *does* offset-anchor, but per-axis by a flag: this panel is
bottom-anchored (Y resolves `558 + 672 = 1230`) but **left**-anchored in X
(X stays `862`, ≈17% from the left) — that asymmetry is the bug.

## Fix 1 — CharacterPanel 3D head (per-frame code cave)

`cl_CharactersPanel`'s layout re-positions its render view every frame and stores
the rect at `[obj+0xcc]=X`. Store site signature
`8B 50 08 89 96 CC 00 00 00` (`mov edx,[eax+8]; mov [esi+0xcc],edx`).

Detour it to a cave: `X += (canvasWidth − 1024)` (read live from the canvas
global), i.e. right-anchor every frame. Self-disables at 1024 wide (offset 0).
Lua/`ABS_X` cannot reach this — the head is a RENDER node positioned by engine
code, not the generic node rect.

## Fix 2 — ButtonPanelRight (engine `SetValueInt`, on the game thread)

The breakthrough lever, taken from a working Lua fix
(`child:SetValueInt("ABS_X", sw - w)` — see `reference/lua/UltrawideFix.lua`):
`cl_ButtonPanel` honors `ABS_X` through the **property system**, and the property
path reaches the physical right edge with **no clip**, unlike the resolver-arg or
raw-rect routes.

- **Capture the node**: the generic geometry resolver calls slot56 with the panel
  via signature `8B 06 51 52 53 55 8B CE FF 90 E0 00 00 00` (the `call [eax+0xe0]`
  is at match+8). A code cave gated on `ABS_WIDTH==162 && ABS_HEIGHT==210`
  (ButtonPanelRight's stable size) captures `esi` (the node) into a global.
- **Apply via the engine setter**: `SetValueInt @0x794ff0` —
  `__thiscall(this=node, key, value)`, `ret 8`. It's the implementation behind the
  Lua `SetValueInt` binding (Lua wrapper at `0x5832b0`; the registration table
  pushes `0x5832b0` + the `"SetValueInt"` string). Called as
  `SetValueInt(node, "ABS_X", screenW-162)` and `(node, "ABS_Y", screenH-210)`.
  Modeled in C as `fastcall` with a dummy `edx` (ecx=this, stack=key,value).
- **Game-thread execution (critical)**: `SetValueInt` mutates UI state / can
  relayout, so calling it from the worker thread risks an intermittent race with
  the renderer. Instead the worker only computes the target and arms a flag; the
  call fires from a hooked **`IDirect3DDevice9::Present`** (frame boundary, game
  thread). We reach Present by vtable-hooking `IDirect3D9::CreateDevice` (index 16)
  on the object returned from `Direct3DCreate9`, then hooking the device's
  `Present` (index 17). A worker-thread fallback applies it after a grace period
  if no Present pump exists (non-d3d9 builds).

## Dead ends (do not relitigate)

- **Resolver-arg rewrite** (set the slot56 `ABS_X` argument to `canvasW-162`):
  panel disappears. `ABS_X` is the resolver *input*, in a virtual GUI canvas
  clipped to ~1365px wide; pushing it past that culls the rect.
- **Raw rect poke** (poll-write the resolved `node+0x64` to `canvasW-W`): on the
  current build the engine re-publishes the rect, and a separate per-frame
  recompute reads a *base* field, so a worker poll is clobbered (or, if written
  mid-resolve, the engine adds `+4096` again → off-screen). The property setter is
  the correct lever.
- **`UI_SetPosition` / vtable[56]**: overridden per class to unrelated handlers
  (407 callers); never a uniform position setter.
- **`.gui` binary `ABS_X` edits**: static, clipped, build-specific; superseded.
- **winmm / dbghelp proxies**: crash via Wine-builtin dependents (see above).
- **Steam Workshop delivery**: ModLauncherEX runs one mod at a time and only
  copies asset folders into the game root; it cannot place a DLL next to the exe.
  Hence the manual d3d9 drop-in is the only mod-agnostic option.

## RE tooling used

Python `capstone` + `pefile` for disassembly/xref:
`python3 -m venv venv && venv/bin/pip install capstone pefile`. Avoid naming a
helper script `dis.py` — it shadows the stdlib `dis` and breaks `capstone`.
