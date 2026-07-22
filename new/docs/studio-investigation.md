# Driving the Maschine Studio — reverse-engineering investigation log

A working log of how we tried to make our own code drive a **Maschine Studio**'s
colour displays, what we found, **how we found it, and where we looked**. Written so
it's resumable without re-deriving anything. Companion to
[studio-display.md](studio-display.md) (display-format specifics) and
[protocol.md](protocol.md) (message tables).

> **Status: ✅ SOLVED (2026-07).** `new/clients/StudioDemo` now drives **both**
> Studio panels over USB. The blocker was **display focus**, not the payload: draws are
> accepted only from the focus client, claimed with `0x434300`+'strt'
> (`onControllerAcquire → setFocusClient`). Draws ride the **controller** connection on
> **NIHardwareAgent** (not a subscription, not HIA). Full narrative:
> [../new/docs/longform_research.md](../new/docs/longform_research.md) (Findings 1–6)
> and [../new/docs/LEARNINGS.md](../new/docs/LEARNINGS.md) E17; recipe in
> [studio-display.md](studio-display.md). Notes below predate the solve — kept for the
> methodology.

---

## 0. The goal and the short version

Goal: push pixels to the Studio's two 480×272 colour panels from our own client. **Done.**

Short version of the **final** conclusion (see the SOLVED banner + `../new/
longform_research.md`):
- The Studio's display draw message is `0x647344` "Dsd" (same tag as the MK1), sent on
  the **controller connection** to **`NIHardwareAgent`** directly.
- The one gate was **display focus**: `ControllerBase::onClientRequest` accepts a draw
  only from the focus client (`controller+0x150 == sender`). You become the focus client
  by sending `0x03434300` + `'strt'` (`onControllerAcquire → setFocusClient`) after
  connect+register. Then draws are accepted and pushed to USB via
  `BulkDisplayImplementation::onDrawDisplayMessage`.

> **What we believed mid-investigation (and why it was wrong), for context on the notes
> below:** we thought the blocker was a `MsgConnectionEstablished` completion handshake on
> our receive port (§6) and that `NIHostIntegrationAgent` was the real renderer (§5). Both
> were dead ends — the "completion" frame was just the `0x444e2b` device-added
> notification, and `NIHardwareAgent` renders the Studio itself. The draw guard in §4 that
> looked like a hardcoded `setResult(false)` is actually the focus check. Sections 4–9
> below are the trail that led here; trust §0 and the banner over any conclusion in them.

---

## 1. Tooling & techniques (the HOW — reusable)

### 1a. Talking to the agent as a client
`NICommon` already speaks the IPC (CFMessagePort). We wrote throwaway probes in the
scratchpad that link `NICommon` + `new/core/NIHexDump` and send raw frames by wrapping
bytes in `NIUnknownMessage`:
```
clang -fobjc-arc -x objective-c -INICommon -INITooling -Wall -mmacosx-version-min=10.7 \
  -framework Foundation NICommon/*.m new/core/NIHexDump.m <probe>.m -o <probe>
```
Scratchpad probes (all under the session scratchpad dir):
`probe.m` (connect + log notifications), `reqoracle.m`/`oracle.m` (replay frames at the
**real** agent to read its replies), `studiodraw*.m`, `dc.m`, `dc5.m` (connect + draw
attempts), `roundtrip.m` (encode↔decode check).

### 1b. The "oracle" technique
To learn what a *correct* reply looks like, replay a captured/crafted frame at the
**real** running agent and dump its reply (`reqoracle.m`). This is how we learned the
v3 `GetServiceVersion` reply (`{0x00020802, 3}`), the port-pair reply format, and that
`0x447143` wants an *empty* reply.

### 1c. Live-debugging the agent (the big unlock)
The real `NIHardwareAgent` is signed with **hardened runtime**; SIP + no `get-task-allow`
means `lldb` attach is denied (confirmed: `codesign -dv` shows `flags=0x10000(runtime)`;
`csrutil status` = enabled; attach → "Not allowed to attach to process"). Workaround that
**works**:
1. `cp -R "/Library/Application Support/Native Instruments/Hardware/NIHardwareAgent.app" $SCRATCH/AgentCopy.app`
2. Re-sign the copy ad-hoc *with* `get-task-allow`:
   `codesign -f -s - --entitlements dbg.entitlements <binary>` where the plist just sets
   `com.apple.security.get-task-allow=true`. (Library validation still passes; the copy runs.)
3. `pkill -x NIHardwareAgent`, run the copy — it grabs the ports **and** the real USB
   device (verified: `this+0x39` device-connected flag = 1; it drives the panels).
