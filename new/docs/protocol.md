# Native Instruments Hardware Agent protocol — reverse-engineering notes

This is the canonical, in-repo record of what we've learned about the IPC protocol
that Native Instruments' `NIHardwareAgent` speaks to talk to Maschine software and
hardware. It is the crown jewels of this project — keep it current as you learn more.

> Status legend: ✅ understood · 🟡 partial / guessed · ❓ unknown ("boh" = Italian
> shrug; the original author used `boh`/`bohN` as field names for bytes he hadn't
> decoded yet — every `boh` is an open question).

## Transport

All communication is **local CFMessagePort IPC** (`CFMessagePortCreateLocal` /
`CFMessagePortCreateRemote`), not USB directly. The real `NIHardwareAgent` owns the
USB link; everything here talks to *it* (or impersonates it).

- `NIClient` (`NICommon/NIClient.m`) — wraps a remote port, `sendMessage:` →
  `CFMessagePortSendRequest`, returns the reply `NSData`.
- `NIServer` (`NICommon/NIServer.m`) — creates a local port, dispatches inbound
  messages via `ServerPortCallback` and a run-loop source.

### Known port names

| Port name                                   | Owner        | Purpose |
|---------------------------------------------|--------------|---------|
| `NIHWMainHandler`                           | agent        | connection / handshake, version queries |
| `NIHWMaschineController0001Request`         | agent        | control requests to the controller |
| `NIHWMaschineController0001RequestMidi`     | agent        | MIDI-side request channel 🟡 |
| `NIHWMaschineController0001Notification`    | client       | agent → client events (pads, wheels, state) |

### Port naming, decoded ✅ (live-tested 2026-07, agent build 2023-09)
`<NIHW><DeviceName><InstanceCounter><Request|Notification>` where:
- **DeviceName** derives from the `controllerId` in `NIDeviceConnectMessage` — which
  is the **USB product id** (NI vendor id `0x17cc`). `0x0808` → `MaschineMK1…`,
  `0x1300` → `MaschineStudioController…` (verified against a physical Studio).
- **InstanceCounter** (`0068`, `0076`, …) is a per-connection session counter, not a
  device id — it increments on every connect, and stale ports linger in the
  bootstrap namespace after a client dies.
- ⚠️ **The agent does not validate the requested device**: connecting with an id for
  hardware that isn't attached still returns `success` + working (phantom) ports.
  The tell: the first `0x03444e00` notification carries `0` (absent) vs `'true'`
  (present).

## Message framing

Every message is a length-prefixed-by-nothing blob whose **first 4 bytes are a
little-endian `messageId`** (`NIMessage messageFromData:`). `ClassForMessageID()`
maps the id to a concrete `NIMessage` subclass which then parses the remainder.

### Message ID table (from `NICommon/NIMessage.m`)

| messageId    | Class                          | Base type          | Notes |
|--------------|--------------------------------|--------------------|-------|
| `0x02536756` | NIGetServiceVersionMessage     | NIPlainMessage     | ✅ |
| `0x02444300` | NIDeviceConnectMessage         | (custom)           | ✅ handshake; `boh`, `clientRole`, `clientName` |
| `0x02404300` | NISetAsciiStringMessage        | (custom)           | 🟡 `boh1`,`boh2` unknown |
| `0x02446724` | NIGetDeviceEnabledMessage      | NINumberValueMessage | ✅ |
| `0x02444e00` | NIDeviceStateChangeMessage     | NINumberValueMessage | ✅ |
| `0x02446743` | NIGetDeviceAvailableMessage    | NINumberValueMessage | ✅ |
| `0x02434e00` | NISetFocusMessage              | NINumberValueMessage | ✅ |
| `0x02434300` | ControllerAcquire ("CC")       | body 4CC `'strt'`  | ✅ **claims display focus — required before any Studio draw** (2026-07) |
| `0x02446744` | NIGetDriverVersionMessage      | NIPlainMessage     | ✅ |
| `0x02436746` | NIGetFirmwareVersionMessage    | NIPlainMessage     | ✅ |
| `0x02436753` | NIGetSerialNumberMessage       | NIPlainMessage     | ✅ |
| `0x02646749` | NIGetDisplayInvertedMessage    | NINumberValueMessage | ✅ |
| `0x02646743` | NIGetDisplayContrastMessage    | NINumberValueMessage | ✅ |
| `0x02646742` | NIGetDisplayBacklightMessage   | NINumberValueMessage | ✅ |
| `0x02566766` | NIGetFloatPropertyMessage      | NINumberValueMessage | 🟡 |
| `0x02647344` | NIDisplayDrawMessage           | (custom)           | ✅ pushes image to a display |
| `0x02654e00` | NIWheelsChangedMessage         | (custom, events)   | ✅ inbound event |
| `0x02504e00` | NIPadsChangedMessage           | (custom, events)   | ✅ inbound event |
| `0x026c7500` | NISetLedStateMessage           | (custom)           | ✅ full-panel LED state |

