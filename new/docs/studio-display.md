# Driving the Maschine Studio display — research & plan

Goal 3 of "build a client that drives the Studio": get pixels onto the Studio's two
480×272 colour TFTs. This is the hard one. Everything below is reverse-engineered
from the `NIHardwareAgent` binary (build 2023-09, x86_64, `NI::NHL2::SERVER::*`) with
`otool -tv`, cross-checked against live behaviour where noted.

> Context: goals 1 (LEDs) and 2 (input) are believed straightforward — the LED message
> (`0x6c7500`) and the input-event notifications are already understood. This doc is
> only about the display.

## ✅ SOLVED (2026-07) — a from-scratch client drives BOTH Studio panels over USB

The blocker was never the pixel format or the payload — it was **display focus**. The
Studio controller accepts a `0x647344` draw only from the *focus client*:
`ControllerBase::onClientRequest` guards it with `ldar x22,[controller+0x150]; cmp x22,
sender; b.eq accept; else MsgDisplayDraw::setResult(false)`. `controller+0x150` is the
focus-client id, set by `onControllerAcquire → setFocusClient(sender, true)`, which is
dispatched for message tag **`0x434300`**. Proof this was the wall: replaying Maschine 2's
*verbatim* captured draw frame from our own connection still rejected — until we acquired
focus, after which it (and our synthetic fills) are accepted (`'true'`) and pushed to USB
via `BulkDisplayImplementation::onDrawDisplayMessage` (verified 1:1 with `onDisplayMessage`,
both panels).

**Working recipe (implemented in `new/clients/StudioDemo`, `MACCHINA_LOOP_DRAW=1`):**
1. Connect `NIHWMainHandler`, **force v3 prefix `0x03`** (version replies mislead).
2. `Du subscribe 0x1300` → register receive port via `0x404300` **with the 8-byte field
   ZERO** (the old `0xabab…` sentinels were wrong) → `subscribeVoidOp 0x447143`.
3. `DeviceConnect 0x1300` (ASCII serial) → local controller port; register + subscribeVoidOp.
4. **ACQUIRE FOCUS: send `0x03434300` + body `'strt'` (`0x73747274`) on the controller
   request port.** ← the missing step.
5. Draw `0x03647344` on the controller port (see Path A below): WriteWindowRequest (op
   `0x0084`, id=display, format `0x20` uncompressed *or* `0x60` compressed) + pixels +
   EndOfUpdateRequest. Now accepted and rendered.

Full disassembly-level narrative: [../new/docs/longform_research.md](../new/docs/longform_research.md)
(Findings 1–6) and [../new/docs/LEARNINGS.md](../new/docs/LEARNINGS.md) E17.

> **Everything below predates the solve and is kept for the research trail only.** Two
> of its headline conclusions were later **disproven** — read them as "what we believed
> mid-investigation", not as fact:
> - *"The real gate is device MODE (`this+0x39` / `SetDeviceMode "APP "`)"* — **wrong.**
>   The gate is display **focus** (`this+0x150 == sender`), claimed with `0x434300`+'strt'.
>   `this+0x39` turned out to be a per-display active flag that is already 1 for both
>   panels; `SetDeviceMode` was never needed.
> - *"The precondition is display attachment / adoption (`this+0x150` is an empty
>   collection)"* — **wrong.** `this+0x150` is the **focus-client id**, not a collection;
>   it's populated by `onControllerAcquire`, which our client now calls.
>
> The Path A envelope below was essentially right (it just needed focus first, and the
> payload is a Bulk stream — see the recipe above). Path B (semantic "SMap") is still
> un-attempted and its notes stand.

## Two independent ways to drive the display

The agent exposes **both** a raw-bitmap path and a semantic UI-model path. They are
separate message types and separate code paths.

### Path A — raw bitmap: `0x647344` "Dsd" (DisplayDraw) ✅ path confirmed
`MaschineStudio::onDisplayMessage` handles tag `0x647344` — the **same tag the MK1
uses** — and forwards it to a display handler (`this+0x310`, vtable slot `0x70`). So
the Studio *does* accept a client-pushed bitmap; our earlier rejection (reply
`0x00000000`) was **wrong dimensions + wrong pixel format**, not a missing path.

