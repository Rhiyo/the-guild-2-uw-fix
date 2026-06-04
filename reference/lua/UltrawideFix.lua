-- =====================================================================
-- UltrawideFix.lua  -- standalone ultrawide HUD repositioning
-- ---------------------------------------------------------------------
-- Wiring: add ONE line at the END of Init() in GameHud.lua (after all the
-- AddPanel/AddSheet calls), so this runs once the HUD nodes exist:
--
--     Include("hud/UltrawideFix.lua")
--
-- All logic lives here, so GameHud.lua diverges from other mods by only
-- that single line -- trivial to re-merge after a modpack update.
--
-- Scope:
--   * ButtonPanelRight (bottom-right buttons): fully fixed -- cl_ButtonPanel
--     honors ABS_X, so this moves it to the physical right edge.
--   * CharactersPanel (top-right): this moves the panel's frame, but the 3D
--     character RENDER is positioned in engine code (cl_CharactersPanel) and
--     cannot be moved from Lua -- that part needs the companion DLL. The line
--     below is left in for the frame; remove it if you prefer to disable the
--     panel instead (see DISABLE note at the bottom).
--
-- Resolution-general: reads the game's actual ScreenWidth/Height each run.
-- Lua-sandbox safe: no tostring/os/io; numbers coerce in '..', nils guarded.
-- =====================================================================

local gfx  = FindNode("\\Settings\\GFX")
local root = FindNode("\\GUI\\HudRoot")

if gfx ~= nil and root ~= nil then
	local sw = gfx:GetValueInt("ScreenWidth")
	local sh = gfx:GetValueInt("ScreenHeight")

	if sw ~= nil and sw > 0 then
		local cnt = root:GetChildCnt()
		for i = 0, cnt - 1 do
			local child = root:GetChildAt(i)
			if child ~= nil then
				local pn = child:GetValueString("PanelName") or ""

				if pn == "ButtonPanelRight" then
					local w = child:GetValueInt("ABS_WIDTH")
					if w == nil or w <= 0 then w = 162 end
					child:SetValueInt("ABS_X", sw - w)
					if sh ~= nil and sh > 0 then
						local h = child:GetValueInt("ABS_HEIGHT")
						if h == nil or h <= 0 then h = 210 end
						child:SetValueInt("ABS_Y", sh - h)
					end
					LogMessage("@ULTRAWIDE ButtonPanelRight -> ABS_X=" .. (sw - w))

				elseif pn == "CharactersPanel" then
					-- Frame only. The 3D head needs the DLL (engine-positioned).
					local w = child:GetValueInt("ABS_WIDTH")
					if w == nil or w <= 0 then w = 147 end
					child:SetValueInt("ABS_X", sw - w)
					LogMessage("@ULTRAWIDE CharactersPanel frame -> ABS_X=" .. (sw - w))
				end
			end
		end
	end
else
	LogMessage("@ULTRAWIDE UltrawideFix: \\Settings\\GFX or \\GUI\\HudRoot not found")
end

-- DISABLE alternative (Workshop build without the DLL): instead of moving the
-- CharactersPanel frame above, hide the panel entirely so there is no
-- half-positioned element. Replace the CharactersPanel branch with:
--     child:SetValueInt("VISIBILITY", 0)
