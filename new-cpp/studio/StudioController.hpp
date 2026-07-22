//
//  StudioController.hpp
//  Macchina — new-cpp/studio
//
//  The Maschine Studio behind the Controller interface, hiding the whole
//  reverse-engineered v3 recipe (new/docs/protocol.md, LEARNINGS.md E17–E18):
//
//    connect → subscribe (Du) → DeviceConnect(serial) → acquire display focus
//
//  Once connected you can push pixels to the two 480×272 colour displays, set
//  any of the 213 LED channels, and receive pad / button / knob / wheel input
//  through onInput. Everything Studio-specific — channel maps, RGB565, the
//  LED report, input decode — lives here, never in core/.
//
//  Requires NIHardwareAgent RUNNING and a Studio plugged in. Events arrive
//  while the Transport's event loop runs, so keep run()/runFor()-ing.
//

#pragma once

#include "core/Controller.hpp"
#include "core/client/AgentConnection.hpp"

#include <array>
#include <memory>
#include <string>

namespace macchina::studio {

// ---- The Studio's shape (live-mapped 2026-07; see protocol.md "LEDs") ------

constexpr uint32_t kProductId       = 0x1300;   // USB product id, NI vendor 0x17cc
constexpr int      kDisplayCount    = 2;
constexpr uint16_t kDisplayWidth    = 480;
constexpr uint16_t kDisplayHeight   = 272;
constexpr size_t   kLedChannelCount = 213;

// RGB elements are 3 consecutive channels in R,G,B order; the rest are mono.
// Channels are the true (un-shifted) indices — flushLeds sends the count
// field M2 sends; without it the buffer lands shifted −4 on the device.
constexpr size_t kPadLedBase[2]   = {0, 62};    // pads 1–8, pads 9–16 (RGB runs)
constexpr int    kPadCount        = 16;
constexpr size_t kGroupLedBase    = 106;        // groups A–H (RGB run)
constexpr int    kGroupCount      = 8;
constexpr size_t kVULeftBase      = 158;        // 16 mono segments, bottom→top
constexpr size_t kVURightBase     = 174;
constexpr int    kVUSegments      = 16;
constexpr size_t kVUColorChannel  = 190;        // LED_LEVEL_COLOR: flips BOTH meters
constexpr size_t kJogRingBase     = 198;        // 15 mono segments, closed ring
constexpr int    kJogRingSegments = 15;

// Pad event states on the wire (InputEvent::padState).
enum PadState : uint32_t
{
    kPadHit      = 1,
    kPadRelease  = 3,
    kPadPressure = 4,
};

// Inbound event tags, already masked to 3 bytes (arrive with the 0x03 prefix
// on the controller Notification port). Record layouts in protocol.md
// "Inbound events"; decode is in StudioController.cpp.
enum EventTag : uint32_t
{
    kEvtPads     = 0x504e00,   // {u32 padId, u32 state, f32 pressure}
    kEvtSwitches = 0x734e00,   // {u32 id, f32 state}
    kEvtKnobs    = 0x654e00,   // {u32 id, f32 delta}
    kEvtWheel    = 0x774e00,   // {u32 id, i32 delta}
};


class StudioController : public Controller
{
public:
    StudioController(Transport & transport, std::string serial);

    /// Full connect + focus claim + LED-cache prime.
    bool connect() override;
    bool isConnected() const override { return connected_; }

    /// (Re)claim display + input focus. connect() already does this; call
    /// again if another client may have stolen focus.
    void acquireFocus();

    const std::string & serial() const { return serial_; }

    // --- Displays (index 0 = left, 1 = right) -----------------------------
    int      displayCount() const override  { return kDisplayCount; }
    uint16_t displayWidth() const override  { return kDisplayWidth; }
    uint16_t displayHeight() const override { return kDisplayHeight; }

    void drawDisplay(int index, const std::vector<uint16_t> & rgb565) override;
    void fillDisplay(int index, uint16_t rgb565) override;

    // --- LEDs (213 channels) ----------------------------------------------
    // Intensity is 7-bit direct level: 0 = off, 127 = full. Values ≥128
    // switch into the factored/global-brightness mode (protocol.md) — the
    // set* helpers clamp to 0–127.
    size_t ledChannelCount() const override { return kLedChannelCount; }
    void   setLed(size_t channel, uint8_t level) override;
    void   setAllLeds(uint8_t level) override;
    void   flushLeds() override;

    /// One RGB pad (0–15) / group button (0–7 = A–H). Components 0–127.
    void setPadLed(size_t pad, uint8_t r, uint8_t g, uint8_t b);
    void setGroupLed(size_t group, uint8_t r, uint8_t g, uint8_t b);

    /// Light the bottom `segments` (0–16) of a VU meter, the rest off.
    void setVU(bool left, size_t segments);

    /// Shared VU colour (BOTH meters): true = blue (input-monitor look),
    /// false = white (normal output metering).
    void setVUBlue(bool blue);

    /// One jog-ring segment (0 = top, clockwise, 0–14). Level 0–127.
    void setJogRingSegment(size_t segment, uint8_t level);

private:
    void handleEventFrame(const Frame & frame);

    Transport &                            transport_;
    std::string                            serial_;
    std::unique_ptr<nhl2::AgentConnection> conn_;
    std::unique_ptr<nhl2::Endpoints>       subscription_;
    std::unique_ptr<nhl2::Endpoints>       controller_;
    std::array<uint8_t, kLedChannelCount>  ledState_{};
    bool                                   connected_ = false;
};

} // namespace macchina::studio