- **Envelope**: reuse `NIDisplayDrawMessage` verbatim — `{msgid, displayNumber, y, x,
  h, w, size, payload}` (see [protocol.md](protocol.md)). `displayNumber` selects the
  panel (0 / 1).
- **Pixel format**: NOT ST7529. The agent encodes via
  `Display::Bulk::renderPixelData(Picture&, …, DataFormat, …)`.
  `Display::getDataFormatProperties(DataFormat, &bytesPerPixel, &x, &bool)` is a jump
  table over the `DataFormat` enum (values from ~`0x12`); it yields **bytes-per-pixel
  ∈ {1, 2, 4}**. A colour TFT ⇒ almost certainly the **2 bytes/pixel** branch =
  **RGB565** (little-endian likely). `Display::adjustUpdateRectForFormat(Rect&,
  DataFormat)` implies dirty-rect coordinates may need alignment to format boundaries.
- **Geometry**: 480×272 per panel (known Studio hardware spec; the agent reads it from
  device properties rather than an inline constant, so **confirm empirically**).
- **USB side (for reference, not what the client sends)**: agent renders →
  `LCDDisplay::drawDisplayEx(displayNum, Picture&, Rect*, Rect*)` →
  `Display::GenericHID::drawBulk` → `sendBulkDisplayRequest` (USB bulk).

**Why start here**: the envelope is already implemented; only two unknowns remain
(exact `DataFormat` + exact dimensions), and both fall out of a handful of live draws
watched on the physical panel. Full pixel control = the project's actual endgame.

**⚠️ [SUPERSEDED — see the SOLVED banner at the top] The real gate is device MODE, not
pixel format (confirmed in disasm).** *(Wrong: the gate is display focus, not mode. Kept
for the trail.)*
`MaschineStudio::onDisplayMessage` only draws when `this+0x39 != 0`, and
`ControllerBase::onClientRequest` replies **`0x00000000`** to a draw exactly when
`this+0x39 == 0` (`cmpb $0x0,0x39(%r14); cmovel` → 0 vs `'true'`). `this+0x39` is the
"device opened for host control" flag, set in `ControllerBase::connect(USBDeviceOSHandle)`.
It is 0 while the Studio is in standalone/MIDI mode (its idle animation). **Every
pixel-format/geometry attempt returns `0x00000000` regardless — because the gate is
closed, not because the format is wrong.**

**The key: `SetDeviceMode` = tag `0x44734d` "Ds M".** `ControllerBase::setDeviceMode`
accepts a mode 4CC at msg+0x14: **`0x41505020` "APP "** (host/application mode) or
**`0x4d494449` "MIDI"** (standalone). Send `SetDeviceMode("APP ")` first to put the
Studio in host mode → agent opens it for host control → `this+0x39=1` → draw gate opens.
This likely also unblocks input-event routing (goal 2) and is the same mode switch the
"adoption" problem needed.

**Validation plan** (needs device on + real agent + our client connected & focused):
0. **Send `SetDeviceMode("APP ")` (`0x44734d`, value `0x41505020`) FIRST.** Without
   this the draw is gated shut. (Revert with `"MIDI"` or a device replug.)
1. Send `0x647344`, `displayNumber=0`, rect `{x:0,y:0,w:480,h:272}`, payload =
   `480*272*2` bytes of solid RGB565 (e.g. all `0xF800` = red). Watch panel 0.
2. Reply tells us a lot: `'true'`/empty = accepted (look at the screen); `0x00000000`
   = rejected (size/format wrong → iterate).
3. Iterate the axis that's wrong: try 2bpp RGB565 both byte orders, then 1bpp and
   4bpp; try half/quarter dimensions if size is rejected; try a diagonal test pattern
   so orientation/stride is obvious.
4. Once a full-screen fill works, test a partial rect to confirm the dirty-rect
   semantics and `adjustUpdateRectForFormat` alignment.