Note the ids look like packed 4-char tags (`0x02_dd_43_00` ≈ `\x02 D C \0`), so the
scheme is probably `0x02` + a mnemonic — a lead worth chasing to *predict* ids for
messages we haven't seen yet.

### Other magic constants seen in the wild
- `0x00000808` — used in `NIAgentClient.m` / `NIMainHandlerServer.m` (client role?) 🟡
- `0x00010400` — `NIMainHandlerServer.m` 🟡
- `0xabababab`, `0xcdcdcdcd` — the two words before the port-name in the v2
  `SetAsciiString`/`MessageSetString` (`0x404300`) register frame. **Decoded (2026-07):
  they must be ZERO for v3** — Maschine 2's `MessageSetString` leaves that 8-byte field
  unset (zero). Our placeholders were harmless here but wrong; use `00…00`.

## Two controller families in the agent 🟡 (from disassembly, 2026-07)

The agent (build 2023-09, `NI::NHL2::SERVER::*` C++) drives controllers through two
distinct code paths — a **transport/architecture split, not an era split** (the
Maschine Studio is from 2013, barely younger than this codebase):

- **MK1 path** — what this repo speaks. Proprietary transfers, mono ST7529 displays,
  display draw tag `0x647344` (`Dsd`).
- **`MaschineHIDController` path** — Maschine Studio and later. HID transport, color
  displays (Studio: 2× 480×272 TFT). **Update (2026-07): the Studio's *display* still
  uses the `0x647344` (`Dsd`) draw tag** — `MaschineStudio::onDisplayMessage` handles it
  and pushes via `BulkDisplayImplementation::onDrawDisplayMessage` — but only after the
  client **acquires display focus** (`0x434300`+'strt'). Payload is a `Display::Bulk`
  stream (WriteWindowRequest fmt 0x20/0x60 + pixels + EndOfUpdate), not raw ST7529. Full
  writeup: [studio-display.md](studio-display.md). (`0x447340`/`onDeviceSpecificMessage`
  is a separate device-specific channel, still undecoded ❓.)

Live consequence (tested against a real Studio): the legacy (v2) handshake half-works —
connect succeeds and `GetSerialNumber`/`GetFirmwareVersion`/`GetDeviceAvailable` return
real hardware data — but a v2 draw is **rejected** and no input flows. **The claim step
was found (2026-07): it is the v3 controller connection + display-focus acquire, not a
mode switch.** `setFocus(true)`, `0x02446724(true)` and `deviceStateChange(true)` are all
no-ops for this; the working sequence is v3 `DeviceConnect 0x1300` → `0x434300`+'strt'
(acquire focus) → `0x647344` Bulk draw on the controller port. See
[studio-display.md](studio-display.md) and `../new/docs/longform_research.md`.

### Message dispatch, decoded from disassembly ✅
- The agent dispatches on **`messageId & 0x00ffffff`** (3-byte tag); the top byte is
  a class/direction prefix: `0x02` = request, `0x03` = async notification (confirmed
  live: `0x03444e00` = notification twin of `0x02444e00` DeviceStateChange).
