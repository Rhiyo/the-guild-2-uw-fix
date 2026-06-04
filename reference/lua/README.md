# Lua reference (not used by the build)

These scripts are an **earlier, Lua-based** approach to the ultrawide fix, kept
for reference only. They are **not** shipped or built — the DLL (`src/`) is the
real fix. They are valuable because they prove the lever the DLL now uses.

- **`UltrawideFix.lua`** — standalone HUD repositioner. The key line,
  `child:SetValueInt("ABS_X", sw - w)`, is exactly what the DLL replicates by
  calling the engine's `SetValueInt` (`0x794ff0`) on the `ButtonPanelRight` node
  from the game thread. Note its own comment: the 3D character *head* cannot be
  moved from Lua (engine-positioned) and needs the DLL's code cave.
- **`GameHud.lua`** — the modpack's HUD registration script, included for context
  (it shows how `ButtonPanelRight` / `CharactersPanel` are registered as panels).

Why Lua isn't the shipped fix: distributing a Workshop mod can't carry a DLL, the
head can't be moved from Lua, and the DLL works mod-agnostically (vanilla or
modpack) without touching any game/script files. See `docs/REVERSE_ENGINEERING.md`.