### Path B — semantic UI model: `0x534d6170` "SMap" (SetMap) 🟡 structure mapped, wire format undecoded
This is how Maschine 2 populates the performance screens (pad names, group names,
parameter values, colours). Client sends **"SMap"**; `ControllerBase::onClientRequest`
(the SMap discriminator is checked at message offset `0x160`) routes it into the Map
system → `MaschineStudio::MapHandler::updateController(IMapControllerSupport::tUpdateData)`.
A background `MaschineStudio::MapUpdateThread` renders the Map to the panels via
`MaschineStudio::DisplayDrawer` — rich widget vocabulary: `renderText`, `renderTextLine`,
`renderTitle`, `renderButton`, `renderPad`, `renderList`, `renderValueArea`,
`renderRoundedBox`, `renderBackground`, `renderPageInfo`.

**Structure decoded from the binary:**
- **The live update is BINARY, not XML.** pugixml in the agent only exposes
  `load_file` (from disk) — the XML `ControllerMap`/`AssignmentMap`
  (`ControllerMap::readMap(pugi::xml_node)`, vocabulary `KNOB`/`PADS`/`PAGE`/`TEMPLATE`/
  `MappingScheme`) is the **on-disk MIDI-mode template system**, a *separate* subsystem
  from the host display protocol. Don't conflate them.
- `updateController` reads a `tUpdateData` struct (fields at offsets `0x110`, `0x120/1`,
  `0x250`, `0x262` on the map object) and emits binary record sub-tags — seen inline:
  `0x4d70546d` **"MpTm"** and `0x4d70576d` **"MpWm"** (Map-Template / Map-Widget-ish).
  These 4CC record types are the vocabulary of the binary Map stream.
- Display content can include **PNG images** (`Display::DataDefs::loadPNG`) and list
  models (`Display::DataDefs::List`, the arg to `renderList`). So an "SMap" can carry
  text, values, widget layout, and embedded PNGs.
- The agent also renders **its own** screens with no client input: `drawSettings`,
  `drawMidiMonitor`, `drawCalibration`, `drawTemplates`, `drawMapping` (SHIFT-menu
  pages). Proof the render pipeline runs independent of "SMap".

**Realistic decode strategy**: static RE of the full `tUpdateData` binary layout is a
large, multi-session effort. The efficient route is **capture real "SMap" frames from
Maschine 2 and decode them using the structural key above** (record tags "MpTm"/"MpWm",
the DisplayDrawer widget set, PNG/List defs). That means finishing the eavesdrop
(server route) — but now we know the exact tag to filter (`0x534d6170`) and how to
interpret the bytes, so a single good capture goes a long way. Start goal: get *one*
`renderText` label onto a panel.

**Why it matters**: native-looking, font-rendered UI, the way the device is meant to be
driven. **Why it's second**: far more complex than a bitmap and needs captured frames.

## ⚠️ [SUPERSEDED] Live test, round 2 (device ON) — the precondition is *display attachment*
*(Wrong conclusion — `this+0x150` is the focus-client id, not a display collection. Kept
for the trail; see the SOLVED banner.)*
With the Studio powered on and in APP mode (`GetDeviceMode` "Dg M" `0x44674d` returns
`"APP "`), draws STILL return `0x00000000` and the panel stays on its "Start MASCHINE
Software" idle screen. Deeper trace of the draw branch in `ControllerBase::onClientRequest`:
it loads `this+0x150` (a collection, zero-inited in the ctor) and **skips the draw when
that collection is empty** (`begin == end`). So the gate is: **the device's displays
must be attached to the controller session serving our client** — which happens for the
adopted host, not a bare IPC client. `SetDeviceMode` and pixel format are both red
herrings (proven live). The lever to find: what populates `this+0x150` — trace
`IPCServer::connectControllerClient`, `MaschineStudio::postConnect`,
`ControllerBase::onFocusClientChanged`, and compare our connect (role `prmy`, boh
`NiM2`) against whatever Maschine 2 does to become the display-owning client.

