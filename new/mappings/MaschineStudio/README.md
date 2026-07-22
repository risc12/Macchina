# mappings/MaschineStudio

Per-device hardware mapping data for the Maschine Studio (USB `0x17cc:0x1300`).
Everything here is in **true LED-buffer coordinates** (byte index `0..212` in the
`0x036c7500` message's 213-byte payload).

## Files
- **`structured.md`** — the LED regions the façade drives, verified on hardware:
  RGB pads/groups, both VU meters + colour channel, jog labels, jog ring. Start here.
- **`controls.tsv`** — device-verified per-control map from our on-hardware probe:
  `channel · description · input_id`. This is the button ↔ LED ↔ input-id relationship
  for the controls we actually pressed. Incomplete (only what was probed) but correct.

## Coordinate note
Our probe (`../../docs/reference/led_map.tsv`) was captured before we found the LED
message's `count` header, so its channels were offset **+4**. `controls.tsv` has that
removed — its channels are the real buffer indices and match `structured.md`.

## Ground truth in Maschine 2 (for completing the map)
M2 carries the authoritative tables in its arm64 binary
(`Maschine 2.app/Contents/MacOS/Maschine 2`):
- `…MaschineStudio::Controller>::s_ledToNHLMap` — logical LED (`tLed` enum) → channel.
- `…::s_nhlToButtonMap` — hardware switch id → button (`tButton` enum).
- `…::s_nhlToEncoderMap` — hardware id → encoder.
- `…::s_ledColorToNHLMap`, `…::s_ledStateToNHLMap` — colour/state palettes.

**Caveats before trusting a naive dump of these** (learned the hard way):
1. **RGB LEDs use a two-channel scheme** — `s_ledToNHLMap[pad]` is a *secondary*
   channel, not the RGB-triplet base actually written. Use `structured.md` for pads/groups.
2. Mapping the enum *index* → *name* needs the real C++ `tLed`/`tButton` enum values;
   the Lua `Scripts/Maschine/Prototyping/API1.lua` listing is close but its order does
   **not** match for the pre-ring LEDs (it mislabels e.g. channel 0 as `LED_TAP` when
   channel 0 is verified to be Pad 1). Decode the enum values from the binary before
   auto-generating a full name→channel table.

The verified regions in `structured.md` + the observed `controls.tsv` are trustworthy
today; a full M2-derived table is a follow-up that needs the enum-value decode above.