- Reply shape: get-style requests answer with data (`u32 length`-prefixed ASCII for
  strings — e.g. serial number); set-style requests answer **empty** on OK, and
  `0x00000000` = rejected.
- `0x444e2b` "DN+" device-added notification — **decoded ✅** (live 2026-07,
  `new/docs/LEARNINGS.md` E1): `[msgid][handle u32][product u32][len u32]
  [serial UTF-16LE + NUL]` (len counts characters incl. the terminator; e.g. 9 for
  `"39195855"`). It is the first frame a fresh subscriber's receive port gets — the
  device is keyed by a **UTF-16 serial**. `0x444e2d` `DN-` presumably the removed
  twin (unverified 🟡). `0x444900` = the v3 DeviceConnect (see handshake below) ✅.
- Still undecoded: `0x44734c`, `0x44674c`, `0x734e00` (hottest unknown, 21 sites),
  `0x657543`/`0x636552` (`Cue`/`Rec`, transport). ❓
- Modern event vocabulary (symbol names): `MsgPadsChanged`, `MsgPotisChanged`,
  `MsgEncodersChanged`, `MsgJogwheelsChanged`, `MsgSwitchesChanged`,
  `MsgTouchControlsChanged`, `MsgPedalsChanged`.
- Input routing is gated by focus — `ControllerBase::setFocusClientByID(uint,bool)`,
  `chooseNewFocusClient` — driven by `ClientProperties` from the connect message;
  which properties qualify is undecoded ❓ (prime suspect: the `boh`/`clientRole`
  fields).
- Mode switch: `ControllerBase::setDeviceMode`/`toggleMode` (the hardware's
  SHIFT+CHANNEL toggles MIDI mode ↔ host mode).

## Protocol v3 handshake (Maschine 2 → agent) 🟡 (live capture 2026-07)

Maschine **2** speaks a newer protocol than the 2012 code (which is v2). Captured by
impersonating the agent (`MacchinaServer` + NITooling handlers) and cross-checked
against the real agent (an "oracle" replay). The top byte of every id is a
**negotiated version**: the client starts at `0x02`, and once we answer
`GetServiceVersion` it switches to `0x03` for subsequent messages.

Observed boot sequence on `NIHWMainHandler`:
1. `0x03536756` GetServiceVersion → real reply **8 bytes**: `version=0x00020802`,
   `count=3` (the 2012 v2 reply was 4 bytes — answering the v2 way keeps the client
   in v2 and it never advances).
