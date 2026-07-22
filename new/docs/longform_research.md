# Longform research — cracking the Maschine Studio display path

Autonomous research session (user away, cannot power-cycle or answer questions).
Goal: understand exactly why Maschine 2's `0x647344` display draws reach
`MaschineStudio::onDisplayMessage` on `NIHardwareAgent`, while our client's draws —
same product/serial connect, handshake queries answered, and *even M2's verbatim
captured frame* — are rejected (`0x00000000`) at `ControllerBase::onClientRequest`.

## Hard facts established before this session (see LEARNINGS.md E1–E16)
- Draws go to **HWAgent** on the **controller connection** (DeviceConnect 0x1300 →
  `connectControllerClient` LOCAL). Not HIA, not the subscription.
- Route (captured live): `IPCServer::onClientRequest` → `ControllerBase::onClientRequest`
  → `MaschineStudio::onDisplayMessage` → `LCDDisplay::drawDisplayEx` → USB.
- Our connection reaches `ControllerBase::onClientRequest` with `0x647344`, but it is
  **rejected before onDisplayMessage**. M2's identical-tag draw dispatches through.
- **Replaying M2's exact 2504-byte frame from our connection still rejects** → it is NOT
  the payload/compression. It is **connection state**: some per-connection condition
  gates the draw dispatch, and ours doesn't satisfy it.
- Our 3 replayed handshake queries DO get real answers (`0x566775`→5/6, `0x636749`→22,
  `0x63674e`→11) — so our controller IS a functional MaschineStudio instance; it just
  won't accept our draws.
- Draw wire format (confirmed byte-exact vs M2): `0x647344` envelope + WriteWindowRequest
  (op 0x0084, id=display, format **0x60** = RGB565+compressed) + compressed pixels +
  EndOfUpdateRequest. dataSize ~2484 for a full 480×272 frame.

## Working hypothesis
`ControllerBase::onClientRequest` dispatches `0x647344` to the derived `onDisplayMessage`
only when the connection holds some **display ownership / active-client / mode** state,
set by a handshake step M2 performs that we do not (or do incorrectly). Find that step by
reading both the receiver (HWAgent `onClientRequest` + the handshake-message handlers) and
the sender (Maschine 2's connect/handshake code).

## Investigation log

### Finding 1 — the draw guard is a FOCUS/OWNER check (HWAgent receiver side)
`ControllerBase::onClientRequest(y, msg)` dispatch for a `0x647344` draw ends at the
default handler `0x100021f5c`:
```
ldar x22, [x19, #0x150]      ; controller+0x150 = focus/owner client token (atomic)
cmp  x22, x21                ; x21 = y = the requesting client's id
b.eq accept                  ; owner == sender  -> tail-call vtable+0x1e0 (onDisplayMessage)
...  else MsgDisplayDraw::setResult(false) -> REJECT (0x00000000)
```
So a draw is accepted ONLY from the **focus client**. `controller+0x150` holds the focus
client id; event-forwarding methods (`onSwitchEvents`, …) also read it to route events.
Managing methods: `ControllerBase::setFocusClient(y,bool)`, `setFocusClientByID(u32,bool)`,
`chooseNewFocusClient(...)`, `onFocusClientChanged()`. `chooseNewFocusClient` selects based
on fields at `controller+0x160` (a 4CC-ish value, e.g. cmp `0x536c6570`) — selection
criteria not yet fully decoded.

**Why our draws reject:** our client never becomes the focus client, so `ctrl+0x150 != y`.
This is why even M2's *verbatim* frame rejects from our connection — it's connection focus,
not payload. **Next: capture live how M2 acquires focus (backtrace on setFocusClient).**

### Finding 2 — the missing step is CONTROLLER ACQUIRE (grants focus) — SOLVED the guard
Live backtrace of how M2 gets focus:
```
setFocusClient(y=<sender id>, true)
 ← ControllerBase::onControllerAcquire(y, Message&)       (vtable+0x218)
 ← ControllerBase::onClientRequest → IPCServer::onClientRequest
```
Dispatch (HWAgent `onClientRequest`): `cmp tag,0x434300 ; ldr x3,[vtable+0x218] ; b dispatch`.
So the **acquire message tag = `0x434300`** → `onControllerAcquire` → `setFocusClient(sender,
true)` → sets `controller+0x150 = sender id` → draws from that client are accepted.

**Captured acquire message (verbatim from M2), 8-byte frame:**
```
msgid 0x03434300   body 0x73747274 = 'strt'   (i.e. [00 43 43 03][74 72 74 73])
```
`onControllerAcquire` also checks `controller+0x250 == 'APP '` (0x41505020) — a client-type
gate; need to confirm our connection's +0x250 is set to 'APP ' (likely from the connect
clientTag / an earlier message). **Plan: send `0x03434300`+'strt' on the controller request
port after connect+register, before drawing → we become focus → draws accepted.**
Also decoded: `MaschineStudio::onDisplayMessage` = vtable+0x1e0 (the accept-path target).

