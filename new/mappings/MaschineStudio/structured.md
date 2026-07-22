# Maschine Studio — structured LED regions (device-verified)

Channels are byte indices `0..212` in the 213-byte LED buffer
(`[msgid 0x036c7500][u32 count=213][213 level bytes]`). Level is 7-bit: `0` = off,
`0x7f` = full. These are the regions the façade (`new/studio/NIStudioController`)
drives and that we verified on hardware.

## RGB elements (3 consecutive channels each: R, G, B)

| element | channels | notes |
|---|---|---|
| **Pads 1–8** | `0 … 23` | pad *k* → `3(k−1) + {0,1,2}` |
| **Pads 9–16** | `62 … 85` | pad *k* → `62 + 3(k−9) + {0,1,2}` |
| **Groups A–H** | `106 … 129` | group *g* → `106 + 3g + {0,1,2}` |

## Mono elements (1 channel = brightness of one LED)

| element | channels | notes |
|---|---|---|
| **VU left** | `158 … 173` | 16 segments, bottom→top |
| **VU right** | `174 … 189` | 16 segments, bottom→top |
| **VU colour** | `190` | `0` = blue (input) · `≥1` = white (output); flips **both** meters |
| **Jog labels** | `192 … 197` | EDIT, CHANNEL, BROWSE, TUNE, SWING, VOLUME (in order) |
| **Jog ring** | `198 … 212` | 15 segments, `198` = top-left, clockwise; all 15 lit = closed ring |

## Input events (separate id space from LED channels)

Input arrives on the controller Notification port; each control has an **input id**
that is *not* the same number as its LED channel — see `controls.tsv` for the
observed pairing per control.

| event | msgId | record |
|---|---|---|
| Pads | `0x03504e00` | `padId(0–15)`, state (1=hit,4=pressure,3=release), f32 pressure |
| Switches (buttons + knob-touch) | `0x03734e00` | `switchId`, f32 state (1=down) |
| Knobs (touch encoders) | `0x03654e00` | `id`, f32 delta |
| Jog wheel (detented) | `0x03774e00` | `id`, i32 step |

**Pad LED-order ↔ input padId** (partially probed): LED pad 1→id 12, 2→13, 9→4,
16→3 (full row order is 12,13,14,15,8,9,10,11 then 4,5,6,7,0,1,2,3).