4. `lldb --batch -p <pid> -s cmds.lldb` attaches fine.

> **This Mac runs the arm64 slice.** The binary is fat (`lipo -archs` → `x86_64 arm64`).
> Early on I disassembled the **x86_64** slice by mistake (`agent-disasm.txt`) and reasoned
> off the wrong code — that's why an early "`this+0x39` gate" hypothesis was wrong. Always
> `otool -tv -arch arm64` and use **symbol-name** breakpoints (ASLR slide changes the
> runtime address every launch, so hardcoded addresses silently point at nothing).

lldb patterns that worked (and the ones that didn't):
- Read msgid at a handler: `p/x ((*(unsigned int*)($x2+0xc)) & 0xffffff)` — the parsed
  `Message` object holds its id at `+0xc`; the low 3 bytes are the tag (`& 0xffffff`).
- Get a caller: `image lookup -a $lr` (works at function entry; a bare `bt` often shows
  only frame 0 there).
- Inline Python (`script print(... ReadMemory ...)`) was **unreliable** in breakpoint
  commands (kept reading `0x1`); use the `p/x` expression form instead.
- Conditional breakpoint on a specific message:
  `breakpoint set -n <sym> -c '((*(unsigned int*)($x2+0xc)) & 0xffffff) == 0x647344'`.
- Deref a `shared_ptr<ClientEntry>` arg (x3) to dump the object: `x/24xg *(void**)$x3`.

### 1d. Static disassembly artifacts
- `agent-disasm.txt` — `otool -tv` of NIHardwareAgent, **x86_64 (wrong slice, kept for
  reference only)**.
- `arm64.txt` — `otool -tv -arch arm64` of NIHardwareAgent (the slice that runs).
- `hia_arm64.txt` — `otool -tv -arch arm64` of NIHostIntegrationAgent.
- `strings - <binary>` for literal pools (port-name templates, log format strings — these
  were very high-signal, e.g. `Forwarded CONNECT request for device 0x%08X`).

---

## 2. Hardware facts (and how)
- Device = **Maschine Studio** (2013). USB `0x17cc:0x1300` (`ioreg -p IOUSB -l | grep
  'Maschine Studio'`). Serial `39195855`, firmware `0x21` — read live via
  `GetSerialNumber`/`GetFirmwareVersion` on the request port.
- The 2012 `Macchina` code targets the **MK1** (`controllerId 0x0808`, 255×64 mono
  ST7529). We connect with `0x1300`. See [studio-display.md](studio-display.md).

---

## 3. The v3 protocol (what + how)
Found by (a) impersonating the agent (`MacchinaServer` + `NITooling` handlers) and
capturing what NI's software sends on `NIHWMainHandler`, and (b) the oracle replays.
- Message ids: top byte is a **negotiated protocol version** (`0x02`→`0x03` once we answer
  `GetServiceVersion`). Dispatch masks `id & 0xffffff`.
- Handshake on `NIHWMainHandler`: `GetServiceVersion 0x03536756` → 8-byte `{0x00020802, 3}`;
  then per device `Du/subscribe 0x03447500` = `[msgid][product u32][tag 'NiM2'=0x4e694d32]
  [role 'prmy'=0x70726d79][0]`; and for the target device `DeviceConnect 0x03444900` =
  `[msgid][product 0x1300]['NiM2']['prmy'][strlen=9]["39195855"]` (the trailing **serial
  string** — 29 bytes total = the `0x1d` length field we saw). Decoded the DeviceConnect
  layout by breakpointing `connectControllerClient` and `x/40xb $x2` on the message object.
- `boh` decoded: it's the **client-software id** (`NiMS`=Maschine 1, `NiM2`=Maschine 2).

---

## 4. Message routing & the ClientEntry linkage (what + how)

This is the crux of why our draws fail. Established with the debuggable copy:

- Our draw returns `0x00000000`. Traced it: in `arm64.txt`, `ControllerBase::onClientRequest`'s
  `0x647344` branch reaches `setResult(false)` (`mov w1, #0x0; bl MsgDisplayDraw::setResult`).
  Found by locating the `0x647344` constant (`mov w9,#0x7344; movk w9,#0x64`) in the function.
  **[Correction, per the solve] that `setResult(false)` is not hardcoded — it's the
  `else` of a focus check** (`ldar x22,[ctrl+0x150]; cmp x22, sender; b.eq accept`). It
  fires for us because we hadn't acquired focus; with `0x434300`+'strt' the same branch
  takes `b.eq accept`. See `../new/docs/longform_research.md` Finding 1.
- Maschine-2-era draws instead go `IPCServer::onClientRequest(…, shared_ptr<ClientEntry>) →
  MaschineStudio::onDisplayMessage → draws`. Found by breakpointing `onDisplayMessage` and
  resolving `$lr` → `IPCServer::onClientRequest + 2064`.
- The two paths are chosen at connect time by `IPCServer::handleRequest`, which calls either
  `connectControllerClient(MsgDeviceConnect)` **or** `connectSubscriptionClient(MsgDeviceSubscribe)`.
  Found by listing callers of those two symbols.
- Comparing the `ClientEntry` of a *working* draw vs ours (`x/24xg *(void**)$x3` at
  `onSubscriberMessage`/`onClientRequest`): the working entry has an **app/project name
  ("New Project")** and **linked controller pointers at +0x50** (same addresses across
  captures = the shared Studio controller); ours has neither. So our connection is *not
  linked to a controller*.
- Full inbound message stream (the "log everything" the user pushed for): there is **no
  single chokepoint** — each per-connection port has its own handler. `IPCReceivePort::callback`
  never fired; the real per-message dispatchers are `handleRequest` (main port),
  `onSubscriberMessage` (subscription ports), `onClientRequest` (controller ports). Tapping
  all three with labels gave the ordered stream: per device `version+Du`→`register+subscribe`;
  for the Studio `version+DeviceConnect`→ a burst of property GETs (`0x566775`, `0x636749`,
  `0x63674e`) on the controller port, then draws. `__CFMessagePortPerform` (CoreFoundation)
  is the true transport-level per-message point (`x1`=length, `x3`=the CFMessagePort) if a
  single tap is ever needed.

---

## 5. Architecture: two daemons (what + how)
Triggered by the user's question "what does NIHostIntegrationAgent do?". Found via
`strings` + `otool` on `NIHostIntegrationAgent.app/Contents/MacOS/NIHostIntegrationAgent`:
- It contains the **same `IPCServer`/`ClientEntry` code**, the Studio **`DisplayDrawer`**
  (`renderText/renderButton/renderList/renderTitle/…`) and **`MapHandler`/`PadHandler`**,
  `IPCConnection` client primitives, and the string `NIHWMainHandler`.
- Log strings reveal its role: `Forwarded CONNECT request for device 0x%08X`,
  `Forwarded CONNECTID request…`, `IOConnectCallMethod(GetDeviceInfo)…`.

```
Maschine 2  →  NIHostIntegrationAgent            →  NIHardwareAgent   →  USB → Studio
 (app)          broker: forwards CONNECT/CONNECTID    owns USB, low-level
                renderer: DisplayDrawer/MapHandler     pixel push (onDisplayMessage)
                also IOKit GetDeviceInfo
```
Implication: the working `0x647344` draws we saw reaching `NIHardwareAgent` were almost
certainly sent by **`NIHostIntegrationAgent`**, not Maschine 2. Apps connect to
HIA's own service (`com.native-instruments.NIHostIntegrationAgent`).

---

## 6. The `establish` handshake & `MsgConnectionEstablished` (what + how)
Read from `hia_arm64.txt`, function `IPCConnection::establish(MsgDeviceConnect,
function<bool(ull,Message&)>, function<void(ull)>, bool)` (listed its `bl` targets in order):
1. `MessageSendPort::sendMessageToConnectionPort` (×3) — sends the connect to the connection
   port; the port name string is **`com.native-instruments.NIHostIntegrationAgent`** (so this
   `establish` is the **app→HIA** connection).
2. `MsgDeviceConnect::setIDString(...)` — a **UTF-16** id (not the ASCII serial we sent).
3. Reads assigned **in/out port names** (`getInPortName`/`getOutPortName`) from the reply.
4. Creates a **send port** to the in-port and a **receive port named as the out-port**, and
   registers a **`receiveEvents` callback** on it.
5. Completion: the server side has `IPCConnection::establish(MsgConnectionEstablished,
   function<void(ull)>)` — the agent connects back and the client must **answer a
   `MsgConnectionEstablished`** on that receive port.

**Root cause, precisely:** our probes created a receive port (`NIServer`) but returned `nil`
to whatever arrived, so the `MsgConnectionEstablished` completion never happened → the
`ClientEntry` was never linked to a controller → draws rejected. This is the single missing
piece. `MsgConnectionEstablished`'s exact tag/bytes are still undecoded.

> **[SUPERSEDED] This root-cause theory was wrong.** The completion handshake was a red
> herring: the first inbound frame on our receive port is the `0x444e2b` "DN+"
> device-added notification (LEARNINGS E1), not a completion we must answer, and replying
> to it changes nothing. The real missing piece was **display focus** (`0x434300`+'strt'),
> unrelated to the receive port. Kept because §6's *reading of `establish`* (the port-pair
> exchange, UTF-16 id string) is accurate and still useful.

