`xbox.bin`: the Xbox button prompts of "Xbox (and switch) UI for Bloodborne" by Dommo
(https://www.nexusmods.com/bloodborne/mods/30, modern Elden Ring variant), whose assets are free
to use outside paid releases. Built by `tools/engine/button_prompts.py`; shown in game by
`gpu/shim/engine/button_prompts.cpp` while an Xbox-style controller is in use.

Layout: the 20 `KG_*` key guide glyphs of `menu/common.tpf.dcx` in that script's order, each
32x32 BC7 as the game stores it (1024 bytes), then the HUD's Circle, Cross and L3 spots of
`MENU_Common_00091` as 28x32 RGBA8 each.
