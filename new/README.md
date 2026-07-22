# new/ — Maschine Studio v3 client

A from-scratch client that speaks NI's modern (**v3 / NHL2**) local IPC dialect and
fully drives a **Maschine Studio**: the two 480×272 colour displays, every LED (RGB
pads + group buttons, both VU meters incl. colour, the jog-wheel ring, buttons), and
all input (pads, buttons, knobs, the jog wheel) — talking only to `NIHardwareAgent`.

## Layout
- **`core/`** — generic NHL2 v3 client library, device-agnostic. `NIRawPort`
  (CFMessagePort transport), `NIAgentConnection` (connect/subscribe/establish),
  `NIResponse`, `NIHexDump`. Nothing Studio-specific lives here.
- **`studio/`** — `NIStudioController`: the Studio façade — connect + display focus,
  `drawDisplay:`, the LED channel map + `setPadLED:`/`setGroupLED:`/`setVU:`/
  `setJogRingSegment:`/`setVUBlue:`, and input decode → delegate.
- **`clients/StudioDemo/`** — demo app (screens + LED demos + input log).
- **`tools/`** — `LedProbe` (step channels, label + capture input id → TSV),
  `LedPoke` (toggle/set any channel), `HIAClient` (NIHostIntegrationAgent research),
  `attic/` (parked dead scaffolding + legacy tests).
- **`docs/`** — [protocol.md](docs/protocol.md) (read first), the investigation logs
  ([studio-investigation.md](docs/studio-investigation.md),
  [LEARNINGS.md](docs/LEARNINGS.md), [longform_research.md](docs/longform_research.md)),
  and [reference/](docs/reference/) (the LED→channel maps).

## Build & run
```bash
./build.sh                    # → build/{StudioDemo,LedProbe,LedPoke,HIAClient}
./build/StudioDemo            # displays + breathing LEDs + input log
MACCHINA_PADS=1 ./build/StudioDemo   # pads + groups RGB rainbow
MACCHINA_VU=1   ./build/StudioDemo   # bouncing VU meters (+ colour)
MACCHINA_JOG=1  ./build/StudioDemo   # fill the jog ring
./build/LedPoke               # interactive: type "<ch> <level>" to poke any LED
```
Needs `NIHardwareAgent` running and a Studio plugged in. **Only one client may drive the
device at a time** — `pkill` stray clients first, or the next one looks broken.

## How it works (short version)
1. Connect `NIHWMainHandler`, force the v3 prefix `0x03`.
2. `Du subscribe 0x1300` → register receive port (`0x404300`) → subscribe void-op.
3. `DeviceConnect 0x1300` (serial) → controller port; register + subscribe.
4. **Claim display focus** — `0x03434300` body `'strt'`. Draws/LEDs are only accepted
   from the focus client.
5. Draw: `0x03647344` bulk envelope, **big-endian RGB565** pixels.
   LEDs: `[0x036c7500][u32 count=213][213 level bytes]` (the count field is mandatory —
   without it the buffer lands shifted −4). Input arrives on the Notification port.

Full protocol, the LED channel map, and the decode story are in [docs/](docs/).
