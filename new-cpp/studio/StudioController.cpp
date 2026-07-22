//
//  StudioController.cpp
//  Macchina — new-cpp/studio
//
//  Port of new/studio/NIStudioController.m. Everything here is the distilled
//  result of the reverse-engineering in new/docs/; the protocol constants are
//  the only "magic" left.
//

#include "studio/StudioController.hpp"
#include "core/transport/HexDump.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace macchina::studio {

using namespace nhl2;

namespace {

const uint8_t kDisplayDataFormat = 0x20;   // uncompressed RGB565 (works once focused)

// Clamp to the 7-bit direct-level range. 0 = off (a real level, not "no change").
uint8_t clampLevel(uint8_t v) { return v > 127 ? 127 : v; }

void appendBE16(Frame & f, uint16_t v)
{
    f.push_back((uint8_t)(v >> 8));
    f.push_back((uint8_t)v);
}

float readF32(const uint8_t * p)
{
    float v;
    memcpy(&v, p, sizeof(v));   // wire floats are little-endian IEEE754, like us
    return v;
}

uint32_t readU32At(const uint8_t * p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

} // namespace


StudioController::StudioController(Transport & transport, std::string serial)
    : transport_(transport), serial_(std::move(serial))
{
}

// --- Connect ----------------------------------------------------------------

bool StudioController::connect()
{
    conn_ = AgentConnection::open(transport_, "NIHWMainHandler");
    if (conn_ == nullptr)
    {
        fprintf(stderr, "StudioController: NIHWMainHandler not found — is NIHardwareAgent running?\n");
        return false;
    }
    if (!conn_->negotiateVersion())
        return false;

    // Subscription connection (device add/remove channel).
    auto sub = conn_->subscribe(kProductId, kClientTagMaschine2, kClientRolePrimary);
    subscription_ = conn_->openEndpoints(sub, "subscription");
    if (subscription_)
    {
        subscription_->setQuiet(true);
        conn_->registerNotificationPort(*subscription_);
        conn_->sendSubscribeVoidOp(*subscription_);
    }

    // Controller connection — this is the one that carries draws, LEDs, input.
    auto dev = conn_->connectDevice(kProductId, kClientTagMaschine2, kClientRolePrimary, serial_);
    controller_ = conn_->openEndpoints(dev, "controller");
    if (controller_ == nullptr)
    {
        fprintf(stderr, "StudioController: controller connect failed (serial %s).\n", serial_.c_str());
        return false;
    }
    controller_->setQuiet(true);
    conn_->registerNotificationPort(*controller_);
    conn_->sendSubscribeVoidOp(*controller_);

    controller_->onInboundFrame = [this](const Frame & frame) { handleEventFrame(frame); };

    // Let the receive port process the async MsgConnectionEstablished completion
    // before we claim focus (mirrors the proven ordering — LEARNINGS.md E17).
    transport_.runFor(0.3);
    acquireFocus();

    // Prime the agent's LED report cache. updateLEDs only re-pushes a report
    // that differs from its last-sent copy; after prior clients that cache is
    // stale, so a lone single-channel write may not reach USB. Flashing all-on
    // then all-off forces every report to change twice, resyncing to blank.
    setAllLeds(0x7f); flushLeds();
    setAllLeds(0);    flushLeds();

    connected_ = true;
    return true;
}

void StudioController::acquireFocus()
{
    Frame body;
    appendU32(body, kBodyStart);   // 'strt'
    conn_->sendTag(kTagControllerAcquire, body, *controller_->request);
}

// --- Displays ---------------------------------------------------------------

void StudioController::drawDisplay(int index, const std::vector<uint16_t> & rgb565)
{
    if (controller_ == nullptr || index < 0 || index >= kDisplayCount)
        return;

    // WriteWindowRequest + pixels + EndOfUpdateRequest.
    uint32_t payloadSize = 16 + (uint32_t)(rgb565.size() * 2) + 4;

    Frame frame;
    frame.reserve(0x14 + payloadSize);

    // 0x647344 envelope.
    appendU32(frame, conn_->messageIdFor(kTagDisplayDraw));
    appendU32(frame, (uint32_t)index);
    appendU32(frame, 0);                                              // (originX<<16)|originY
    appendU32(frame, ((uint32_t)kDisplayWidth << 16) | kDisplayHeight); // (width<<16)|height
    appendU32(frame, payloadSize);

    // WriteWindowRequest (opcode 0x0084) + big-endian geometry.
    uint8_t ww[8] = { 0x84, 0x00, (uint8_t)index, kDisplayDataFormat, 0, 0, 0, 0 };
    frame.insert(frame.end(), ww, ww + sizeof(ww));
    appendBE16(frame, 0);               // left
    appendBE16(frame, 0);               // top
    appendBE16(frame, kDisplayWidth);
    appendBE16(frame, kDisplayHeight);

    // The panel samples RGB565 BIG-endian on the wire (verified live: native
    // 0xF800 "red" showed as blue — a clean byte-swap). Callers pass native
    // little-endian pixels; swap here.
    for (uint16_t px : rgb565)
    {
        frame.push_back((uint8_t)(px >> 8));
        frame.push_back((uint8_t)px);
    }

    // EndOfUpdateRequest (opcode 0x0040).
    uint8_t eou[4] = { 0x40, 0x00, (uint8_t)index, 0x00 };
    frame.insert(frame.end(), eou, eou + sizeof(eou));

    controller_->request->post(frame);   // fire-and-forget
}

void StudioController::fillDisplay(int index, uint16_t rgb565)
{
    std::vector<uint16_t> px((size_t)kDisplayWidth * kDisplayHeight, rgb565);
    drawDisplay(index, px);
}

// --- LEDs -------------------------------------------------------------------

void StudioController::setLed(size_t channel, uint8_t level)
{
    if (channel < kLedChannelCount)
        ledState_[channel] = level;
}

void StudioController::setAllLeds(uint8_t level)
{
    ledState_.fill(level);
}

void StudioController::setPadLed(size_t pad, uint8_t r, uint8_t g, uint8_t b)
{
    if (pad >= (size_t)kPadCount)
        return;
    // Pads split into two RGB runs: 1–8 and 9–16 (live-mapped).
    size_t base = (pad < 8) ? kPadLedBase[0] + pad * 3
                            : kPadLedBase[1] + (pad - 8) * 3;
    setLed(base + 0, clampLevel(r));
    setLed(base + 1, clampLevel(g));
    setLed(base + 2, clampLevel(b));
}

void StudioController::setGroupLed(size_t group, uint8_t r, uint8_t g, uint8_t b)
{
    if (group >= (size_t)kGroupCount)
        return;
    size_t base = kGroupLedBase + group * 3;
    setLed(base + 0, clampLevel(r));
    setLed(base + 1, clampLevel(g));
    setLed(base + 2, clampLevel(b));
}

void StudioController::setVU(bool left, size_t segments)
{
    size_t base = left ? kVULeftBase : kVURightBase;
    for (int i = 0; i < kVUSegments; i++)
        setLed(base + i, ((size_t)i < segments) ? 127 : 0);
}

void StudioController::setVUBlue(bool blue)
{
    setLed(kVUColorChannel, blue ? 0 : 127);
}

void StudioController::setJogRingSegment(size_t segment, uint8_t level)
{
    if (segment < (size_t)kJogRingSegments)
        setLed(kJogRingBase + segment, clampLevel(level));
}

void StudioController::flushLeds()
{
    if (controller_ == nullptr)
        return;

    Frame frame;
    frame.reserve(8 + ledState_.size());
    appendU32(frame, conn_->messageIdFor(kTagSetLedState));
    appendU32(frame, (uint32_t)ledState_.size());   // count=213 (M2 sends this; without it the
                                                    // buffer lands shifted −4 and truncates the last 4 channels)
    frame.insert(frame.end(), ledState_.begin(), ledState_.end());

    static const bool debug = getenv("MACCHINA_LED_DEBUG") != nullptr;
    if (debug)
    {
        size_t nonzero = 0; long first = -1;
        for (size_t i = 0; i < ledState_.size(); i++)
            if (ledState_[i]) { nonzero++; if (first < 0) first = (long)i; }
        fprintf(stderr, "flushLeds: %zu-byte frame, %zu nonzero channels, first lit = %ld\n",
                frame.size(), nonzero, first);
    }

    controller_->request->post(frame);   // fire-and-forget
}

// --- Inbound events ---------------------------------------------------------

void StudioController::handleEventFrame(const Frame & frame)
{
    // MACCHINA_EVT_DEBUG=1 hexdumps every raw event frame — for checking the
    // decode below against what the wire actually says.
    static const bool debug = getenv("MACCHINA_EVT_DEBUG") != nullptr;
    if (debug)
        fprintf(stderr, "evt %s", describeFrame(frame).c_str());

    // Common header: [u32 msgId][u32 seq][u32 tsNanos][u32 count] then records.
    if (frame.size() < 16 || !onInput)
        return;

    uint32_t msgId = *nhl2::messageId(frame);
    uint32_t count = *readU32(frame, 12);

    const uint8_t * rec   = frame.data() + 16;
    size_t          avail = frame.size() - 16;

    switch (msgId & 0xffffff)
    {
        case kEvtPads:
            for (uint32_t i = 0; i < count && avail >= 12; i++, rec += 12, avail -= 12)
            {
                InputEvent e;
                e.type     = InputEvent::Type::Pad;
                e.id       = readU32At(rec + 0);
                e.padState = readU32At(rec + 4);
                e.pressure = readF32(rec + 8);
                onInput(e);
            }
            break;

        case kEvtSwitches:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                InputEvent e;
                e.type = InputEvent::Type::Button;
                e.id   = readU32At(rec + 0);
                // The state word is NOT the f32 the old docs claimed (that
                // read every release as down): live capture 2026-07 shows
                // press 0x3f800b01 / release 0x3f800b00 — bit 0 is the button,
                // the boh upper bytes look like f32 1.0 + unknown 0x0b.
                e.down = (readU32At(rec + 4) & 1) != 0;
                onInput(e);
            }
            break;

        case kEvtKnobs:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                InputEvent e;
                e.type  = InputEvent::Type::Knob;
                e.id    = readU32At(rec + 0);
                e.delta = readF32(rec + 4);
                onInput(e);
            }
            break;

        case kEvtWheel:
            for (uint32_t i = 0; i < count && avail >= 8; i++, rec += 8, avail -= 8)
            {
                InputEvent e;
                e.type = InputEvent::Type::Wheel;
                e.id   = readU32At(rec + 0);
                e.step = (int32_t)readU32At(rec + 4);
                onInput(e);
            }
            break;

        default:
            break;
    }
}

} // namespace macchina::studio
