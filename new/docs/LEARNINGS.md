# NewClients — experiments & learnings

Running log of what the `StudioDemo` / `HIAClient` experiments taught us.
Newest at the bottom. Protocol context lives in
[../new/docs/studio-investigation.md](../new/docs/studio-investigation.md).

---

## E1 — `StudioDemo` vs the **real** `NIHardwareAgent` (HIA also running)
`MACCHINA_ESTABLISH_REPLY=echo`, product `0x1300`, serial `39195855`.

**Ran through cleanly up to the controller connect:**
- `GetServiceVersion 0x02536756` → **4 bytes `0x00020802`** (our raw-port read got 4, not
  the 8-byte `{0x00020802,3}` the oracle saw — so the client stayed on the `0x02` prefix).
- `Du subscribe 0x02447500` (product `0x1300`) → **port pair** `NIHWS13000043Request` /
  `NIHWS13000043Notification`. ✅
- `SetAsciiString 0x02404300` (register our receive port) → `'true'`. ✅
- `SubscribeVoidOp 0x02447143` sent.

**First inbound frame on our receive port — DECODED:**
```
02 44 4e 2b | 10 e1 09 8d | 00 13 00 00 | 09 00 00 00 |
33 00 39 00 31 00 39 00 35 00 38 00 35 00 35 00 | 00 00
```
- msgid `0x02444e2b` → tag **`0x444e2b` "DN+" = device-added notification** (matches
  `USBDeviceClass::onDeviceAddedEx` in the disasm).
- `+0x04` a handle/ptr; `+0x08` product `0x1300`; `+0x0c` len `9`; `+0x10` **serial as
  UTF-16LE** `"39195855"` + NUL.
- **Learning:** this is NOT `MsgConnectionEstablished` — it's the agent announcing the
  device to a fresh subscriber. The completion handshake belongs to the *controller*
  flow, which we never reached. Also confirms the device is keyed by a **UTF-16 serial**
  (matches `MsgDeviceConnect::setIDString` being UTF-16 in `establish`).

**Controller connect refused:**
- `DeviceConnect 0x02444900` (`[product 0x1300]['NiM2']['prmy'][len 9]["39195855"]`, 29 B)
  → **4 bytes `64 69 63 65` = `'dice'`** (`0x65636964`).
- The *same* message returned a real port-pair earlier when aimed at our **re-signed
  AgentCopy** (HIA not the controller there). Difference now: real `NIHardwareAgent` +
  `NIHostIntegrationAgent` both live.

**Disasm explains `'dice'`/`'ectl'`:** in `IPCServer::handleRequest`, both 4CCs are written
as the message *result* immediately before `IPCServer::forwardRequestToPort(MsgDeviceConnect,
port)`. So `NIHardwareAgent` **forwards** the connect into the daemon chain, and
`'dice'`/`'ectl'` are the **forward-refused / device-busy** statuses left standing when the
forward fails (cf. strings `"Device Busy"`, `"Server rejected connection request"`,
`"Already open"`). The `[x8+0x290]` count checked (`cmp #1`/`cmp #2`) looks like an
existing-connection count — i.e. **the controller is exclusive and already held.**

**Net learning:** our direct-to-`NIHardwareAgent` controller path is blocked not by our
bytes but by **exclusivity** — `NIHostIntegrationAgent` already holds the controller in the
forwarding chain. Two implications:
1. Raw-bitmap path likely needs HIA out of the way (test: stop HIA, retry — see E2).
2. The "designed" path is to go **through HIA** (its app service), which is what Maschine 2
   does; but HIA speaks a different opening dialect (see §9 / `HIAClient`: version
   reply `0x00010f00`, connects refused with `'ectl'`).

**Tooling learnings:**
- `NewClients` in one run beat manual lldb probing for this: it drives the full flow and
  hexdumps every frame both directions, so the undecoded frames (DN+, the refusal codes)
  fell out immediately. Keep the raw-frame + reply-strategy design.
- Our `GetServiceVersion` read returned 4 bytes here vs 8 via the oracle — worth checking
  `NIRawPort`'s reply-length handling / whether the reply truly is 4 vs 8 bytes.

---

## E3–E5 — the draw finally lands (protocol level)
Chain of fixes, each exposed by the previous run:

- **E3 — version prefix bug.** `NIRawPort` read a **4-byte** `GetServiceVersion` reply
  (`0x00020802`) where the oracle saw 8, so the client stayed on the `0x02` prefix and its
  `DeviceConnect` went out as `0x02444900` → `'dice'`. Fix: treat a version `>= 0x00020000`
  as protocol-v3 and switch to the `0x03` prefix. With `0x03444900` the **DeviceConnect
  succeeds** → controller port `NIHWMaschineStudioController-<serial>NNNN`, and
  `GetSerialNumber` on it returns `39195855`. So `'dice'`/`'ectl'` = **wrong-protocol / not
  a valid connect for this port**, NOT device-busy. Exclusivity was a wrong hypothesis.
- **E4 — register the controller's receive port.** We registered the *subscription*'s
  notification port but not the *controller*'s, so the agent never connected back. Adding
  `registerNotificationPort` + `sendSubscribeVoidOp` on the controller made the agent send
  a first inbound frame on the controller receive port: **`0x03444e00`** (deviceStateChange,
  8 B) — the "completion" frame. But sweeping `MACCHINA_ESTABLISH_REPLY` (echo/true/empty/
  none) changed nothing: draws on the controller port always reject.
