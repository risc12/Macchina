# Studio screen rendering pipeline

How Maschine 2 draws text/UI on the two 480×272 colour screens — and what it means for us.

## The device is a dumb framebuffer
The screens have **no font, no text engine, no character generator**. They only accept
**finished 480×272 RGB565 pixels** (our `0x647344` bulk draw). M2's own call is
`MaschineStudio::Controller::drawDisplay(idx, NI::UIA::Picture const&, …)` — it hands
over a **rasterized bitmap** (`UIA::Picture`), never text. All rendering happens host-side.

## Host-side pipeline (all in Maschine 2)
1. **Lua page logic** (`Contents/Resources/Scripts/**`, e.g. `ScreenMaschineStudio.lua`,
   the `*PageStudio.lua` files) builds/updates the widget tree and sets text/values.
2. **NWL ("NI Widget Library")** — a retained-mode GUI toolkit for the hardware screens:
   `Widget`, `Label`, `Button`, `Bar`, `TextPanel`, `TextEdit`, `Scrollbar`, `LevelMeter`,
   `Stack`, `PaintContext`… Text is drawn by `NWL::PaintContext::drawText`.
3. **CSS-like stylesheets** (`Contents/Resources/skin/stylesheets/**/*.txt`) parsed by
   `NWL::StyleParser`: real selectors (`Label`, `Label.BusyLabel`, `Label:!enabled`),
   properties (`font-name`, `font-size`, `foreground-color`, `horizontal-alignment`,
   `margin-*`, `width/height`), `@import url(...)`, and `@define $var` custom properties.
   Per-widget + per-page rule files (`MixerPage.txt`, `ArrangerPage.txt`, …).
4. **Bundled TTF fonts** (`Contents/Resources/skin/fonts/`): NI's own tiny pixel fonts
   (`Uni05_53v1.ttf`, `Standard0765vR1.ttf`, `prg65_j.ttf` = Pragmatica) + Roboto,
   rasterized by **FreeType statically linked into the binary** (no dylib; internal
   strings `FREETYPE_PROPERTIES` / `glyph-to-script-map` are the tell).
5. Widgets paint into a **`UIA::Picture`** (RGB bitmap = the framebuffer) → `drawDisplay`
   → the `0x647344` push to the device.

**Chain:** Lua page → NWL widget tree → CSS-like skin → FreeType text raster → `UIA::Picture`
→ `drawDisplay` → device.

## Implication for us
To show text/UI on the Studio we own the **entire** rasterization — the hardware gives
nothing. Render into an RGB565 buffer host-side (a small bitmap font, or a TTF via
stb_truetype / FreeType) and push it through the existing draw path
(`NIStudioController -drawDisplay:rgb565Pixels:`). Same approach as M2, minus NWL.
