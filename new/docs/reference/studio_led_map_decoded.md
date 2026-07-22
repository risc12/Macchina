# Studio LED map, decoded — s_ledToNHLMap × the M2 Lua enum (2026-07-22)

The authoritative LED→channel map, derived offline by joining two artifacts:

1. `studio_led_map.txt` — the raw `s_ledToNHLMap` dump from the Maschine 2
   arm64 binary (`tLed` enum index → NHL LED channel, −1 = absent on Studio).
2. The official `NI.HW.LED_*` enum order from M2's Lua scripting reference
   (`Maschine 2.app/Contents/Resources/Scripts/**`) — the same order the
   dump is indexed by (modulo a few internal-only entries; see notes).

All channels below are TRUE (count-field) indices. The empirical probe in
`led_map.tsv` ran without the LED count field, so its channels read +4 high.
Cross-checks: every previously live-verified channel (Play=146 etc.) matches.

## Named singles

| M2 name | channel | our capture |
|---|---|---|
| LED_TRANSPORT_LOOP ("Restart") | 142 | button 0x1c ✓ |
| LED_TRANSPORT_PREV  | 103 | id unknown |
| LED_TRANSPORT_NEXT  | 104 | id unknown |
| LED_TRANSPORT_GRID  | 145 | id unknown |
| LED_TRANSPORT_PLAY  | 146 | button 0x1d ✓ |
| LED_TRANSPORT_RECORD| 147 | id unknown |
| LED_TRANSPORT_ERASE | 148 | id unknown |
| LED_SHIFT           | 149 | id unknown |
| LED_TRANSPORT_EVENTS| 144 | id unknown |
| LED_TRANSPORT_METRO | 143 | button 0x1f ✓ |
| LED_CHANNEL         |  40 | button 0x07 ✓ |
| LED_PLUGIN          |  41 | button 0x00 ✓ |
| LED_SCENE … LED_MUTE| 150–157 | 0x23,22,21,20,27,26,25,24 ✓ |
| LED_TAP             | 138 | button 0x08 ✓ |
| LED_ENTER           | 105 | 🟡 likely the jog-wheel push (BUTTON_WHEEL) |
| LED_BACK            | 102 | button 0x37 ✓ |
| LED_MACRO           | 140 | button 0x0a ✓ |
| LED_NOTE_REPEAT     | 141 | button 0x0b ✓ |
| LED_STEP ("Step Mode") | 139 | button 0x09 ✓ |
| LED_BROWSE          |  44 | id unknown |
| LED_SAMPLE ("Sampling") | 45 | id unknown |
| LED_LEFT / LED_RIGHT| 46 / 47 | ids unknown |
| LED_SNAP            |  −1 (no LED on Studio) | |
| LED_ALL             |  48 | id unknown |
| LED_AUTO_WRITE ("Auto") | 49 | id unknown |
| LED_ARRANGE / LED_MIX | 42 / 43 | 0x06 ✓ / id unknown |

## Runs

| M2 names | channels | notes |
|---|---|---|
| LED_GROUP_A–H (RGB base) | 106,109,112,115,118,121,124,127 | R,G,B triplets |
| LED_GROUP_A–H **white**  | 130–137 | 4th component, one per group |
| LED_PAD_1–8 (RGB base)   | 0,3,6,9,12,15,18,21 | |
| LED_PAD_9–16 (RGB base)  | 62,65,68,71,74,77,80,83 | |
| LED_PAD_1–8 **white**    | 24–31 | |
| LED_PAD_9–16 **white**   | 86–93 | |
| LED_JOGWHEEL_RING_1–15   | 198–212 | closed ring |
| LED_JOGWHEEL_EDIT…VOLUME | 192–197 | Edit, Channel, Browse, Tune, Swing, Volume — SEPARATE from the ring |
| LED_LEVEL_LEFT_1–16      | 158–173 | (established live) |
| LED_LEVEL_RIGHT_1–16     | 174–189 | (established live) |
| LED_LEVEL_COLOR          | 190 | (established live) |
| LED_LEVEL_IN1–4          | 54–57 | 0x48–0x4b ✓ |
| LED_LEVEL_MASTER/GROUP/SOUND/CUE | 58–61 | 0x4f,0x4e,0x4d,0x4c ✓ |
| LED_EDIT_COPY…CLEAR      | 94–101 | 0x2b,28,2f,2c,2a,29,2e,2d ✓ |
| LED_SCREEN_BUTTON_1–8    | 32–39 ✓ | live-verified 2026-07-22 |
| LED_LEVEL_MIDI_IN/OUT1–3 | firmware-driven | not 50–53 (poked dark); never referenced in M2's scripts — the device lights them from MIDI activity itself (2026-07-22) |

## Enum-alignment notes

- The dump is 128 words (`x/128xw`) — it truncates after LED_JOGWHEEL_EDIT
  (idx 127 = 192). Everything past that (jog labels 193–197, MIDI levels,
  VU, screen buttons) comes from live probing / the tsv, not the dump.
- The binary enum has a few slots the Lua doc doesn't list (e.g. two −1s
  before TRANSPORT_LOOP, one between PLAY and RECORD). Alignment was pinned
  on unambiguous runs (pads' RGB arithmetic, groups ×3, ring 15) and on
  live-verified anchors (Play=146, Restart=142, Tap=138, Back=102).
- Mikro/MK2-only entries (F1–GROUP, VOLUME…MASTER_RIGHT, CONTROL, SNAP,
  ENTER-as-button) are −1 on Studio in the dump, as expected — except
  LED_ENTER which HAS a channel (105) on Studio, hence the wheel-push theory.
