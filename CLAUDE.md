# CLAUDE.md — Macchina

Guidance for AI assistants (and humans) working in this repo.

## What this is
Reverse-engineering of the **Native Instruments Maschine Studio** — a hardware USB
controller plus NI's software. It reimplements the local IPC protocol NI's
`NIHardwareAgent` daemon speaks (the **v3 / NHL2** dialect), so we can drive the
controller from our own code: push RGB images to the two 480×272 displays, drive every
LED (RGB pads + group buttons, the two VU meters incl. colour, the jog-wheel ring, all
buttons), and receive pad / button / knob / wheel input. macOS-only proof-of-concept.

**Status: the Studio is fully driven** — displays, LEDs, and input all working from our
own client. The original 2012 code (Antonio "Willy" Malara) targeted the MK1 and is kept
as legacy.

## Layout
The current work lives under **`new/`**; the 2012 code is untouched legacy.

- `new/core/` — **generic NHL2 v3 client library, device-agnostic.** Nothing Studio-specific
  belongs here.
  - `NIRawPort` — CFMessagePort raw-frame transport (+ hexdump logging via `NIHexDump`).
  - `NIAgentConnection` — the v3 connect/subscribe/establish handshake.
  - `NIResponse` — parses the connect reply's port-pair. `NIHexDump` — logging util.
- `new/studio/` — **everything Maschine-Studio-specific.**
  - `NIStudioController` — the façade: connect+focus, display draw, the LED channel map
    (pads/groups/VU/ring/colour) + helpers, and input decode → delegate.
- `new/clients/StudioDemo/main.m` — demo app: draws the screens, runs LED demos, logs input.
- `new/tools/` — interactive probes + research: `LedProbe` (step channels, label + capture
  input id → TSV), `LedPoke` (toggle/set any channel), `HIAClient` (NIHostIntegrationAgent
  research), and `attic/` (parked dead scaffolding + legacy tests).
- `new/docs/` — **read `protocol.md` first for any protocol work; update it when you decode
  something.** Also: `studio-investigation.md` (live-investigation log), `studio-display.md`
  (display format), `LEARNINGS.md` / `longform_research.md` (experiment logs),
  `reference/studio_led_map.txt` (the authoritative LED→channel map from M2) and
  `reference/led_map.tsv` (our probed map).
- **Legacy (committed, do not disturb):** `NICommon/` (2012 MK1 library), `MacchinaClient/`,
  `MacchinaServer/`, `Macchina.xcodeproj`.

The `core/` ↔ `studio/` split is a hard rule: the moment code needs a channel number,
product id `0x1300`, RGB565, etc., it belongs in `new/studio/`, never `new/core/`.

## Build & verify
```bash
./build.sh            # build the new/ Studio targets → build/{StudioDemo,LedProbe,LedPoke,HIAClient}
./build.sh studio     # just StudioDemo
./build.sh check      # syntax-check all new/ sources
./build.sh legacy     # the 2012 MacchinaClient + MacchinaServer (NICommon)
./build.sh all        # both
```
- ARC, Foundation-only, deployment target 10.7. No manual `retain`/`release`.
- `build.sh` is the fast IDE-independent feedback loop; prefer it for compile checks.

## Running (needs real hardware + NI software)
`StudioDemo` and the tools need `NIHardwareAgent` **running** and a Studio plugged in.
Only ONE client may drive the device at a time — `pkill` stray clients first. Examples:
```bash
./build/StudioDemo                 # displays + breathing LEDs + input log
MACCHINA_PADS=1 ./build/StudioDemo # pads+groups RGB rainbow
MACCHINA_VU=1   ./build/StudioDemo # bouncing VU meters (+ colour flip)
MACCHINA_JOG=1  ./build/StudioDemo # fill the jog ring
./build/LedPoke                    # interactive: type a channel/level to toggle
```

## Key protocol facts (full detail in `new/docs/protocol.md`)
- Message ids are little-endian 4-byte tags; dispatch masks `id & 0xffffff`; the top byte
  is the protocol version (`0x03` = v3).
- **Display focus** must be claimed (`0x03434300` body `'strt'`) before draws/LEDs are
  accepted for our connection.
- **Display**: `0x03647344` bulk draw; pixels are **big-endian RGB565**.
- **LED set message**: `[msgid 0x036c7500][u32 count=213][213 level bytes]`. The count field
  is required — without it the buffer lands shifted −4 and truncates the last 4 channels.
  Level is 7-bit (`0`=off … `0x7f`=full); the agent only re-pushes a *changed* report, so
  prime with a full write after connecting.
- **Input** arrives on the controller Notification port: pads `0x03504e00`, switches
  `0x03734e00`, knobs `0x03654e00`, wheel `0x03774e00`.

## Live reverse-engineering workflow
Most facts were found live, not from source (detail in `new/docs/studio-investigation.md`):
- **Device must be physically on.** The user powers/replugs; a fresh replug clears a
  half-claimed / black-screen state.
- **Drive NI's own software to generate traffic.** `open -a "Maschine 2"` (~30 s boot) does
  the real handshake/draws/LED updates. **Always quit it when done**
  (`osascript -e 'quit app "Maschine 2"'`) — it holds the device.
- **Daemons:** start with `open <app>.app`, stop with `pkill -x NIHardwareAgent` /
  `pkill -x NIHostIntegrationAgent`. Two of them: `NIHardwareAgent` (owns USB, drives LEDs
  + displays) and `NIHostIntegrationAgent` (broker).
- **Debugging the shipped agent** (hardened-runtime + SIP blocks lldb): use the re-signed
  copies staged in **`.debug-agents/`** (gitignored) — `AgentCopy.app` / `HIACopy.app` are
  ad-hoc-signed with `get-task-allow`. `pkill` the real daemon, `open` the copy (it grabs
  ports + USB), then `lldb -p <pid>`. This Mac runs **arm64**; `break set -r` matches
  **demangled** names.
- **M2 uses Lua** (`Maschine 2.app/Contents/Resources/Scripts/**`) over a fully-symboled
  arm64 binary — the LED/ring/meter logic and the `s_ledToNHLMap` (LED→channel table) are
  the ground truth for hardware layout.
- **Be a good citizen:** afterwards restore the real daemons (`open` both) and quit copies.

## Conventions
- Objective-C, ARC, Foundation. Match the `NI` class-prefix style.
- `boh`/`boh1`… = undecoded byte(s), an open question. Decode → rename AND update
  `new/docs/protocol.md`.
- **Never leave a client running** — it holds LED focus and makes the next one look broken.

## Good next steps
- Fold the LED-value **factored mode** (bytes ≥ 0x80) and the full RGB **colour-LUT** scheme
  (M2's two-channel pad colour) into the façade if richer colour control is wanted.
- Enumerate the remaining button input-id ↔ channel map (partly captured in `led_map.tsv`).
- Error handling / graceful termination were deliberately skipped — still the biggest gap.