- **E5 — draw on the SUBSCRIPTION port, not the controller port.** This was the missing
  routing. With subscription + DeviceConnect both established, a `0x03647344` draw on the
  **subscription request port** returns **`'true'`/empty = ACCEPTED** (both displays),
  while the identical draw on the controller port still returns `0x00000000`. Matches the
  debugger finding that Maschine 2 draws on its `NIHWS…` subscription port (its ClientEntry
  is linked to a controller → `onSubscriberMessage` forwards to `onDisplayMessage`).

**Working recipe (protocol level):** v3 handshake → `Du subscribe 0x1300` (+register
+subscribe) → `DeviceConnect 0x1300 serial` (+register +subscribe) → **draw `0x03647344`
on the subscription request port.**

**Still to verify:** whether pixels actually render correctly (RGB565 2bpp @ 480×272) —
"accepted" only proves routing. `NIHostIntegrationAgent` runs its own renderer and may
overwrite our fill immediately, so a clean visual test may need HIA stopped or repeated draws.

---

## E6 — CORRECTION: E5's "accepted" was a FALSE POSITIVE
The user confirmed the panel never changed (still the firmware "Start MASCHINE
Software" idle prompt). Verified in the debugger (re-signed copy, HIA stopped):
- The subscription-port draw `0x03647344` reaches **none** of `onSubscriberMessage`,
  `ControllerBase::onClientRequest`, `MaschineStudio::onDisplayMessage`, or
  `LCDDisplay::drawDisplayEx` — but register (`0x404300`) and subscribe (`0x447143`)
  on the same port *do* hit `onSubscriberMessage`. So the draw is filtered/dropped
  before any handler and returns an **empty** reply.
- The client's success test (`reply.length == 0 || value == 'true'`) wrongly counts an
  **empty/nil reply as accepted**. Fix the probe: only `'true'` (or pixels on screen)
  counts; empty = unhandled/dropped.

**Real status:** the full connection recipe (v3 + Du subscribe + DeviceConnect, both
registered) completes, GetSerialNumber works — but our draws still never reach the
display path. Only `NIHostIntegrationAgent`'s connection does. The ClientEntry-linkage
that routes a draw to `onDisplayMessage` is still not reproduced; matching the message
*sequence* is not sufficient. Next: in the debugger, dump HIA's ClientEntry at a real
`onDisplayMessage` hit vs ours, and find the field/step that differs (the +0x50 controller
link) — and/or why `0x647344` on our subscription port is dropped pre-handler.

---

## E7 — WHY the link never forms: the Studio is a **forward** device in HWAgent (static analysis)
Answered E6's open question by reading the arm64 disasm of `NIHardwareAgent`
(`arm64.txt`) instead of probing bytes. The routing is decided by a **static per-device
flag**, not by our message sequence — so no bytes we send could ever have worked.

**The connect decision — `IPCServer::handleRequest`, DeviceConnect branch (`0x100075d80`):**
- calls `USBDeviceManager::getDeviceEnabled(productId)` (`0x100071208`) via deviceMgr
  vtable+0x10. It returns `deviceClass[+0x141] != 0` — an **"enabled/locally-owned" byte**
  in HWAgent's per-device record.
- flag **set** → falls through to `connectControllerClient` — HWAgent owns the device and
  builds a controller `ClientEntry` locally.
- flag **clear** → calls `getForwardPortName(productId, &name)` (deviceMgr vtable+0x20,
  `0x10007126c`; name string lives at `deviceClass+0x148`) then
  `forwardRequestToPort(msg, name)` — hands the whole connect to another daemon.

**For the Studio (0x1300) we always take the forward path** — that *is* where the
`'dice'`/`'ectl'` result codes are written (E1). So HWAgent is configured to **forward
0x1300 to `NIHostIntegrationAgent`, not own it.**

**Why that kills the subscriber draw — `IPCServer::onSubscriberMessage` (`0x10007aa..`):**
it dispatches on `msg[+0xc] & 0xffffff`, and for the control tags it loads the **linked
controller from `ClientEntry+0x40`** and calls a vtable method on it. The connect-time
deque search in `handleRequest` (`0x10007600c`) likewise matches an entry by
`type(+0x0)==0 && +0x50==<controller>`. Both `+0x40`/`+0x50` are populated **only by
`connectControllerClient`** — which never runs for a forward device. So our subscription
`ClientEntry` has a **null controller link**, and `onSubscriberMessage` has nothing to
forward our `0x647344` draw to → dropped before any handler. Exactly E6's symptom.

**Conclusion — approach A (drive the panel by talking to `NIHardwareAgent` directly) is a
dead end for the Studio.** The controller+display logic lives behind HWAgent's forward, in
`NIHostIntegrationAgent`. HWAgent is a USB shim + proxy for 0x1300; it will never create a
linked controller entry for an external client, regardless of the bytes.

**Cheap empirical confirmation (optional, needs device on):** re-signed HWAgent copy,
`b USBDeviceManager::getDeviceEnabled`, run our `StudioDemo`, check `w0` on return
for productId 0x1300 — expect **0** (forward). And `b …connectControllerClient` should
**never** hit for 0x1300 while `…forwardRequestToPort` does.

**Reorients the whole effort to approach B — talk to `NIHostIntegrationAgent`** the way
Maschine 2 does. That is the daemon that actually owns the Studio controller and renders
the displays. Our `HIAClient` reached HIA but its connects are refused `'ectl'`
(§9): HIA speaks a different opening dialect (version reply `0x00010f00`) we haven't
decoded yet. **That handshake is now the single thing standing between us and pixels.**

---

## E8 — the forward-vs-own decision is **on-disk `.ndd` config** — and we can flip it
Traced `getDeviceEnabled` back to its writer:
`USBDeviceManager::SupportedDeviceClass::checkEnabledStateAndForwarding()`
(`arm64.txt 0x100070538`). It is the single authority for the `+0x141` enabled byte:

1. builds path `<system_data_ni_dir>/Hardware/Devices/<deviceClassName>.ndd`
   (macOS: `/Library/Application Support/Native Instruments/Hardware/Devices/…`);
2. `boost::filesystem::status(path)` → **enabled(+0x141) = fileExists OR preset(+0x140)**;
3. if enabled AND flag(+0x142) AND regular file: parse it as XML — root
   `ni-hardware-controller` with attr `version >= 2`, child `<forward-port-name>` → copy
   into `deviceClass+0x148`, **and then clear enabled(+0x141) back to 0** (`0x100070904`).

So the `.ndd` file decides ownership: **file with a `<forward-port-name>` ⇒ forwarded to
that port; file with none ⇒ owned locally; no file ⇒ compiled-in default.**

**Confirmed by the one file that exists on this machine** — `Komplete Kontrol.ndd`:
```xml
<ni-hardware-controller version="2">
  <forward-port-name>com.native-instruments.NIHostIntegrationAgent</forward-port-name>
</ni-hardware-controller>
```
i.e. KK is explicitly configured to forward to HIA. **There is NO `Maschine Studio.ndd`**,
so the Studio's forwarding is the compiled-in default in its registration
(`SupportedDeviceClassImpl<MaschineStudio>`), which is why it forwards to HIA with no file
present.

**Key capability check:** `NIHardwareAgent` itself contains the *entire* local Studio stack
— `SERVER::MaschineStudio` (+`C2`, `registerInternalClientsFor`, `SleepModeClient`),
`MaschineStudio::DisplayDrawer`, `MaschineStudio::MapHandler`, `ControllerBase::
onDisplayMessage`, `getLCDDisplayInterface`. So HWAgent can own the Studio and drive its
displays with **no HIA involved** — the forward is pure configuration, not a capability gap.

**The experiment this unlocks (approach A, revived — the clean path to pixels):**
drop in `/Library/Application Support/Native Instruments/Hardware/Devices/Maschine Studio.ndd`
containing a valid `ni-hardware-controller version="2"` with **no `<forward-port-name>`**,
restart `NIHardwareAgent`. Then `getDeviceEnabled(0x1300)` returns 1 →
`handleRequest` takes the local `connectControllerClient` path → our `DeviceConnect` builds
a linked controller `ClientEntry` → our subscriber `0x647344` draw reaches
`MaschineStudio::onDisplayMessage` → `DisplayDrawer`/`LCDDisplay::drawDisplayEx` → USB.
Fully reversible (delete the file, restart the daemon).

Caveats: (1) writing under `/Library/Application Support` needs `sudo` — the USER runs it,
not us. (2) exact filename should be confirmed empirically (e.g. `sudo fs_usage -f
filesys NIHardwareAgent | grep '\.ndd'` at startup) — it derives from the device-class
name string, "Maschine Studio". (3) while the file is present NI's own stack will find the
Studio locally-owned too; fine for our isolated test (we don't run Maschine 2).

---

## E9 — the `.ndd` flip WORKS (debugger-proven), but local ownership ≠ a routed draw
Confirmed the device-class name statically: `SupportedDeviceClassImpl<MaschineStudio>::C2(
"Maschine Studio", …)` → file is `Maschine Studio.ndd`. Dropped it in (no
`<forward-port-name>`), restarted `NIHardwareAgent`, and ran `StudioDemo` against a
re-signed debuggable copy. Regex breakpoints (note: `break set -r` matches **demangled**
names; mangled fragments stay *pending*), auto-continue `-G 1`, command `-C "frame info"`.

**Agent-side dispatch order during our connect (clean capture, `lldb6.log`/`hwtest6.log`):**
`getDeviceEnabled → onSubscriberMessage ×2 (register+subscribe) → getDeviceEnabled →
connectControllerClient`. **`forwardRequestToPort` never fires.** So the config flip did
exactly what E8 predicted: **HWAgent now owns the Studio locally instead of forwarding to
HIA.** This holds with **HIA killed** too — HIA was never the blocker (tested: same result).

**But the draw still does not render** (client: subscription draw → empty/DROPPED,
controller draw → `0x00000000`). The `0x647344` draw on the subscription port **never
reaches `onSubscriberMessage`** (it fired only for the setup register/subscribe, before the
draw). So the draw is dropped at the request-router *before* the subscriber handler,
because the **subscription↔controller link is still not formed**:
`connectSubscriptionClient` only zero-inits the link field (`str q0, [x22, #0x40]`); nothing
we send sets it. Local ownership was necessary but not sufficient.

**Corrections to earlier notes:** (a) the Studio's display handler is
`MaschineStudio::onDisplayMessage` (`…edd44`), an override — NOT `ControllerBase::
onDisplayMessage` (I briefly watched the wrong symbol; client-side proof unaffected).
(b) HIA's DeviceConnect refusal is *also* a forward (`getForwardPortName`), not a hardcoded
`'ectl'` (E7).

**Open nut (the whole game now):** what sets the subscription ClientEntry's `+0x40`
controller link. Candidates, untested: (1) a link/attach message NI's real client sends
after connect that we don't replay; (2) the link forms only via the establish-handshake
completion and our `MACCHINA_ESTABLISH_REPLY` answer is wrong; (3) NI uses **one connection**
that both subscribes and connects, and the agent links by connection identity — our client
splits them across two ports, so they can't be matched. Next probe: debugger on
`connectControllerClient` (does it search for & link a subscription?) + capture NI's real
Studio controller-connect sequence to diff against ours.

**Env note:** after this session `.ndd` is present, HIA is down, real HWAgent up. To return
to normal NI use: delete `Maschine Studio.ndd`, relaunch `NIHostIntegrationAgent`.

---

## E11 — disassembled **Maschine 2** itself: how the real client connects + draws
Maschine 2's arm64 binary is **fully symboled** (325k syms) and links the NHL2 *client*
code. Chased the call chain (lldb `disassemble` on the static target, `m2_*.txt`):

`MaschineController<MaschineStudio>::connectToDevice` (`0x10011c948`, passes clientTag
`'NiM2'` + role `'prmy'` — same 4CCs we send) → **`NHL2::Controller::connect(idstring,
productId, ClientRole, callback, connection)`** (`0x1022736bc`) → builds one
`MsgDeviceConnect`, `new IPCConnection(portName)`, two `std::function` callbacks (a
`bool(u64,Message&)` handler + `void(u64)` drop) → **`IPCConnection::establish`**
(`0x10226eab4`, virtual).

**`establish` (the whole handshake, one connection):**
1. `sendMessageToConnectionPort` (init) → `startService` → `sendMessageToConnectionPort`.
2. idstring re-encoded **UTF-16** (`boost::locale::utf`), `MsgDeviceConnect::setIDString(
   char16)` → `sendMessageToConnectionPort` (the connect). Reply → `getInPortName` /
   `getOutPortName`.
3. `make_shared<MessageSendPort>(inPortName)` + `IPCPort::OSImpl::open` — the **send** port.
4. `MessageReceivePort(outPortName)` + `OSImpl::create` + **`receiveEvents(handler)`** — the
   **receive** port.
5. **`MessageSetString(0x404300, outPortName)`** → `IPCSendPort::sendMessage` — registers
   the receive port on the send port.

**No `MsgDeviceSubscribe` anywhere.** The controller is a **single `MsgDeviceConnect`** +
one send/receive port pair. Draws (`LCDDisplay::drawDisplayEx` → vtable send on
`LCDDisplayImplClient<MaschineStudio>`) ride **that controller connection's send port** —
**not** a subscription. So our E5 model (Du-subscribe + draw-on-subscription-port) was
wrong from the start.

**The register message format — decoded from `MessageSetString::MessageSetString`
(`0x102278b54`) + the `sendMessage` in establish (wire = `msgobj+0xc`, len `strlen+0x10`):**
```
[msgid 0x03404300][8 bytes = ZERO][strlen u32][portName bytes + NUL]
```
The ctor writes msgid (`w1 | 0x03000000`), strlen at +0x18, string at +0x1c, and **leaves
+0x10..0x17 zero**. **Our `registerNotificationPort` puts `0xabababab 0xcdcdcdcd` in exactly
those 8 bytes** (self-flagged "v2 sentinel, v3 undecoded"). That field is what associates
the receive port with this controller connection — non-zero garbage is the leading suspect
for the missing `+0x40` link.

**Concrete fix plan for `StudioDemo` (make it a faithful `establish` replica):**
1. Drop the separate `Du subscribe`; do a **single `DeviceConnect`**.
2. Register with a **clean `0x404300`**: msgid, **8 zero bytes**, strlen, name (kill the
   sentinels).
3. **Draw on the controller connection's send/request port** (the `DeviceConnect` reply's
   in-port), not a subscription port.
4. Likely also send the idstring as **UTF-16** (establish does).
Then re-test draws → expect `MaschineStudio::onDisplayMessage` to finally fire.

---

## E12 — the real reason: the Studio speaks the **Bulk display protocol**, not raw RGB565
Fixing the register sentinels to zero (E11 plan #2) changed nothing — draws still
dropped/rejected. So the register field wasn't the link key. Kept chasing Maschine 2's
disasm down the **draw** path and found the actual wall: **we've been sending the wrong
display protocol the whole time.**

Chain: `MaschineStudio::Controller::drawDisplayEx` (`0x10227db4c`) fetches a
**`Display::DataFormat`** (vtable+0x170) + `Display::adjustUpdateRectForFormat`, then →
**`NHL2::Controller::drawBulkDisplay`** (`0x102273054`). `drawBulkDisplay` builds a
`0x647344` `MsgDisplayDraw` envelope whose **payload is a `Display::Bulk` command stream**,
2 bytes/pixel (`lsl w,#1`):

**Draw message wire format (Studio):**
```
0x647344 envelope (MsgDisplayDraw::create, 0x102278e1c), header from obj+0xc:
  +0x00  msgid    0x03647344
  +0x04  display  (raw index, e.g. 0/1 — NOT display|0x10000000; our OR was wrong)
  +0x08  (originX<<16)|originY          (from rect, BE-in-halfwords via bitfields)
  +0x0c  (width<<16)|height
  +0x10  dataSize
  +0x14  ── PAYLOAD = Bulk command stream ──
         WriteWindowRequest  (16 B, 0x10226bac4):
           +0  0x0084 (LE16 opcode)
           +2  id      = display index (byte)
           +3  DataFormat (byte)
           +4  uint32 BE = 0
           +8  left  (BE16)   +0xa top (BE16)   +0xc width (BE16)   +0xe height (BE16)
         pixel data          (width*height*2 bytes, DataFormat via Display::Bulk::renderPixelData)
         EndOfUpdateRequest  (4 B, 0x10226bb24): 0x0040 (LE16) | id (byte) | 0x00
```
`drawBulkDisplay` args: `(uint display, DataFormat, Picture&, Rect&, Picture* prev)`;
size passed to `create` = `width*height*2 + 0x50` (slack), then `setPictureDataSize` sets
the exact used size. All multi-byte geometry inside the Bulk requests is **big-endian**.

**So our `SolidRGB565DrawFrame` was wrong on two counts:** (1) `display | 0x10000000`
should be the raw index; (2) the payload must be a **Bulk stream** (`WriteWindowRequest` +
pixels + `EndOfUpdateRequest`), not a bare RGB565 array. Sending a raw fill is why
`ControllerBase::onClientRequest` rejected it (`0x00000000`) — the server's display parser
never saw a valid `WriteWindowRequest`.

**Still to pin down (1 byte + encoding):** the exact `DataFormat` enum value for the Studio
(the +0x3 byte; `renderPixelData` @ `0x10226bb38` bit-tests it: `and w,#0x38; cmp #0x18`,
`cmp w7,#0x8` — a channel-layout code, 2 B/px). Get it from the Studio's vtable+0x170
format-getter, or brute a couple of candidates. Then `renderPixelData`'s pixel encoding
(likely RGB565, possibly byte-swapped) is the last detail.

**Next: rewrite the draw to emit the Bulk stream** (raw display index + WriteWindowRequest
+ pixels + EndOfUpdateRequest, BE geometry) on the controller port, and test.

---

## E13 — pivot to HIA: captured M2↔HIA, cracked the dialect, subscribe works, draw still drops
HWAgent proxies the Studio (`ControllerBase::onClientRequest` rejects draws regardless of
payload), so pivoted to **NIHostIntegrationAgent** — the daemon M2 actually drives.
Confirmed the whole path works: ran a re-signed **HIA copy** in place of the real one,
launched Maschine 2, and the **Studio showed M2's UI through our copy** (so the copy is a
valid drop-in; M2 → HIA-copy → HWAgent → USB is live). *Gotcha:* restarting `NIHardwareAgent`
while M2 is connected **severs** the live device connection (screens fall back to the idle
prompt) — don't.

**Captured M2's connect to HIA** (lldb on the copy, breakpoint `IPCServer::handleRequest`,
dump `msg+0xc`):
- HIA answers `GetServiceVersion` = **`0x00010f00`**, yet M2 uses the **`0x03` prefix**
  (`0x03536756`, `0x03447500`). Our client derived `0x02` from that version → every connect
  refused `'ectl'`. **Fix: force `0x03` for both daemons.** (Done in `NIAgentConnection`.)