## ⚠️ [SUPERSEDED] Unifying conclusion (after live testing, 2026-07)
*(This framed the problem as "device adoption / `this+0x39`". The actual answer was
display **focus** via `0x434300`+'strt' — see the SOLVED banner and `longform_research.md`
Finding 2. Kept for the trail.)*
Live test result: `SetDeviceMode("APP ")` from a bare client is accepted (empty reply)
but the draw still returns `0x00000000` — the `this+0x39` gate stays 0. That flag is
set in `ControllerBase::connect(USBDeviceOSHandle&)` (agent↔physical-device open), so
a plain IPC client gets a controller/proxy view with the device **not** connected to
it. **All three goals — raw draw (A), semantic Map (B), and input events (2) — share
this one gate: our client must be *adopted* as the host controller so the agent opens
the device to us in APP mode.** No display path bypasses it. This is the same
"adoption" wall the server-side eavesdrop hit. So the single highest-value target for
the whole project is: **make the agent connect the device to our client** (set
`this+0x39`) — i.e., replicate whatever Maschine 2 does that we don't. Candidate
differences to probe: exact `SetDeviceMode` wire layout (mode 4CC offset unconfirmed),
client role/`ClientProperties`, focus ordering, and any post-connect message Maschine 2
sends that triggers the device open.

## Recommended sequence
1. **Path A first** — crack `DataFormat` + dimensions with live draws (fast feedback,
   small unknown space, full pixel control). Extend `NIDisplayDrawMessage` /
   `NIImageConversions` with a Studio RGB565 encoder + a `NIST7529…`-style decoder for
   captures.
2. **Path B later** — resume the eavesdrop specifically to capture "SMap" frames (now
   we know the exact tag to filter for), and decode `tUpdateData` incrementally
   (start: a single text label on one panel).

## Open questions to close
- [x] Does the raw `0x647344` path need the `0x03` (v3) prefix? **Yes** — the whole
      conversation must use `0x03`; `0x02` connects are refused (`'dice'`).
- [x] Exact `DataFormat` value — Maschine 2 uses **`0x60`** (RGB565 `0x20` + compression
      `0x40`); our client drives the panels with uncompressed **`0x20`** (full `w*h*2`).
- [x] Panel dimensions & full-panel vs dirty-rect — **480×272**, driven via a Bulk
      WriteWindowRequest rect (full-panel confirmed working; dirty-rect available).
- [ ] Confirm the exact RGB565 channel order / byte order on a photographed fill (we
      have "accepted + pushed to USB"; a colour-accuracy check is the last visual detail).
- [ ] The compressed `0x60` scheme (`Display::Bulk` compressor) — only needed to match
      M2's bandwidth; uncompressed `0x20` already works.
- [ ] `tUpdateData` / "SMap" wire layout (Path B — still un-attempted).

## Full client→controller message tag map (from `ControllerBase::onClientRequest`)
IDs are compared as `messageId & 0xffffff` (3-byte tag) except the 4-char-code tags
(`SMap`, `APP `, `MIDI`, `@Cln`, `MR·`, `MS·`, `MY·`) which use all four bytes.
Property tags follow `[prop][g|s][D|I|V]` = get/set. Known/relevant:

| tag        | meaning (⇒ our use) |
|------------|---------------------|
| `0x647344` | DisplayDraw raw bitmap — **Path A** |
| `0x534d6170` "SMap" | SetMap semantic UI — **Path B** |
| `0x6c7500` / `0x6c5500` | Set/Get LED state — **goal 1** |
| `0x446742/43/44` | Get display Backlight / Available / DriverVersion |
| `0x44674d`,`0x44734d` | get/set device ? (`Dg M`/`Ds M`) |
| `0x446772/73`, `0x447372/73` | get/set device property (r*/s*) |
| `0x496774`,`0x497374` | get/set ? property (…I) |
| `0x566766/74/75`, `0x567366/75` | get/set float/value property (…V) |
| `0x434300`,`0x432d00` | connect / disconnect (`CC`/`C-`) |
| `0x4d494449` "MIDI", `0x4d5300`,`0x4d5900`,`0x4d52ff` | MIDI transport |
| `0x41505020` "APP ", `0x40436c6e` "@Cln" | app / client identity |
