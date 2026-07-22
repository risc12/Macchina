//
//  Controller.hpp
//  Macchina — new-cpp/core
//
//  The device-agnostic controller interface — the boundary the future daemon
//  programs against. A Controller is "a thing with displays, flat LED
//  channels, and input events"; everything device-specific (channel maps,
//  pixel formats, product ids) lives behind a concrete implementation
//  (studio/StudioController). Nothing here may mention a channel number.
//
//  Device-specific conveniences (pad/group RGB helpers, VU ladders, …) are
//  NOT part of this interface — they live on the concrete class, and a
//  front-end that wants them addresses the device by raw channel.
//

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace macchina {

/// One input gesture, already decoded from the wire. A tagged struct rather
/// than a variant so it maps onto any language's plainest record type.
struct InputEvent
{
    enum class Type : uint8_t { Pad, Button, Knob, Wheel };

    Type     type;
    uint32_t id = 0;         // padId / buttonId / knobId / wheelId

    // Pad — state is the raw wire value (Studio: 1 hit, 4 pressure, 3 release;
    // named constants on the concrete controller), pressure 0.0–1.0.
    uint32_t padState = 0;
    float    pressure = 0;

    // Button (also touch-sensitive knobs' touch sensors).
    bool     down = false;

    // Knob — fine continuous delta, no detents.
    float    delta = 0;

    // Wheel — whole detent steps (+1 / -1 per notch).
    int32_t  step = 0;
};


class Controller
{
public:
    virtual ~Controller() = default;

    /// Full connect (+ whatever claim/focus the device needs). false if the
    /// backing service isn't running or the device refused.
    virtual bool connect() = 0;
    virtual bool isConnected() const = 0;

    // --- Displays ---------------------------------------------------------
    virtual int      displayCount() const = 0;
    virtual uint16_t displayWidth() const = 0;
    virtual uint16_t displayHeight() const = 0;

    /// Push a full frame of native little-endian RGB565 pixels
    /// (width*height of them) to a display.
    virtual void drawDisplay(int index, const std::vector<uint16_t> & rgb565) = 0;

    /// Fill a whole display with one RGB565 colour.
    virtual void fillDisplay(int index, uint16_t rgb565) = 0;

    // --- LEDs -------------------------------------------------------------
    // A flat run of intensity channels; the concrete controller documents its
    // map. Set* edits a local buffer; flushLeds() sends it.

    virtual size_t ledChannelCount() const = 0;
    virtual void   setLed(size_t channel, uint8_t level) = 0;
    virtual void   setAllLeds(uint8_t level) = 0;
    virtual void   flushLeds() = 0;

    // --- Input ------------------------------------------------------------
    /// Fires on the transport's event-loop thread while it runs.
    std::function<void(const InputEvent &)> onInput;
};

} // namespace macchina