- M2 does **only `Du subscribe`** to HIA — **no `DeviceConnect`**, `connectSubscriptionClient`
  fires (×7), `connectControllerClient` never does. It walks a **different product-id
  space** than HWAgent's USB PID `0x1300`: `0x1350, 0x1600, 0x1610, 0x1700, 0x1730, 0x1820,
  0x1860` (per product: `GetServiceVersion(pid)` then `Du subscribe(pid)`). The subscribe
  body carries clientTag `'NiM2'`, role `'prmy'`, and sometimes a request-port name.

**With `0x03` + a valid HIA product id, we get past `'ectl'`:** `Du subscribe` to any of the
7 returns a real port pair (`NIHWS<pid><n>Request`/`Notification`); `SetAsciiString`
register returns `'true'`. DeviceConnect now returns `'dice'` (wrong-protocol) not `'ectl'`.

**But the bulk draw on the subscription port still returns empty (dropped)** — the *same*
linkage gap as HWAgent (E9): a subscription accepts register/subscribe but drops `0x647344`
until it's linked to a controller. Our capture only watched `handleRequest` (the main
connection port), so it **missed whatever M2 does on the per-subscription port** (→
`onClientRequest`/`onSubscriberMessage`) that forms the link and carries the draw.

**The one remaining unknown, and the definitive next step:** re-capture M2↔HIA with
breakpoints on **`IPCServer::onClientRequest` / `onSubscriberMessage` / `MaschineStudio`
display handlers** (not just `handleRequest`), to see the post-subscribe step that links the
subscription + the exact draw M2 sends on it. Also still open: which of the 7 pids is the
Studio (needs the device forwarded into HIA — a device-added `DN+` on that pid's
subscription; our short-lived probes saw none, forward-state uncertain after restarts).

Client changes landed this session: `NIAgentConnection` forces `0x03`; `StudioDemo`
and `HIAClient` emit the Bulk draw (`WriteWindowRequest`+pixels+`EndOfUpdate`,
format `0x60`). Env left: real HIA replaced by re-signed `HIACopy.app` under lldb; restore
with `pkill`-copy + `open` the real `NIHostIntegrationAgent.app`.

---

## E14 — captured M2's ACTUAL draw in HWAgent: pixels are **compressed** (that's the bug)
Ran **both** daemons as re-signed copies, powered the Studio, launched Maschine 2 against
them, lldb on the HWAgent copy. Draws land in **HWAgent** (not HIA): `MaschineStudio::
onDisplayMessage` → `LCDDisplay::drawDisplayEx` → USB. Two phases observed:
- **Phase 1 (discovery, via `onSubscriberMessage`):** per product — `GetServiceVersion` +
  `Du subscribe 0x447500` + register `0x404300` + `subscribeVoidOp 0x447143`.
- **Phase 2 (control+draw, via `onClientRequest`):** `GetServiceVersion` → **`DeviceConnect
  0x444900`** → register `0x404300` → a Get-handshake (`0x566775`, `0x636749`,
  `0x436753`=GetSerial, `0x63674e`) → then **`0x647344` draws**. So **draws ride the
  controller connection**, dispatched by `ControllerBase::onClientRequest` → the Studio's
  `onDisplayMessage`. (Not the subscription — E13's subscription-draw attempts were wrong.)

**M2's draw bytes at `onDisplayMessage` (display 0):**
```
+00 44 73 64 03   msgid 0x03647344
+04 00 00 00 00   display 0
+08 00 00 00 00   originXY 0
+0c 10 01 e0 01   (width<<16)|height = 480,272
+10 b4 09 00 00   dataSize = 0x9b4 = 2484        <-- full-screen window, only 2484 B
+14 84 00 00 60   WriteWindowRequest: op 0x0084, id 0, format 0x60
+18 00000000 0000 0000 01e0 0110   u32BE=0, left0 top0 width480 height272 (BE)
+24 01 00 75 92 … compressed pixel stream
```
**The envelope + WriteWindowRequest match our `SolidBulkDrawFrame` byte-for-byte.** The one
difference: **dataSize 2484 ≪ 480·272·2 = 261 120**, i.e. the pixel data is **compressed**.
Format **`0x60` = RGB565 base `0x20` + compression flag `0x40`** (the exact bit
`Display::renderPixel` masks off with `& ~0x40`). Our client sends format `0x60` but a
**full uncompressed** 261 KB payload → the server decompresses garbage → reject. That is
the whole reason draws fail; nothing else in the frame is wrong.

**Two ways to fix (next):**
1. **Uncompressed:** send format **`0x20`** (base RGB565, compression bit clear) with the
   full `width*height*2` payload. One-byte change if the server honours uncompressed.
2. **Compressed:** implement the `0x40` scheme (decode M2's `renderPixelData` /
   `Display::Bulk` compressor — the 2484-byte stream is the reference).

**Still to nail for the controller connection:** M2's `DeviceConnect` to HWAgent reaches
`onClientRequest` **locally even in normal topology** (ours forwards 0x1300 to HIA / rejects
under the `.ndd`). Need M2's `DeviceConnect` **product id + body** (recapture the connect
with a `handleRequest` hexdump) — likely a different pid than USB `0x1300`, or it works
because Phase 1's subscribe primed it. That's the last piece before our own draw can ride
the same path.

---

## E15 — captured M2's DeviceConnect body: the extra **port-name fields** make it local
Recaptured a fresh M2 connect (quit+relaunch M2, HWAgent copy under lldb, `handleRequest`
hexdump). M2's `DeviceConnect` for product **`0x1300`** routes straight to
**`connectControllerClient` (LOCAL)** — no forward — in normal topology. Body (wire):
```
+00 00 49 44 03            msgid 0x03444900
+04 00 13 00 00            product 0x1300   (same USB pid we use!)
+08 32 4d 69 4e            'NiM2'  (clientTag)
+0c 79 6d 72 70            'prmy'  (role)
+10 09 00 00 00            idString len = 9
+14 39 31 39 35 38 35 35 00  "39195855\0"  (serial, ASCII — same as us)
+1d …"…0055Request\0"      M2's own REQUEST port name (null-terminated)
+29 1a 00 00 00 "NIHWS…0055Notification\0"  (len 26) M2's NOTIFICATION port name
```
**The difference from our `DeviceConnect`:** we stop after the serial; **M2 appends its own
request + notification port names.** With them, HWAgent connects the controller *locally*;
without them ours gets forwarded (`'dice'`/`'ectl'`). So the client must **pre-create its
receive/send ports and name them in the `DeviceConnect`**, the way `IPCConnection::establish`
does (it opens `inPortName`/`outPortName` from the reply — here M2 supplies them up front).

**M2's full working sequence to light the Studio (all on HWAgent):**
1. Phase 1 — per family: `GetServiceVersion(pid)` + `Du subscribe(pid)` + register
   `0x404300` + `subscribeVoidOp 0x447143` (via `onSubscriberMessage`). Discovery.
2. Phase 2 — `GetServiceVersion` → `DeviceConnect 0x1300 …+port names` →
   `connectControllerClient` LOCAL → register `0x404300` → Get-handshake
   `0x566775`/`0x636749`/`0x63674e` (+`GetSerial 0x436753`) → **compressed `0x647344`
   draws** (via `onClientRequest` → `MaschineStudio::onDisplayMessage` → `drawDisplayEx`).

**Remaining to implement in our client (have the full reference now):**
- `DeviceConnect` that pre-creates + names the request/notification ports (→ local connect).
- the Get-handshake trio (verify whether required to arm the display, or just capability
  queries).
- draw payload: either uncompressed **format `0x20`** (full `w*h*2`, simplest — try first)
  or the compressed **`0x60`** scheme M2 uses (2484 B/frame reference).

**CORRECTION (E15 port-name claim was wrong):** `MsgDeviceConnect` has only *getters*
`getInPortName`/`getOutPortName` (no setters); `establish` reads them from the **reply**.
The "port names" seen after the serial in M2's captured `DeviceConnect` were **stale buffer**
from the preceding subscribe message — **not** part of the connect. M2's `DeviceConnect` is
the same 29 bytes as ours (`[pid][NiM2][prmy][len9]["39195855\0"]`). **Our connect already
works locally** (we get a `NIHWMaschineStudioController-…` port and `GetSerial` answers) —
so the connect is NOT the problem.

---

## E16 — our draw reaches the controller but is rejected pre-`onDisplayMessage`
Built the fix (draw format configurable, default uncompressed `0x20`), quit M2 to free the
controller, ran our client against the HWAgent copy under lldb. Result:
- connect fine, `GetSerial` answers;
- our `0x647344` draw **reaches `ControllerBase::onClientRequest`** and is **rejected
  (`0x00000000`)** — it does **NOT** reach `MaschineStudio::onDisplayMessage`.

**Disasm of `ControllerBase::onClientRequest` (`0x100021700`)** is a tag binary-search that
has **no case for `0x647344`** → falls through to reject. So a draw on a *base* controller
is always rejected. But **M2's `0x647344` draws DO reach `MaschineStudio::onDisplayMessage`**
(via `IPCServer::onClientRequest`, confirmed in the capture). Conclusion: **M2's connection
resolves to a display-capable `MaschineStudio` controller; ours resolves to a plain
`ControllerBase`.** Same product/serial connect — so the promotion happens *after* connect.

**Prime suspect: M2's Get-handshake configures/promotes the controller.** Captured bodies
(structured, not empty gets), sent on the controller port before any draw:
- `0x03566775`: `[0x23436c6e][04 00 00 00][zeros…]` …
- `0x03636749`: `[0][0x15][0][0x16][8][0x14][4]…` (a list of section IDs)
- `0x0363674e`: `[0x16][0][0x0d]…`
Our client sends none of these. Likely one of them (or the trio) switches the controller
into the Maschine-Studio display mode so `onClientRequest` routes `0x647344` onward.

**Next experiment:** replay these three messages (exact captured bytes) on the controller
request port after connect + register, then draw — and watch whether `onDisplayMessage`
finally fires. If it does, decode/parametrize them; then tackle the payload (uncompressed
`0x20` vs compressed `0x60`). This is the last gap.

---

## E17 — ✅ SOLVED: it was **display FOCUS**, not the handshake queries or the payload
Full story + disassembly in [longform_research.md](longform_research.md). The 3 config
queries above (`0x566775`/`0x636749`/`0x63674e`) are capability queries — NOT the gate.
The real gate: `MaschineStudio::onDisplayMessage` accepts a `0x647344` draw only from the
**focus client** — `ControllerBase::onClientRequest` guard `ldar x22,[ctrl+0x150]; cmp
x22, sender; b.eq accept; else setResult(false)`. `ctrl+0x150` = the focus client, set by
`onControllerAcquire` → `setFocusClient(sender,true)`, dispatched for tag **`0x434300`**.

**THE MISSING STEP:** send `0x03434300` + body `'strt'` (`0x73747274`) on the controller
request port after connect+register. Then draws are accepted (`'true'`) and pushed to USB
via `BulkDisplayImplementation::onDrawDisplayMessage`. Verified live: onDisplayMessage ==
USB-push count 1:1, both panels. `StudioDemo` now drives BOTH Studio displays
(`MACCHINA_LOOP_DRAW=1`).

**Final working recipe (see longform_research.md Finding 5 for details):**
v3 prefix `0x03` → `Du subscribe 0x1300` (+register w/ zero 8-byte field, +subscribeVoidOp)
→ `DeviceConnect 0x1300` (ASCII serial) → **`0x434300`+'strt' (ACQUIRE FOCUS)** →
`0x647344` draw (WriteWindowRequest fmt 0x20 uncompressed or 0x60 compressed, +pixels,
+EndOfUpdate) on the controller port. Corrections to earlier entries: draws ride the
**controller** connection (not subscription — E5/E13 wrong); the format/compression was
never the blocker (E12/E14 were red herrings — even M2's verbatim frame rejected without
focus); connect already worked locally (E15).

---

## E10 — shared notification-port identity does NOT form the `+0x40` link (refuted)
E9's leading untested hypothesis (#3): the agent links the controller `ClientEntry` to the
subscription one by **shared notification-port identity**, and our client defeats that by
giving each connection its own receive port + registering each under its own name.

Wired a switch to test it without a recompile — `MACCHINA_SHARED_NOTIFY_PORT=1`
(`HardwareAgentClient/main.m` + `NIAgentConnection registerNotificationPortName:toPort:`):
when set, the controller connection registers the **subscription's** notification-port name
(via `SetAsciiString 0x404300`) against the controller request port, so both connections
advertise the *same* identity. Everything else (ports, draw sequence) is unchanged.

```bash
MACCHINA_SHARED_NOTIFY_PORT=1 build/StudioDemo
```

**RESULT — hypothesis #3 REFUTED (live run 2026-07-22, `.ndd` flip active so HWAgent owns
0x1300 locally; controller port came back `NIHWMaschineStudioController-391958550046…`,
confirming local ownership).** With the subscription's notification-port name
(`NIHWS13000045Notification`) registered against the **controller** request port
(`0x03404300` → `'true'`), draws are **still not routed**, byte-for-byte as E9:
- subscription-port `0x03647344` → **empty / DROPPED** (both displays);
- controller-port `0x03647344` → **`0x00000000` rejected** (both displays);
- `GetSerialNumber` on the controller port still answers, so the ports are live.

So the `+0x40` controller link is **not keyed on the registered notification-port name** —
making both connections share one identity changes nothing. That eliminates the "split
across two ports, matched by name" theory.

**Remaining E9 candidates (now the live leads):**
1. an **attach/link message** NI's real client sends after connect that we don't replay —
   needs a capture of NI's real Studio controller-connect to diff against ours;
2. a **single connection** that both subscribes and connects on **one** request+receive
   port (not just a shared *name* — a shared *port*), so the agent links by connection
   object identity. The E10 knob shared the name only; a true single-port design is the
   next code change to try.
3. the link forms inside `connectControllerClient` by searching the subscription deque —
   check in the debugger whether it runs that search at all for a locally-owned Studio.

## E18 — LEDs + input both work over the focused controller connection ✅ (2026-07-22)
With the connection focused (E17 recipe: subscribe → DeviceConnect → acquire `0x434300`+'strt'):

**LEDs OUT — solved.** Message = `[u32 0x036c7500][213 raw level bytes]`, **no count field**.
- Decoded from `MaschineStudio::updateLEDs` (agent arm64 `0x100039580`): a 213-byte
  (`0xd5`, = `LEDsV1(213)` ctor) intensity buffer at controller `+0x2e0`, each byte scaled
  by a brightness factor, pushed as 4 HID reports — `0x80`:62 (idx 0-61), `0x81`:44 (62-105),
  `0x82`:52 (106-157), `0x83`:55 (158-212).
- Live: `MACCHINA_LED=1 MACCHINA_LED_FRAMING=0 MACCHINA_LED_VAL=ff` lit ~all buttons/pads.
  **Framing matters:** `FRAMING=1` (legacy MK1 `[msgid][n=57][bytes]` count field) → frame
  **DROPPED** (empty reply); `FRAMING=0` (bare `[msgid][213]`) → lights on. The reply is
  empty either way (fire-and-forget), so judge by the device, not the reply.
- **LED intensity is 7-bit (0–127).** Bit 7 is not brightness: a value ≥128 wraps, so a
  smooth 0→255 breath snaps to dark at 128 (looked like the loop "restarting" mid-fade).
  all-`0xff` still reads full (low 7 bits = 127). Bit-7 meaning undecoded.
- **Display pixels are big-endian RGB565 on the wire** — sending native little-endian showed
  red as blue and blue as green; NIStudioController byte-swaps each pixel before sending.
- `in`/`out1..3` LEDs stay dark — MIDI I/O status LEDs (Studio has MIDI only, no audio
  interface), outside the 213-channel map. One jog-ring segment also unmapped. Index→LED
  map is the remaining TODO.

**INPUT IN — solved.** Events arrive on the controller **Notification** port (not the
subscription port), one message per gesture, common 16-byte header
`[u32 msgId][u32 seq][u32 tsNanos][u32 count]` + fixed records:
- `0x03504e00` Pads: `{u32 padId(0-15), u32 state(1=hit,4=pressure,3=release), f32 pressure}`.
- `0x03734e00` Switches: `{u32 switchId, f32 state(1.0/0.0)}` — buttons + touch-knob touch
  sensors (PLAY=`0x1d`, a knob-touch=`0x58`).
- `0x03654e00` Knobs (touch-sensitive, no detents): `{u32 id, f32 delta}` fine deltas.
- `0x03774e00` Big wheel (detented): `{u32 id, i32 delta}` ±1 per notch.
`seq` is constant across one gesture (touch-down → deltas → touch-up share it), bumps between.
Full protocol in new/docs/protocol.md (LEDs / Inbound events).

Both proven from the same from-scratch `StudioDemo`. Next: fold display+LED+input into
a clean `NIStudioController` façade (delegate for events), replacing the probe scaffolding.