---

## 7. Where everything lives
- Repo tooling: `new/tools/attic/` (hexdump, capture tap, ST7529 decode, display-draw category),
  `NIToolingTests/`, `build.sh` targets `tests` and `server-tap`.
- **`new/`** — the productized follow-up to the scratchpad probes: `StudioDemo`
  (NIHardwareAgent) and `HIAClient` (NIHostIntegrationAgent) on a shared raw-frame
  core, with the establish reply switchable via `MACCHINA_ESTABLISH_REPLY`
  (echo/true/empty/none). `build.sh` targets `hwclient`/`hiaclient`/`newclients`.
  See `new/README.md`.
- Scratchpad (session dir `…/scratchpad/`): re-signed `AgentCopy.app`; disasm dumps
  `arm64.txt` (HWAgent), `hia_arm64.txt` (HIA), `agent-disasm.txt` (x86_64, wrong slice);
  probes `dc.m`/`dc5.m`/`studiodraw*.m`/`reqoracle.m`/`probe.m`; lldb scripts `bp.lldb`;
  logs `*.log`.
- Memory: `debuggable-agent-technique.md`, `maschine-studio-hardware.md`.

Key symbols (arm64) to breakpoint next time: `IPCServer::handleRequest`,
`IPCServer::connectControllerClient` / `connectSubscriptionClient`,
`IPCServer::onClientRequest` (3-arg), `IPCServer::onSubscriberMessage`,
`MaschineStudio::onDisplayMessage`, `Interfaces::LCDDisplay::drawDisplayEx`,
`IPCConnection::establish`.

