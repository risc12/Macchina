# new-cpp — the C++ port

C++20 port of `new/` (the ObjC v3/NHL2 client), CoreFoundation-only, no
Foundation/ObjC. "NHL2" is NI's own name for this protocol layer, taken from
the `NI::NHL2::*` symbols in the shipped agent — we keep their name so the
code cross-references cleanly against the agent's disassembly.

## What this wants to be

The end-game is a **daemon that owns the device** and exposes it over a plain
socket/OSC boundary, so people can write adapters that make their controller
useful with Ableton (a Remote Script is just a socket client), Max for Live,
or anything else — long after NI's own software has moved on.

On the way there, this tree has a second job: being **the readable reference
for the protocol**. Someone should be able to learn the NHL2 wire dialect from
this code and port it to Rust, Python, or Go without reading a line of ObjC or
a disassembler pane. Code quality here is judged by that standard:

- **The layering is the documentation.** Includes are the architecture
  diagram: if a file includes CoreFoundation, it's platform plumbing; if it's
  in `macchina::nhl2`, it's a wire-protocol fact a porter must reimplement;
  if it only sees `Frame.hpp`, it's generic. Keep that signal honest — it's
  the first thing that rots.
- **Wire facts live exactly once**, as named constants/shapes in
  `core/protocol/` — never as magic numbers at call sites. The prose ground
  truth stays in `new/docs/protocol.md`; code comments cite it for the *why*
  (and for evidence: "LEARNINGS.md E11"), the code states the *what*.
- **Small surface over clever abstraction.** Plain structs, `std::function`
  callbacks, no frameworks. A porter should map every type here onto their
  language's stdlib in an afternoon.
- **Keep the experiment hooks.** This protocol is reverse-engineered and
  unfinished (`boh` = still undecoded). Runtime switches like
  `MACCHINA_ESTABLISH_REPLY` exist so probing a hypothesis is a re-run, not a
  recompile — don't clean them away, and add new ones in the same style when
  you chase a new unknown.

## Where to experiment

- **Unknown frames / handshake behaviour** — `core/client/AgentConnection.cpp`
  (`openEndpoints` handler + `establishReply`); every frame is hexdumped by
  default, `setQuiet(true)` per port when it drowns you.
- **New messages** — add the tag to `core/protocol/Protocol.hpp`, build the
  body with `appendU32`, fire it from a copy of `tools/HandshakeSmoke/`.
  One-off probe binaries are cheap: new dir under `tools/`, one line in
  `../build.sh`.
- **Raw transport** — `core/transport-macos/` is the only place the OS
  exists; everything above it can be exercised against a fake Transport.

## Layering (strict, dependency arrows point down)

```
core/Frame.hpp         the one shared type: a wire message, verbatim.
core/Controller.hpp    the device-agnostic controller interface (+ InputEvent)
                       — the boundary the future daemon programs against.
core/transport/        abstract Transport / RequestPort / ReceivePort + HexDump.
                       NO platform includes, ever.          (namespace macchina)
core/transport-macos/  the CFMessagePort implementation — the ONLY place
                       CoreFoundation may be included.
core/protocol/         NHL2 wire vocabulary: tags, 4CCs, frame helpers,
                       parsed reply shapes. Pure data.  (namespace macchina::nhl2)
core/client/           AgentConnection — the v3 handshake flow, written
                       against the abstract Transport only.
studio/                everything Maschine-Studio-specific: StudioController
                       (connect+focus recipe, channel maps, RGB565 draw, the
                       213-byte LED report, input decode) + StudioLayout (the
                       control-name ↔ buttonId ↔ LED-channel table).
                                                     (namespace macchina::studio)
gfx/                   host-side rasterizer, pure pixels (no transport, no
                       device): RGB565 Surface + built-in 5×7 font. The
                       screens are dumb framebuffers — M2 rasterizes host-side
                       too (new/docs/rendering-pipeline.md).
                                                        (namespace macchina::gfx)
daemon/                macchinad → build/macchinad — owns the device, exposes
                       it on udp://127.0.0.1:7000 (MACCHINA_OSC_PORT): /led,
                       /pad/rgb, /vu, /ring, /display/* in; /event/* out to
                       the subscriber. The full address list is the header
                       comment in daemon/main.cpp. Studio-specific for now.
daemon/osc/            minimal OSC 1.0 codec (messages, i/f/s/b args). Pure
                       data, no sockets.                (namespace macchina::osc)
daemon/ableton/        the Ableton Live Remote Script (Live 11/12) — a thin
   Macchina/           OSC client of macchinad: transport + master volume +
                       tempo on the wheel, VU + HUD on the hardware.
clients/               runnable apps (StudioDemo → build/StudioDemoCpp,
                       StudioProbe → build/StudioProbe,
                       HelloScreen → build/HelloScreen — text on glass,
                       Showcase → build/Showcase — the integral demo:
                       screens as live UI + LEDs as feedback for every input).
tools/                 runnable probes (HandshakeSmoke).
```

The `core/` ↔ `studio/` rule from CLAUDE.md carries over: anything with a
channel number, product id `0x1300`, or RGB565 in it belongs in `studio/`,
never `core/`. The daemon touches the device only through `core/Controller.hpp`
+ the `StudioLayout` vocabulary (which doubles as the OSC name space).

## The Ableton integration

```
Live 12 ── Remote Script (python, UDP) ──> macchinad ──> NIHardwareAgent ──> Studio
```

1. `./build.sh cpp && ./build/macchinad` (needs the agent + device, as always)
2. `cp -r new-cpp/daemon/ableton/Macchina ~/Music/Ableton/User\ Library/Remote\ Scripts/`
3. Live > Settings > Link/Tempo/MIDI > Control Surface: **Macchina**
   (input/output: None)

Play/Rec/Restart/Metro drive the transport (LEDs follow Live's state), the
master encoder is master volume, the wheel is tempo (push = 0.1 BPM steps),
the hardware VUs show Live's master meters, and the left screen is a
tempo/position HUD. Quit Live (or unload the script) and macchinad keeps
running for the next front-end.

## Build & run

```bash
../build.sh cpp        # → build/HandshakeSmoke, build/StudioDemoCpp
./build/HandshakeSmoke [serial]   # needs NIHardwareAgent running + Studio on
./build/StudioDemoCpp  [serial]   # displays + breathing LEDs + input log
../build.sh compdb     # regenerate compile_commands.json for clangd
```

`HandshakeSmoke` runs the full v3 handshake and proves it with a
GetSerialNumber round-trip; it deliberately stops before claiming display
focus, so it never disturbs whatever is currently driving the device.

`StudioProbe` is the interactive mapper: press any control and it lights up +
prints its name from `studio/StudioLayout.cpp` (or its raw id with an
"add it!" nudge when the table doesn't know it — the tool exists to finish
that table; note the old `led_map.tsv` channels carry a +4 shift). The
terminal is a LedPoke-style REPL: `<chan> <level>`, `all`, `off`,
`what <chan>`, `list`. The wheel walks a cursor around the jog ring.

`StudioDemoCpp` is the full proof: connect + focus, red/blue display fill,
LED animations, input logging. Same env switches as the ObjC demo
(`MACCHINA_PADS`, `MACCHINA_VU`, `MACCHINA_JOG`, `MACCHINA_LED_ALL`,
`MACCHINA_LED_CHASE`, `MACCHINA_NO_DRAW`), plus `MACCHINA_RUN_SECS=N` to
exit cleanly (LEDs blanked) after N seconds for scripted runs. It claims
focus and drives the device — never leave it running alongside another
client.
