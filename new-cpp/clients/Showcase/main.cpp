//
//  main.cpp
//  Macchina — new-cpp/clients/Showcase
//
//  The integral demo: everything this tree can do, in one client.
//    - screens as live UI (gfx/Surface): last event in big type + history on
//      the left; knob bars, 4×4 pad grid, wheel counter on the right
//    - LEDs as feedback (full StudioLayout map): pads flash with pressure,
//      held buttons light, groups are a rainbow radio selector, the VU
//      meters follow knobs 1–2, the jog ring follows the wheel
//    - input decode + the completed name table drive it all
//
//  Usage:   Showcase [serial]        (default serial 39195855)
//  Env:     MACCHINA_RUN_SECS=N     exit (blanked) after N seconds
//  Ctrl-C blanks LEDs + screens and exits.
//
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "gfx/Surface.hpp"
#include "studio/StudioController.hpp"
#include "studio/StudioLayout.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <string>

using namespace macchina;

namespace {

volatile sig_atomic_t gStopRequested = 0;
void onSigint(int) { gStopRequested = 1; }

void hueToRGB(double hue01, uint8_t & r, uint8_t & g, uint8_t & b)
{
    double  h = hue01 * 6.0;
    int     s = (int)h;
    double  f = h - s;
    uint8_t v = 127, p = 0, q = (uint8_t)(127 * (1 - f)), t = (uint8_t)(127 * f);
    switch (s % 6)
    {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

struct State
{
    std::string             lastEvent = "TOUCH ME!";
    std::deque<std::string> history;              // most recent first, capped
    float                   knob[8]      = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    bool                    knobTouch[8] = {};
    float                   master       = 0.5f;   // the big level encoder (knob id 8)
    int                     wheelPos     = 0;
    float                   padLevel[16] = {};    // current pressure per pad
    int                     selectedGroup = 0;    // radio selector A–H
    std::set<size_t>        litButtons;           // LED channels of held buttons
    bool                    dirty = true;

    void event(std::string text)
    {
        lastEvent = std::move(text);
        history.push_front(lastEvent);
        if (history.size() > 7)
            history.pop_back();
        dirty = true;
    }
};

char lineBuf[64];

void handleInput(State & st, const InputEvent & e)
{
    switch (e.type)
    {
        case InputEvent::Type::Pad:
        {
            int pad = studio::padIndexForInputId(e.id);
            if (pad < 0)
                break;
            // Threshold out sensor noise: some pads (hi pad 4) trickle tiny
            // pressure values while untouched, which a brightness floor would
            // otherwise turn into a permanently lit pad.
            st.padLevel[pad] = (e.padState == studio::kPadRelease || e.pressure < 0.04f)
                                   ? 0.0f : e.pressure;
            if (e.padState == studio::kPadHit)
            {
                snprintf(lineBuf, sizeof(lineBuf), "PAD %d HIT %.2f", pad + 1, e.pressure);
                st.event(lineBuf);
            }
            else if (e.padState == studio::kPadRelease)
                st.dirty = true;
            else
                st.dirty = true;   // pressure: update bars/LEDs, skip history spam
            break;
        }
        case InputEvent::Type::Button:
        {
            // Knob touch caps get their own treatment (bar highlight).
            if (e.id >= 0x58 && e.id <= 0x5f)
            {
                st.knobTouch[e.id - 0x58] = e.down;
                st.dirty = true;
                break;
            }
            const auto * c = studio::controlForButton(e.id);
            if (e.down)
            {
                // Unknown ids show their hex — that's mapping data, report it!
                if (c)
                    snprintf(lineBuf, sizeof(lineBuf), "%s DOWN [0x%x]", c->name, c->buttonId);
                else
                    snprintf(lineBuf, sizeof(lineBuf), "BTN 0X%02X DOWN (UNMAPPED)", e.id);
                st.event(lineBuf);
            }
            else
                st.dirty = true;
            // Groups are a radio selector; other mapped buttons light while held.
            if (c && c->buttonId >= 0x10 && c->buttonId <= 0x17 && c->rgb)
            {
                if (e.down)
                    st.selectedGroup = (int)((c->ledChannel - (int)studio::kGroupLedBase) / 3);
            }
            else if(c && c->buttonId == 0x33)
            {
                // Light up all segments when the jogwheel is pressed.
                for(size_t i = studio::kJogRingBase; i < studio::kJogRingBase + studio::kJogRingSegments; i++) {
                    if(e.down) st.litButtons.insert(i);
                    else       st.litButtons.erase(i);
                }
            }
            else if (c && c->ledChannel >= 0)
            {
                int n = c->rgb ? 3 : 1;
                for (int i = 0; i < n; i++)
                {
                    if (e.down) st.litButtons.insert((size_t)c->ledChannel + i);
                    else        st.litButtons.erase((size_t)c->ledChannel + i);
                }
            }
            break;
        }
        case InputEvent::Type::Knob:
        {
            int k = (int)e.id;
            if (k == (int)studio::kMasterKnobId)
            {
                st.master = std::min(1.0f, std::max(0.0f, st.master + e.delta));
                snprintf(lineBuf, sizeof(lineBuf), "MASTER = %.2f", st.master);
            }
            else if (k >= 0 && k <= 7)
            {
                st.knob[k] = std::min(1.0f, std::max(0.0f, st.knob[k] + e.delta));
                snprintf(lineBuf, sizeof(lineBuf), "KNOB %d = %.2f", k + 1, st.knob[k]);
            }
            else
                snprintf(lineBuf, sizeof(lineBuf), "KNOB ID %d? = %+.3f (UNMAPPED)", k, e.delta);
            st.lastEvent = lineBuf;   // no history spam for continuous deltas
            st.dirty = true;
            break;
        }
        case InputEvent::Type::Wheel:
        {
            st.wheelPos += e.step;
            snprintf(lineBuf, sizeof(lineBuf), "WHEEL %+d  (POS %d)", e.step, st.wheelPos);
            st.lastEvent = lineBuf;
            st.dirty = true;
            break;
        }
    }
}

// --- Screens ----------------------------------------------------------------

const uint16_t kBlack  = gfx::rgb565(0, 0, 0);
const uint16_t kWhite  = gfx::rgb565(255, 255, 255);
const uint16_t kOrange = gfx::rgb565(255, 140, 0);
const uint16_t kGrey   = gfx::rgb565(120, 120, 120);
const uint16_t kDim    = gfx::rgb565(50, 50, 50);

void renderLeft(gfx::Surface & s, const State & st)
{
    s.clear(kBlack);
    s.drawText(8, 8, "MACCHINA SHOWCASE", kOrange, 2);
    s.fillRect(0, 30, s.width, 2, kOrange);

    // Last event, as big as fits.
    int scale = 4;
    while (scale > 1 && gfx::Surface::textWidth(st.lastEvent, scale) > s.width - 16)
        scale--;
    s.drawText(8, 52, st.lastEvent, kWhite, scale);

    // History.
    int y = 110;
    for (const auto & line : st.history)
    {
        s.drawText(8, y, line, y == 110 ? kGrey : kDim, 2);
        y += 22;
        if (y > s.height - 16)
            break;
    }
}

void renderRight(gfx::Surface & s, const State & st)
{
    s.clear(kBlack);

    // 8 knob bars across the top half.
    const int barW = 36, gap = 16, top = 24, barH = 90;   // leaves room for the MASTER bar
    for (int k = 0; k < 8; k++)
    {
        int x = 8 + k * (barW + gap);
        int fill = (int)(st.knob[k] * barH);
        s.fillRect(x, top, barW, barH, kDim);
        s.fillRect(x, top + barH - fill, barW, fill, st.knobTouch[k] ? kOrange : kWhite);
        snprintf(lineBuf, sizeof(lineBuf), "%d", k + 1);
        s.drawText(x + barW / 2 - 5, top + barH + 6, lineBuf, st.knobTouch[k] ? kOrange : kGrey, 2);
    }

    // 4×4 pad grid, physical orientation (13–16 on top, 1–4 at the bottom).
    const int cell = 26, pitch = 30, gx = 8, gy = 150;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
        {
            int pad = (3 - row) * 4 + col;   // top row = pads 13–16
            float lvl = st.padLevel[pad];
            uint16_t c = lvl > 0
                ? gfx::rgb565((uint8_t)(60 + 195 * lvl), (uint8_t)(40 + 100 * lvl), 0)
                : kDim;
            s.fillRect(gx + col * pitch, gy + row * pitch, cell, cell, c);
        }
    s.drawText(gx, gy + 4 * pitch + 4, "PADS", kGrey, 2);

    // Wheel + group + master readouts.
    snprintf(lineBuf, sizeof(lineBuf), "WHEEL %d", st.wheelPos);
    s.drawText(160, 170, lineBuf, kWhite, 3);
    snprintf(lineBuf, sizeof(lineBuf), "GROUP %c", 'A' + st.selectedGroup);
    s.drawText(160, 210, lineBuf, kOrange, 3);

    // Master level: vertical bar on the far right, mirroring the VU meters.
    const int mW = 24, mH = 200, mX = s.width - mW - 12, mY = 24;
    int fill = (int)(st.master * mH);
    s.fillRect(mX, mY, mW, mH, kDim);
    s.fillRect(mX, mY + mH - fill, mW, fill, kOrange);
    s.drawText(mX - 5, mY + mH + 10, "MST", kGrey, 2);
}

// --- LEDs -------------------------------------------------------------------

void renderLeds(studio::StudioController & studio, const State & st)
{
    studio.setAllLeds(0);

    // Pads: hue by index, brightness by pressure.
    for (int p = 0; p < studio::kPadCount; p++)
        if (st.padLevel[p] > 0)
        {
            uint8_t r, g, b;
            hueToRGB(p / 16.0, r, g, b);
            float lvl = 0.25f + 0.75f * st.padLevel[p];
            studio.setPadLed((size_t)p, (uint8_t)(r * lvl), (uint8_t)(g * lvl), (uint8_t)(b * lvl));
        }

    // Groups: rainbow radio — selected bright, others dim.
    for (int g = 0; g < studio::kGroupCount; g++)
    {
        uint8_t r, gg, b;
        hueToRGB(g / 8.0, r, gg, b);
        int div = (g == st.selectedGroup) ? 1 : 10;
        studio.setGroupLed((size_t)g, (uint8_t)(r / div), (uint8_t)(gg / div), (uint8_t)(b / div));
    }

    // Held buttons.
    for (size_t ch : st.litButtons)
        studio.setLed(ch, 127);

    // VU meters follow knobs 1–2; jog ring follows the wheel.
    // The master encoder drives the hardware VU meters — it IS the level knob.
    studio.setVU(true,  (size_t)lround(st.master * studio::kVUSegments));
    studio.setVU(false, (size_t)lround(st.master * studio::kVUSegments));
    int n   = studio::kJogRingSegments;
    int seg = ((st.wheelPos % n) + n) % n;
    studio.setJogRingSegment((size_t)seg, 127);

    studio.flushLeds();
}

} // namespace


int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : "39195855";

    CFMessagePortTransport   transport;
    studio::StudioController studio(transport, serial);
    State                    st;
    studio.onInput = [&st](const InputEvent & e) { handleInput(st, e); };

    fprintf(stderr, "connecting to Maschine Studio (serial %s)…\n", serial.c_str());
    if (!studio.connect())
    {
        fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
        return 1;
    }
    fprintf(stderr, "connected. Play with EVERYTHING — pads, buttons, knobs, the wheel. Ctrl-C quits.\n");

    signal(SIGINT, onSigint);

    gfx::Surface left(studio.displayWidth(), studio.displayHeight());
    gfx::Surface right(studio.displayWidth(), studio.displayHeight());

    long runSecs = getenv("MACCHINA_RUN_SECS") ? strtol(getenv("MACCHINA_RUN_SECS"), nullptr, 10) : 0;
    auto start   = std::chrono::steady_clock::now();
    auto lastDraw = start - std::chrono::hours(1);

    while (!gStopRequested)
    {
        transport.runFor(1.0 / 60);

        auto now = std::chrono::steady_clock::now();
        if (runSecs > 0 && std::chrono::duration<double>(now - start).count() >= (double)runSecs)
            break;

        if (!st.dirty)
            continue;

        renderLeds(studio, st);   // LEDs are cheap — update on every change

        // Screens are ~256 KiB per push; cap redraws at ~12 fps.
        if (std::chrono::duration<double>(now - lastDraw).count() >= 1.0 / 60)
        {
            renderLeft(left, st);
            renderRight(right, st);
            studio.drawDisplay(0, left.pixels);
            studio.drawDisplay(1, right.pixels);
            lastDraw = now;
            st.dirty = false;
        }
    }

    // Blank everything on the way out.
    studio.setAllLeds(0);
    studio.flushLeds();
    for (int i = 0; i < studio.displayCount(); i++)
        studio.fillDisplay(i, kBlack);
    fprintf(stderr, "\nblanked, bye.\n");
    return 0;
}