---

## 8. Open questions / next steps
The original blocker is **closed** — fork (a) below won, via display focus, not a
completion handshake. Remaining threads:
1. ~~Decode `MsgConnectionEstablished`~~ — moot; it never existed as a gate (see §6 note).
2. ~~Decide the fork~~ — **(a) direct controller connection to `NIHardwareAgent` won.**
   Fork (b) (drive `NIHostIntegrationAgent`'s app service / the semantic "SMap" Map
   protocol) remains un-attempted and is the natural next project — see
   [studio-display.md](studio-display.md) Path B.
3. Confirm the exact RGB565 channel/byte order on a photographed fill (we have
   accepted-and-pushed; colour-accuracy is the last unverified visual detail).
4. Input events (pads/wheels/encoders) and LEDs over this same focused controller
   connection — the display is proven; the rest of the surface is the follow-on work.

---

## 9. First live contact with HIA from our own client (2026-07-21, `HIAClient`)

First facts from talking to the **running** `NIHostIntegrationAgent` on its service port
`com.native-instruments.NIHostIntegrationAgent` (no capture, our frames):

- `GetServiceVersion` (`0x02536756`) → **4 bytes `0x00010f00`** — *not* the HW agent's
  8-byte v3 reply `{0x00020802, 3}`. So HIA's main port either speaks a different
  version-negotiation scheme or `0x536756` means something else there. Our client
  therefore stayed on the `0x02` prefix for the attempts below.
- All three connect candidates — DeviceConnect `0x444900` with UTF-16LE id string,
  DeviceConnect with ASCII serial, and `Du` subscribe `0x447500` (each with product
  `0x1300`, `'NiM2'`/`'prmy'`) — were answered with the **same 4 bytes
  `6c 74 63 65`** (LE u32 `0x6563746c`, 4CC **`'ectl'`**). A structured refusal/status
  code, not a hang and not the HW agent's `0x00000000`: the port parses our frames and
  has an opinion. **Since explained** (`new/docs/LEARNINGS.md` E1/E3): like the HW
  agent's `'dice'`, it is the result 4CC `IPCServer::handleRequest` leaves standing when
  it declines to forward a connect it doesn't accept — for the HW agent the cure was the
  `0x03` prefix. HIA's version reply `0x00010f00` keeps our client pre-v3, and its
  accepted opening dialect is still unknown.
- Practical read: HIA's app-facing dialect differs from the HW agent's device dialect at
  the very first message. The §8.1 plan stands — capture a real app↔HIA connect (debuggable
  HIA copy) — and now there's a precise diff target: whatever the app sends first vs. our
  `0x02536756`, and what turns `'ectl'` into a port-pair reply.