2. `0x03447500` "Du" — a **connect that doubles as device enumeration**: it is sent
   once per supported USB product id (`0x1500, 0x1300, 0x1140, 0x0808, …`), carrying
   client identity `boh='NiM2'` (Maschine 2 software; MK1's was `'NiMS'`) and
   `role='prmy'`. Reply for a *present* device = `'true'` + request/notification
   port-name pair (63 bytes, same layout as the v2 `0x02444300` DeviceConnect);
   absent = `0x00000000`. v3 port names use a new scheme: `NIHWS<productid><counter>`
   (e.g. `NIHWS13000044Request`), distinct from the v2
   `NIHWMaschineStudioController00NN` names.
3. On the returned request port: `0x03404300` SetAsciiString registers the client's
   notification port (its `boh` fields differ from the v2 `abababab/cdcdcdcd`).
4. `0x03447143` — a void/set op (`IPCServer::onSubscriberMessage` branch that
   enumerates subscribers); the real agent replies **empty (0 bytes)**. Answering
   `'true'` here is malformed and stalls the client.
5. `0x03444900` DeviceConnect — **live-verified as a client ✅** (2026-07,
   `new/docs/LEARNINGS.md` E3–E4): `[msgid][product][clientTag][role][len]
   [ASCII serial + NUL]` → `'true'` + port pair
   (`NIHWMaschineStudioController-<serial>NNNN…`). The `0x03` prefix is mandatory:
   with `0x02` the agent refuses with the 4CC **`'dice'`** (HIA's equivalent:
   `'ectl'`) = wrong-protocol/invalid-connect, *not* device-busy. Note the version
   quirk: our raw read of GetServiceVersion returned only 4 bytes (`0x00020802`) —
   any version ≥ `0x00020000` still means "speak v3".

### ✅ Connection → adoption: SOLVED (was "Connection ≠ adoption")
The full connect recipe — v3 negotiate, `Du` subscribe + register + subscribe-op,
`DeviceConnect` + register + subscribe-op — completes and `GetSerialNumber` answers on
the controller port, but that alone is **not** enough: a `0x03647344` draw is still
rejected. The missing step is **display focus** — send `0x03434300` + body `'strt'`
(`onControllerAcquire → setFocusClient`, storing our id at `controller+0x150`) on the
controller request port. `ControllerBase::onClientRequest` accepts a draw only when
`controller+0x150 == sender`. With focus, draws are accepted (`'true'`) and pushed to
USB via `BulkDisplayImplementation::onDrawDisplayMessage` — both panels, live-verified.
Draws ride the **controller** connection (not the subscription — an early wrong model)
and `NIHardwareAgent` renders directly (not HIA). Full experiment log:
`new/docs/LEARNINGS.md` (E17); narrative: `new/docs/longform_research.md`.

### ⚠️ Impersonation is incomplete
Feeding correct-shaped replies for all of the above gets Maschine 2 through the
handshake **but it still does not add the controller** (Preferences → Controller
stays empty) and never emits display draws. Something past the handshake — device
capability readback, a notification the agent normally pushes once the USB device
responds, or a validation we haven't found — gates adoption. This is the open blocker
for the capture approach. Candidates to investigate: the unscheduled
`…RequestMidi` port (NIAgent creates but never run-loops it), and
`IPCServer::connectControllerClient` / device-manager instantiation in the
disassembly.

## Handshake (client → agent)

`NIMainHandlerClient connectToControllerWithId:boh:clientRole:clientName:` sends an
`NIDeviceConnectMessage` on `NIHWMainHandler` and gets back an
`NIDeviceConnectResponse`. `clientRole` distinguishes standalone vs plugin vs editor
(exact values 🟡). After connecting, the client stands up its own
`NIHWMaschineController0001Notification` server to receive events.

## Displays — image encoding ✅ (mostly)

`NIDisplayDrawMessage` carries an **ST7529-encoded** image (`st7529EncodedImage`).
`NI24BPPToST7529Data(width, height, bitmap)` in `NICommon/NIImageConversions.m`
converts a 24bpp bitmap to the display's native format. The Maschine displays are
driven by an ST7529-class LCD controller. `TestImageDataMessage()` builds a canned
draw message from `TestImage.h` for smoke-testing.

### Wire layout ✅ (decoded from `-dataRepresentation`)
`NIDisplayDrawMessage` is a **partial-rectangle blit**, not a full-screen push — it
carries an origin and size, so NI can update only dirty regions:

| offset | size | field | notes |
|--------|------|-------|-------|
| `0x00` | u32  | messageId       | `0x02647344` |
| `0x04` | u32  | displayNumber   | OR'd with marker `0x10000000` on the wire |
| `0x08` | u16  | originY         | |
| `0x0a` | u16  | originX         | |
| `0x0c` | u16  | sizeHeight      | |
| `0x0e` | u16  | sizeWidth       | |
| `0x10` | u32  | payloadSize     | length of the ST7529 blob that follows |
| `0x14` | …    | st7529EncodedImage | `payloadSize` bytes |

> Our client only ever sends one full-screen rect at (0,0); the *protocol* supports
> dirty-rect updates. Capturing NI's own draws (below) should reveal its real
> rectangle strategy.

### Maschine Studio displays — ✅ working, see [studio-display.md](studio-display.md)
The MK1's ST7529 path below is **not** how the Studio's two 480×272 colour panels work.
The Studio takes the same `0x647344` "Dsd" draw tag (via `MaschineStudio::onDisplayMessage`)
but the payload is a **`Display::Bulk` command stream** — WriteWindowRequest (op `0x0084`,
`DataFormat` `0x20` uncompressed / `0x60` compressed) + 2-byte/px RGB565 pixels +
EndOfUpdateRequest — **not** ST7529, and only after the client holds display focus
(`0x434300`+'strt'). A from-scratch client now drives both panels this way. There is also a
separate semantic path `0x534d6170` "SMap" (agent-side renderer) — un-attempted (Path B).
Full recipe, wire layout, and the client→controller tag map: `new/docs/studio-display.md`.

### Payload bit depth 🟡 (MK1 / Mikro — ST7529)
The encoder thresholds every pixel to pure black/white (`srcColor ? 0x1F : 0x00`) and
bit-packs **5 bits/pixel, 3 px → 2 bytes**. The 5-bit field (`0x1F`) strongly hints
the panel is **grayscale**, not 1-bit — NI may send intermediate levels our encoder
throws away. "ST7529" itself is a working assumption, not confirmed. `NITooling`'s
decoder (`NIST7529DataToGray8` / `NIST7529WritePGM`) expands the 5-bit levels to
8-bit and dumps a PGM so captured payloads can be eyeballed for gray vs. B/W vs.
compression. **Open: confirm bit depth and packing against a real captured draw.**

## LEDs ✅

Legacy (MK1): `NILedState` is a flat intensity map (`setLed:intensity:` /
`getLedIntensity:`), serialized by `dataRepresentation` and shipped in
`NISetLedStateMessage` as `[u32 msgid][u32 n=57][57 level bytes]`.

**Maschine Studio, v3 (live-verified 2026-07, CORRECTED):** the LED-set message is
`[u32 msgid = 0x036c7500][u32 count = 213][213 level bytes]` — the count field **is
required** (M2 sends it; the earlier "no count field" note here was wrong). Without it
the buffer lands shifted **−4** on the device and the last 4 channels truncate — which
is exactly the state the channel map below was probed in, so **the table below reads
+4 high; subtract 4 for true indices**. The authoritative un-shifted map is
[reference/studio_led_map_decoded.md](reference/studio_led_map_decoded.md).
One byte = one LED channel's intensity, with these live-verified traits:
- **`0x00` = OFF** (level 0), `0x7f` = full. Writes always apply — `0` is NOT "no change".
  Verified: `all 0x7f` then set one channel `0x00` → that LED goes dark. (Earlier "0 = no
  change" notes were wrong — an artifact of a stale second client and the dedup below.)
- **The agent only re-pushes a changed HID report.** `MaschineStudio::updateLEDs` compares
  each computed report to the agent's last-sent copy and skips USB if unchanged. After churn
  (another client, a daemon restart) that cache goes stale, so a lone single-channel change
  may not reach USB while a full-buffer write does. Practical fix: **prime with a full write
  (`all <level>`)** to force a resync, then individual channels behave.
- **Intensity is 7-bit:** bit 7 selects a factored mode — `0x00`–`0x7f` = direct level
  `min(byte,127)`; `0x80`–`0xff` = `f1 + f2·(byte&0x7f)` through global level factors
  (`getLEDLevelFactors`, default ≈ `[13,127]`). So a naive 1→255 ramp looks like it "wraps"
  at 128 — that's the mode switch, not a wrap. M2 only uses direct-mode values in practice.
- Only ONE client may drive LEDs at a time; kill stale clients (`pkill`) before testing. 213 = `0xd5`, matching
the `LEDsV1(213)` constructor. `MaschineStudio::updateLEDs`
(agent arm64 `0x100039580`) scales each byte by a global brightness factor and pushes
them to USB as **four HID reports**: `0x80` (channels 0–61), `0x81` (62–105), `0x82`
(106–157), `0x83` (158–212). Sending all-`0xff` lights ~every button/pad; the
`in`/`out1..3` LEDs are the **MIDI** in/out status LEDs (the Studio has MIDI I/O, no
audio interface), driven separately and outside these 213 channels.
### Studio LED channel map (live-mapped 2026-07, via `LedProbe`/`LedPoke`)

⚠️ **Probed without the count field — every channel below is +4 high** (see above).
Prefer [reference/studio_led_map_decoded.md](reference/studio_led_map_decoded.md):
the `s_ledToNHLMap` dump joined with M2's official `LED_*` enum order, which also
decoded (2026-07-22): **white channels** for pads (true ch 24–31 / 86–93) and groups
(130–137), the jog **labels** at true 192–197 (separate from the ring at 198–212),
Prev/Next at 103/104, Auto at 49, and `LED_ENTER` at 105 (wheel push? 🟡).

RGB elements are 3 consecutive channels in **R, G, B** order; the rest are mono.

| channels | element |
|---|---|
| 0–3 | unused (dark) |
| **4–27** | **Pads 1–8** — RGB, `pad k → 4+3(k−1)`; input padIds 12,13,14,15,8,9,10,11 |
| 36–43 | buttons above the screen (mono) |
| 44–52 | left column: Channel, Plug-in, Arrange, Mix, Browse, Sampling, `<`, `>`, All |
| 58–65 | right meters: In 1–4 (`0x48–4b`), MST `0x4f`, GRP `0x4e`, SND `0x4d`, CUE `0x4c` |
| **66–89** | **Pads 9–16** — RGB, `pad k → 66+3(k−9)`; input padIds 4,5,6,7,0,1,2,3 |
| 98–105 | edit: Copy, Paste, Note, Nudge, Undo, Redo, Quantize, Clear (`0x28–0x2f`) |
| 106 | Back |
| **110–133** | **Groups A–H** — RGB, `group g → 110+3g` |
| 142–145 | Tap, Step Mode, Macro, Note Repeat (`0x08–0x0b`) |
| 146–153 | transport: Restart, Metro, Events, Grid, Play, Rec, Erase, Shift |
| 154–158 | Scene, Pattern, Pad Mode, Navigate, Duplicate (`0x23,22,21,20,27`) |
| 159–161 | Select, Solo, Mute (`0x26,25,24`) |
| **162–177** | **Left VU** — 16 mono segments (bottom→top, top = red) |
| **178–193** | **Right VU** — 16 mono segments |
| 196–201 | jog labels: Edit, Channel, Browser, Tune, Swing, Volume |
| **202–212** | **jog ring** — 11 mono segments (top, then clockwise). The left→top arc has no channel (1:1 confirmed) so it can't be lit individually. |

`NIStudioController` exposes `-setPadLED:red:green:blue:`, `-setGroupLED:…`,
`-setVU:segments:`, and the region constants. Input ids above are partial.

## Inbound events ✅

All arrive on the focused controller's **Notification** port, one message per gesture,
sharing a common 16-byte header then `count` fixed-size records:

```
u32 messageId
u32 seq          // per-gesture counter (constant across one touch/turn, bumps between)
u32 timestampNs  // low 32 bits of a nanosecond clock
u32 count        // number of records that follow
```

Studio v3 message ids + record layouts (live-verified 2026-07):

| msgId | control | record | notes |
|---|---|---|---|
| `0x03504e00` | **Pads** | `u32 padId · u32 state · f32 pressure` | padId 0–15; state `1`=hit `4`=pressure `3`=release; pressure 0.0–1.0 |
| `0x03734e00` | **Switches** | `u32 switchId · u32 state` | buttons AND touch-sensitive knob touch-sensors; **bit 0 of state = down** (live capture 2026-07: press `0x3f800b01`, release `0x3f800b00`). The boh upper bytes look like an f32 `1.0` with unknown bits `0x0b` mixed in ❓ — the earlier "f32 `1.0`=down `0.0`=up" reading was wrong (≥0.5 on both made releases decode as down). PLAY=`0x1d`; a knob touch seen as `0x58` |
| `0x03654e00` | **Knobs** (touch encoders, no detents) | `u32 id · f32 delta` | fine continuous deltas (~0.005/tick) |
| `0x03774e00` | **Big wheel** (detented) | `u32 id · i32 delta` | discrete integer step per notch (+1/-1) |

A touch-sensitive knob turn emits, under one `seq`: switch `id`=down → knob deltas →
switch `id`=up. Legacy names: `NIPadsChangedMessage` (`0x02504e00`),
`NIWheelsChangedMessage` (`0x02654e00`). The id → physical-control map is **TODO**.

## Live-verified facts (Maschine Studio, 2026-07 session)
- USB: vendor `0x17cc`, product `0x1300` (from `ioreg`). Device: Maschine Studio.
- `GetSerialNumber` → `39195855` (u32-length-prefixed ASCII + NUL).
- `GetFirmwareVersion` → `0x21` (33). `GetDriverVersion` → `1`.
- `0x02446724` ("GetDeviceEnabled") behaves like a **Set** (empty reply) — probably
  `setMIDITransportEnabled`; rename candidate once confirmed.

## Open questions / next targets
- [x] **Find the device-claim step** that takes a Studio out of standalone mode —
      **it's display-focus acquire** (`0x434300`+'strt') on a v3 controller connection,
      not a mode switch. See [studio-display.md](studio-display.md).
- [ ] Decode the separate device-specific channel `0x447340`
      (`MaschineHIDController::onDeviceSpecificMessage`) — distinct from the `0x647344`
      display draw the Studio actually uses; purpose still unknown.
- [x] Route **input events** (pads/wheels/encoders) and **LEDs** over the focused
      controller connection — **done (2026-07)**: input events arrive on the controller
      Notification port; LEDs via `0x036c7500` + 213 raw bytes. See the LEDs / Inbound
      events sections above.
- [ ] Decode every `boh*` field (capture real traffic against `NIHardwareAgent`).
- [ ] Confirm `clientRole` enum values / which `ClientProperties` win focus.
- [ ] Document the LED index map and the pad-id layout.
- [ ] Generalize port names + device id for non-Maschine NI controllers.
- [x] Verify the `0x02 + mnemonic` messageId theory — confirmed: dispatch masks
      `id & 0xffffff`; `0x02` request / `0x03` notification.

## How to capture more (method)
Run `MacchinaServer` with the real `NIHardwareAgent` *off* to impersonate the agent
and log what NI's own software sends (see README "To run MacchinaServer"). Dump raw
`CFData` at `ServerPortCallback` and diff against these tables to spot new ids/fields.

### Tooling (`new/tools/attic/`) — kept separate from `NICommon`
Reverse-engineering aids that don't change library behaviour. Build/run with
`./build.sh tests` (headless round-trip tests) and `./build.sh server-tap`
(MacchinaServer linked with the tooling).

- **`NIHexDump` / `NIDescribeFrame`** — turn raw frames into offset/hex/ASCII dumps
  and identify `messageId → class` without parsing (safe on unknown frames).
- **`NICaptureTap`** — swizzles `NIServer -handleIncomingData:` to log every raw
  frame and append it to a replayable per-port capture file. Auto-installs when
  `MACCHINA_CAPTURE_DIR` is set — no edit to `main.m` needed. File format per
  record: `u32 length` + `length` bytes.
- **`NIReplay`** — reads capture files back; `NIDescribeCaptureFile` inspects them
  offline, `NISendFramesToPort` injects them into a live port (e.g. replay a
  captured draw into the **real** agent to watch the physical panel).
- **`NIDisplayDrawCapture`** — categories that (a) give `NIDisplayDrawMessage` an
  `+messageFromData:` so the server can actually *parse* inbound draws (previously
  it fell through to `NIMessage`'s default and recursed on itself), and (b) add
  `-handleNIDisplayDrawMessage:` to `NIControllerRequestServer` to hexdump the
  payload, dump a PGM, and return a plausible `'true'` ACK so NI's app doesn't stall.
- **`NIImageDecode`** — the inverse of `NI24BPPToST7529Data` (see "Payload bit depth").