### Finding 3 — ACQUIRE SOLVES THE GUARD: draw now ACCEPTED (but USB push not yet firing)
Added `SendControllerAcquire` (send `0x03434300`+'strt' on controller port after
connect+register+handshake, before draw). Ran our client with acquire + M2's verbatim
2504-byte frame against the HWAgent copy. Server side:
- `setFocusClient` FIRES → we are the focus client (ctrl+0x150 = our id). ✅
- draw reply flips `0x00000000` → **`0x74727565` = 'true' = ACCEPTED**. ✅
- **`MaschineStudio::onDisplayMessage` FIRES** — the draw reaches the display handler. ✅

Remaining: `LCDDisplay::drawDisplayEx` (the actual USB pixel push) did NOT fire. So the
draw is decoded/accepted into the display buffer but not pushed to the panel — likely a
flush/partial-frame condition inside `MaschineStudio::onDisplayMessage`, or the push is on
a timer/thread our single-shot client didn't reach. Next: read
`MaschineStudio::onDisplayMessage` to find what triggers `drawDisplayEx`.

### Finding 4 — with focus, OUR synthetic full-screen 0x20 draws are ACCEPTED too
Ran client with acquire + synthetic full-screen format-0x20 fill (no replay). Both displays:
`*** DRAW ACCEPTED ('true') ***`, and `MaschineStudio::onDisplayMessage` fires for both
(msgid 0x03647344). So uncompressed 0x20 full-screen draws work once we hold focus.
BUT `drawDisplayEx` (USB push) still does not fire — for our draws OR M2's replay. So the
frame is accepted into the display buffer but not presented to the panel. Suspect a
device-display-mode / "software takeover" step (panel still shows firmware idle prompt) or a
present/flush trigger. Investigating onDisplayMessage → drawDisplayEx path + setDeviceMode.

### Finding 5 — THE FULL CHAIN, CRACKED. Our draws reach the USB push.
`MaschineStudio::onDisplayMessage`:
```
if (msgid&0xffffff == 0x647344) {
    if (this->byte[0x39] == 0) return;            // per-display "active" gate
    (this+0x310)->vtable+0x70(msg);               // = BulkDisplayImplementation::onDrawDisplayMessage(MsgDisplayDraw&)
}
```
The push fn is **`BulkDisplayImplementation::onDrawDisplayMessage`** (0x102299ddc) — NOT
`LCDDisplay::drawDisplayEx` (which never fires; I was watching the wrong symbol).
Breakpointing the push call with our client running: **"PUSH CALL REACHED (gate passed)"**
fires — so our accepted draw is pushed to USB. Some draws hit "gate FAILED" (ctrl+0x39==0)
— a per-display active/sleep flag (set in `ControllerBase::connect` from vtable+0x170);
worth confirming both displays are awake, but at least one pushes.

## COMPLETE RECIPE to drive the Maschine Studio display (from our own client)
1. Connect to `NIHWMainHandler` (NIHardwareAgent), **v3 prefix 0x03** (force it; HIA/HW
   version replies are misleading).
2. `Du subscribe 0x1300` (+register notif port via 0x404300 with ZERO 8-byte field, not the
   old sentinels; + subscribeVoidOp 0x447143).
3. `DeviceConnect 0x1300` (ASCII serial) → local controller port
   `NIHWMaschineStudioController-<serial>NNNN`.  (Register + subscribeVoidOp on it too.)
4. **CLAIM FOCUS: send `0x03434300` + body `'strt'` (0x73747274)** on the controller port.
   → `onControllerAcquire` → `setFocusClient(us,true)` → ctrl+0x150 = us.  ← THE KEY STEP.
5. Draw: `0x03647344` envelope + WriteWindowRequest(op 0x0084, id=display, format 0x60 or
   0x20) + pixels + EndOfUpdateRequest, on the controller port. Now accepted ('true') and
   pushed via `onDrawDisplayMessage`.
   - format 0x20 = uncompressed RGB565 (full w*h*2 bytes) — WORKS (accepted).
   - format 0x60 = RGB565 + compression (M2's default, ~2.5KB/frame).
Open: the per-display ctrl+0x39 "active" gate (sleep/wake) — ensure both panels awake.

### Finding 6 — ✅ SOLVED, BOTH PANELS. Our client drives the Studio display over USB.
Corrected the "gate-FAILED" false alarm: `0x100039d84` is BOTH the cbz-skip target AND the
return address after the push (`blr x9` at 0x100039d80 returns to +0x84). So those hits were
post-push returns, not failures. Live proof:
- `ctrl+0x39` gate = **0x01 for BOTH display 0 and display 1** (same controller instance).
- **onDisplayMessage calls == onDrawDisplayMessage (USB push) calls, 1:1 (10==10).**
  Every accepted draw is pushed to USB. No gate failures.

**RESULT: our from-scratch client (`StudioDemo`) is driving BOTH Maschine Studio
panels over USB**, cycling colours via the persistent `MACCHINA_LOOP_DRAW=1` loop.

The complete, verified protocol is the recipe in Finding 5. The single missing insight that
unlocked everything was step 4: **CONTROLLER ACQUIRE (`0x03434300`+'strt') to become the
focus client** — without it the display-owner guard (`ctrl+0x150 == sender`) rejects every
draw, which is why replaying M2's exact frame failed. With it, draws are accepted and pushed.
