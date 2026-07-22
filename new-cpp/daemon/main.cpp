//
//  main.cpp
//  Macchina — new-cpp/daemon (macchinad)
//
//  The daemon: owns the Studio (exclusively — one client per device!) and
//  exposes it over OSC/UDP so front-ends stay simple socket clients (the
//  Ableton Remote Script, a CLI, Max for Live, …).
//
//  It talks to the device only through the Controller interface + the
//  StudioLayout vocabulary; the OSC address space mirrors that vocabulary.
//
//  Transport: UDP on 127.0.0.1:7000 (MACCHINA_OSC_PORT to change). A client
//  sends /macchina/subscribe once; the daemon replies /macchina/hello and
//  from then on pushes /event/* messages to that client's source address.
//  Last subscriber wins (one front-end at a time, like the device itself).
//
//  Commands (→ daemon):
//    /macchina/subscribe            claim the event stream (reply: /macchina/hello)
//    /macchina/ping                 reply /macchina/pong (liveness)
//    /led        i chan  i level    one LED channel, 0–127 (auto-flushed)
//    /led/name   s name  i level    by StudioLayout m2 name ("transport_play");
//                                   RGB controls set all three channels
//    /all        i level            every channel
//    /pad/rgb    i pad1-16 i r i g i b
//    /group/rgb  i grpA=1..8 i r i g i b
//    /vu         f left f right     0.0–1.0 per meter
//    /vu/color   i 0|1              1 = blue (input-monitor look)
//    /ring       i seg1-15 i level
//    /ring/pos   i pos              light exactly one segment (pos mod 15)
//    /display/clear i disp [i rgb565]
//    /display/text  i disp i x i y i scale i rgb565 s text
//    /display/rect  i disp i x i y i w i h i rgb565
//    /display/flush i disp          push the staged surface to the device
//
//  Events (daemon → subscriber):
//    /event/button s name i down    all switches (incl. "wheel" = jog push)
//    /event/pad    i pad1-16 i state f pressure     (state: 1 hit, 4 press, 3 release)
//    /event/knob   i knob1-8 f delta
//    /event/master f delta
//    /event/wheel  i step
//    /event/cap    i knob1-8 i touching
//
//  Ctrl-C blanks the device and exits.
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "gfx/Surface.hpp"
#include "daemon/osc/Osc.hpp"
#include "studio/StudioController.hpp"
#include "studio/StudioLayout.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <csignal>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace macchina;

namespace {

volatile sig_atomic_t gStopRequested = 0;
void onSigint(int) { gStopRequested = 1; }

std::string lowercased(std::string s)
{
    for (auto & c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

const studio::ControlInfo * controlByM2Name(const std::string & lowerName)
{
    for (const auto & c : studio::controls())
        if (lowercased(c.m2Name) == lowerName)
            return &c;
    return nullptr;
}


class Daemon
{
public:
    Daemon(studio::StudioController & studio, int fd)
        : studio_(studio), fd_(fd),
          left_(studio.displayWidth(), studio.displayHeight()),
          right_(studio.displayWidth(), studio.displayHeight()),
          page0_(studio.displayWidth(), studio.displayHeight()),
          page1_(studio.displayWidth(), studio.displayHeight())
    {
    }

    // --- events: device → subscriber ------------------------------------

    void onInput(const InputEvent & e)
    {
        using osc::Value;
        switch (e.type)
        {
            case InputEvent::Type::Pad:
            {
                int pad = studio::padIndexForInputId(e.id);
                if (pad >= 0)
                    send(osc::makeMessage("/event/pad",
                        {Value::ofInt(pad + 1), Value::ofInt((int32_t)e.padState),
                         Value::ofFloat(e.pressure)}));
                break;
            }
            case InputEvent::Type::Button:
            {
                if (e.id >= 0x58 && e.id <= 0x5f)   // knob touch caps
                {
                    send(osc::makeMessage("/event/cap",
                        {Value::ofInt((int32_t)(e.id - 0x58 + 1)), Value::ofInt(e.down)}));
                    break;
                }
                const auto * c = studio::controlForButton(e.id);
                // MIX / CHANNEL / PLUG-IN drive the page system — consumed
                // here, never forwarded. Pressing the active page's button
                // returns Home.
                if (c && (c->buttonId == 0x01 || c->buttonId == 0x07 || c->buttonId == 0x00))
                {
                    Mode page = c->buttonId == 0x01 ? Mode::Mix
                              : c->buttonId == 0x07 ? Mode::Chan : Mode::Plug;
                    if (e.down && mode_ != Mode::Waiting)
                        setMode(mode_ == page ? Mode::Home : page);
                    break;
                }
                // Mix-page gestures (Maschine style): hold MUTE or SOLO, then
                // press a screen button to toggle that strip's track.
                if (mode_ == Mode::Mix && c)
                {
                    if (c->buttonId == 0x24 || c->buttonId == 0x25)   // Mute / Solo
                    {
                        (c->buttonId == 0x24 ? muteHeld_ : soloHeld_) = e.down;
                        studio_.setLed((size_t)c->ledChannel, e.down ? 127 : 8);
                        updateGestureLeds();
                        break;   // consumed
                    }
                    if (c->ledChannel >= 32 && c->ledChannel <= 39)
                    {
                        int slot = c->ledChannel - 32;
                        if (muteHeld_ || soloHeld_)
                        {
                            if (e.down)
                                send(osc::makeMessage(
                                    muteHeld_ ? "/live/track/mute" : "/live/track/solo",
                                    {Value::ofInt(mixOffset_ + slot)}));
                        }
                        else if (tracks_[slot].isGroup && e.down)
                            send(osc::makeMessage("/live/track/fold",
                                                  {Value::ofInt(mixOffset_ + slot)}));
                        break;   // consumed either way on the mix page
                    }
                }
                // Channel page: screen buttons select a device in the chain.
                if (mode_ == Mode::Chan && c && c->ledChannel >= 32 && c->ledChannel <= 39)
                {
                    int slot = c->ledChannel - 32;
                    if (e.down && chan_.dev[slot].present)
                        send(osc::makeMessage("/live/chan/device", {Value::ofInt(slot)}));
                    break;
                }
                // Plug-in page + EQ8: screen buttons toggle bands on/off,
                // arrows cycle what the knobs edit (gain / freq / Q).
                if (mode_ == Mode::Plug && plug_.isEq8 && c)
                {
                    if (c->ledChannel >= 32 && c->ledChannel <= 39)
                    {
                        if (e.down)
                            send(osc::makeMessage("/live/plug/eq8_on",
                                                  {Value::ofInt(c->ledChannel - 32)}));
                        break;
                    }
                    if (c->buttonId == 0x04 || c->buttonId == 0x03)   // ← / →
                    {
                        if (e.down)
                        {
                            eq8Field_  = (eq8Field_ + (c->buttonId == 0x03 ? 1 : 2)) % 3;
                            plugDirty_ = true;
                        }
                        break;
                    }
                }
                std::string  name = c ? lowercased(c->m2Name) : "unknown_" + std::to_string(e.id);
                send(osc::makeMessage("/event/button",
                    {Value::ofString(std::move(name)), Value::ofInt(e.down)}));
                break;
            }
            case InputEvent::Type::Knob:
                if (e.id == studio::kMasterKnobId)
                {
                    // Local echo: move the HUD's volume bar NOW (60 fps),
                    // don't wait for Live's ~10 Hz round trip. Live's next
                    // /hud/state reconciles (same math → converges).
                    if (hud_.active)
                    {
                        hud_.vol    = std::min(1.f, std::max(0.f, hud_.vol + e.delta));
                        hudEchoAt_  = clock_;
                        hudDirty_   = true;
                    }
                    send(osc::makeMessage("/event/master", {Value::ofFloat(e.delta)}));
                }
                else if (mode_ == Mode::Home && e.id <= 1)
                {
                    // Home-page tuning: knob 1 = colour gamma, knob 2 = saturation.
                    double & p = (e.id == 0) ? gamma_ : sat_;
                    p = std::min(3.0, std::max(0.5, p + e.delta));
                    hudDirty_ = true;   // readout lives on the HUD
                }
                else if (mode_ == Mode::Plug && e.id < 8)
                {
                    // Local echo on the plug-in page: param bars / EQ gains
                    // follow the knob before Live's round trip.
                    if (plug_.isEq8)
                    {
                        // Echo the field the arrows selected, and send a
                        // semantic nudge — the script needn't know the mode.
                        auto & b = plug_.bands[e.id];
                        float & f = eq8Field_ == 0 ? b.gainN
                                  : eq8Field_ == 1 ? b.freqN : b.qN;
                        f        = std::min(1.f, std::max(0.f, f + e.delta));
                        b.echoAt = clock_;
                        static const char * kFields[3] = {"gain", "freq", "q"};
                        send(osc::makeMessage("/live/plug/eq8_nudge",
                            {Value::ofInt((int32_t)e.id + 1),
                             Value::ofString(kFields[eq8Field_]),
                             Value::ofFloat(e.delta)}));
                    }
                    else
                    {
                        if (plug_.params[e.id].present)
                        {
                            auto & p = plug_.params[e.id];
                            p.norm   = std::min(1.f, std::max(0.f, p.norm + e.delta));
                            p.echoAt = clock_;
                        }
                        send(osc::makeMessage("/event/knob",
                            {Value::ofInt((int32_t)e.id + 1), Value::ofFloat(e.delta)}));
                    }
                    plugDirty_ = true;
                }
                else
                {
                    // Mix page: echo the fader locally before Live's round trip.
                    if (mode_ == Mode::Mix && e.id < 8 && tracks_[e.id].present)
                    {
                        auto & t  = tracks_[e.id];
                        t.vol     = std::min(1.f, std::max(0.f, t.vol + e.delta));
                        t.echoAt  = clock_;
                        mixDirty_ = true;
                    }
                    send(osc::makeMessage("/event/knob",
                        {Value::ofInt((int32_t)e.id + 1), Value::ofFloat(e.delta)}));
                }
                break;
            case InputEvent::Type::Wheel:
                send(osc::makeMessage("/event/wheel", {Value::ofInt(e.step)}));
                break;
        }
    }

    // --- commands: subscriber → device ----------------------------------

    void handlePacket(const uint8_t * data, size_t length, const sockaddr_in & from)
    {
        auto msg = osc::Message::parse(data, length);
        if (!msg)
        {
            fprintf(stderr, "macchinad: unparseable %zu-byte packet dropped\n", length);
            return;
        }
        const auto & a = msg->address;

        if (a == "/macchina/subscribe")
        {
            // Clients re-subscribe periodically (keep-alive across daemon
            // restarts), so this must be idempotent: the full reset only runs
            // for a NEW subscriber. A new Live set = new script = new socket
            // = new port, so set switches still reset.
            bool isNew = !hasSubscriber_
                      || from.sin_addr.s_addr != subscriber_.sin_addr.s_addr
                      || from.sin_port != subscriber_.sin_port;
            subscriber_    = from;
            hasSubscriber_ = true;
            send(osc::makeMessage("/macchina/hello", {osc::Value::ofString("macchinad 1")}));
            if (!isNew)
                return;
            fprintf(stderr, "macchinad: subscriber %s:%d\n",
                    inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            for (auto & t : tracks_)
                t = TrackState{};
            studio_.setAllLeds(0);   // also retires the waiting spinner
            studio_.setVUBlue(false);   // LED_LEVEL_COLOR defaults to blue
                                        // (input-monitor look); we want normal
            studio_.flushLeds();
            setMode(Mode::Home);
        }
        else if (a == "/macchina/ping")
            send(osc::makeMessage("/macchina/pong", {}));
        else if (a == "/led")
        {
            auto chan = msg->intAt(0);
            auto lvl  = msg->intAt(1);
            if (chan && lvl)
            {
                studio_.setLed((size_t)*chan, (uint8_t)std::min(*lvl, 127));
                studio_.flushLeds();
            }
        }
        else if (a == "/led/name")
        {
            auto name = msg->stringAt(0);
            auto lvl  = msg->intAt(1);
            const auto * c = name ? controlByM2Name(lowercased(*name)) : nullptr;
            if (c && lvl && c->ledChannel >= 0)
            {
                for (int i = 0; i < (c->rgb ? 3 : 1); i++)
                    studio_.setLed((size_t)c->ledChannel + i, (uint8_t)std::min(*lvl, 127));
                studio_.flushLeds();
            }
        }
        else if (a == "/all")
        {
            if (auto lvl = msg->intAt(0))
            {
                studio_.setAllLeds((uint8_t)std::min(*lvl, 127));
                studio_.flushLeds();
            }
        }
        else if (a == "/pad/rgb" || a == "/group/rgb")
        {
            auto idx = msg->intAt(0);
            auto r = msg->intAt(1), g = msg->intAt(2), b = msg->intAt(3);
            if (idx && r && g && b)
            {
                if (a == "/pad/rgb")
                    studio_.setPadLed((size_t)(*idx - 1), (uint8_t)*r, (uint8_t)*g, (uint8_t)*b);
                else
                    studio_.setGroupLed((size_t)(*idx - 1), (uint8_t)*r, (uint8_t)*g, (uint8_t)*b);
                studio_.flushLeds();
            }
        }
        else if (a == "/vu")
        {
            // Sets the TARGET; tick() slews the LEDs there at the daemon's own
            // frame rate (meter ballistics) — front-ends like Live's Remote
            // Scripts can only send ~10 Hz, which looks steppy raw.
            auto l = msg->floatAt(0), r = msg->floatAt(1);
            if (l && r)
            {
                vuTarget_[0] = std::min(std::max(*l, 0.f), 1.f);
                vuTarget_[1] = std::min(std::max(*r, 0.f), 1.f);
            }
        }
        else if (a == "/mix/track")
        {
            // i idx(1-8), s name, f volume 0-1, f meter 0-1 — streamed by the
            // front-end; the daemon renders and animates.
            auto idx = msg->intAt(0);
            auto name = msg->stringAt(1);
            auto vol = msg->floatAt(2);
            auto meter = msg->floatAt(3);
            if (idx && *idx >= 1 && *idx <= 8 && name && vol && meter)
            {
                auto & t  = tracks_[*idx - 1];
                t.present = true;
                t.meterT  = std::min(std::max(*meter, 0.f), 1.f);
                float v    = std::min(std::max(*vol, 0.f), 1.f);
                bool    mute  = msg->intAt(4).value_or(0) != 0;
                bool    solo  = msg->intAt(5).value_or(0) != 0;
                int32_t color = msg->intAt(6).value_or(-1);   // 0xRRGGBB, -1 = none
                bool    group = msg->intAt(7).value_or(0) != 0;
                bool    fold  = msg->intAt(8).value_or(0) != 0;
                int32_t depth = msg->intAt(9).value_or(0);
                bool    changed = t.name != *name || t.muted != mute || t.solo != solo
                               || t.color != color || t.isGroup != group
                               || t.folded != fold || t.depth != depth;
                t.name    = *name;
                t.muted   = mute;
                t.solo    = solo;
                t.color   = color;
                t.isGroup = group;
                t.folded  = fold;
                t.depth   = depth;
                // While our local echo is fresh, the knob knows better than
                // this (up to ~150 ms stale) confirmation — skipping it kills
                // the rubber-band snap while turning.
                if (clock_ - t.echoAt > 0.4 && std::abs(t.vol - v) > 0.004f)
                {
                    t.vol   = v;
                    changed = true;
                }
                if (changed && mode_ == Mode::Mix)
                {
                    mixDirty_ = true;
                    updateGestureLeds();   // hold state row OR group-button hints
                }
            }
        }
        else if (a == "/mix/view")
        {
            auto offset = msg->intAt(0);
            auto total  = msg->intAt(1);
            if (offset && total && (*offset != mixOffset_ || *total != mixTotal_))
            {
                mixOffset_ = *offset;
                mixTotal_  = *total;
                if (mode_ == Mode::Mix)
                    mixDirty_ = true;
            }
        }
        else if (a == "/chan/state")
        {
            auto name = msg->stringAt(0);
            auto color = msg->intAt(1);
            auto count = msg->intAt(2);
            if (name && color && count)
            {
                if (chan_.track != *name || chan_.color != *color || chan_.count != *count)
                {
                    chan_.track = *name;
                    chan_.color = *color;
                    chan_.count = *count;
                    for (int i = *count; i < 8; i++)
                        chan_.dev[i] = ChanDevice{};
                    if (mode_ == Mode::Chan) { chanDirty_ = true; updatePageLeds(); }
                }
            }
        }
        else if (a == "/chan/device")
        {
            auto idx = msg->intAt(0);
            auto name = msg->stringAt(1);
            auto sel = msg->intAt(2);
            auto act = msg->intAt(3);
            auto eq8 = msg->intAt(4);
            if (idx && *idx >= 1 && *idx <= 8 && name && sel && act && eq8)
            {
                ChanDevice next{true, *name, *sel != 0, *act != 0, *eq8 != 0};
                auto & d = chan_.dev[*idx - 1];
                if (d.present != next.present || d.name != next.name
                    || d.selected != next.selected || d.active != next.active)
                {
                    d = next;
                    if (mode_ == Mode::Chan) { chanDirty_ = true; updatePageLeds(); }
                }
                else
                    d = next;
            }
        }
        else if (a == "/plug/state")
        {
            auto name = msg->stringAt(0);
            auto eq8 = msg->intAt(1);
            if (name && eq8)
            {
                if (plug_.device != *name || plug_.isEq8 != (*eq8 != 0))
                {
                    bool wasEq8  = plug_.isEq8;
                    plug_.device = *name;
                    plug_.isEq8  = *eq8 != 0;
                    eq8Field_    = 0;
                    for (auto & p : plug_.params)
                        p = PlugParam{};
                    if (wasEq8 && !plug_.isEq8)   // hand the arrows back to the script
                    {
                        studio_.setLed(46, 0);
                        studio_.setLed(47, 0);
                    }
                    if (mode_ == Mode::Plug) { plugDirty_ = true; updatePageLeds(); }
                }
            }
        }
        else if (a == "/plug/view")
        {
            auto offset = msg->intAt(0);
            auto total  = msg->intAt(1);
            if (offset && total && (*offset != plugOffset_ || *total != plugTotal_))
            {
                plugOffset_ = *offset;
                plugTotal_  = *total;
                if (mode_ == Mode::Plug)
                    plugDirty_ = true;
            }
        }
        else if (a == "/plug/param")
        {
            auto idx = msg->intAt(0);
            auto name = msg->stringAt(1);
            auto norm = msg->floatAt(2);
            auto disp = msg->stringAt(3);
            if (idx && *idx >= 1 && *idx <= 8 && name && norm && disp)
            {
                auto & p = plug_.params[*idx - 1];
                bool changed = !p.present || p.name != *name || p.disp != *disp;
                p.present = true;
                p.name    = *name;
                p.disp    = *disp;
                float v = std::min(std::max(*norm, 0.f), 1.f);
                if (clock_ - p.echoAt > 0.4 && std::abs(p.norm - v) > 0.004f)
                {
                    p.norm  = v;
                    changed = true;
                }
                if (changed && mode_ == Mode::Plug)
                    plugDirty_ = true;
            }
        }
        else if (a == "/plug/eq8")
        {
            auto band = msg->intAt(0);
            auto on = msg->intAt(1);
            auto freq = msg->floatAt(2);
            auto gain = msg->floatAt(3);
            auto q = msg->floatAt(4);
            auto type = msg->intAt(5);
            if (band && *band >= 1 && *band <= 8 && on && freq && gain && q && type)
            {
                auto & b = plug_.bands[*band - 1];
                bool changed = b.on != (*on != 0) || b.type != *type
                            || std::abs(b.freqN - *freq) > 0.003f
                            || std::abs(b.qN - *q) > 0.003f;
                b.on   = *on != 0;
                b.type = *type;
                if (clock_ - b.echoAt > 0.4)   // same echo guard for freq/Q
                {
                    b.freqN = *freq;
                    b.qN    = *q;
                }
                float g = std::min(std::max(*gain, 0.f), 1.f);
                if (clock_ - b.echoAt > 0.4 && std::abs(b.gainN - g) > 0.004f)
                {
                    b.gainN = g;
                    changed = true;
                }
                if (changed && mode_ == Mode::Plug)
                {
                    plugDirty_ = true;
                    updatePageLeds();
                }
            }
        }
        else if (a == "/hud/state")
        {
            // Semantic HUD: the front-end sends state (at whatever rate it
            // can), the daemon owns the pixels. Args: f tempo, i playing,
            // s position, f masterVol.
            auto tempo = msg->floatAt(0);
            auto playing = msg->intAt(1);
            auto pos = msg->stringAt(2);
            auto vol = msg->floatAt(3);
            if (tempo && playing && pos && vol)
            {
                HudState next{true, *tempo, *playing != 0, *pos,
                              std::min(1.f, std::max(0.f, *vol))};
                if (clock_ - hudEchoAt_ < 0.4)
                    next.vol = hud_.vol;   // our echo is fresher than this confirmation
                if (!hud_.active || next.tempo != hud_.tempo || next.playing != hud_.playing
                    || next.pos != hud_.pos || next.vol != hud_.vol)
                {
                    hud_      = next;
                    hudDirty_ = true;
                }
            }
        }
        else if (a == "/vu/color")
        {
            if (auto blue = msg->intAt(0))
            {
                studio_.setVUBlue(*blue != 0);
                studio_.flushLeds();
            }
        }
        else if (a == "/ring")
        {
            auto seg = msg->intAt(0);
            auto lvl = msg->intAt(1);
            if (seg && lvl)
            {
                studio_.setJogRingSegment((size_t)(*seg - 1), (uint8_t)std::min(*lvl, 127));
                studio_.flushLeds();
            }
        }
        else if (a == "/ring/pos")
        {
            if (auto pos = msg->intAt(0))
            {
                int n = studio::kJogRingSegments;
                for (int i = 0; i < n; i++)
                    studio_.setJogRingSegment((size_t)i, 0);
                studio_.setJogRingSegment((size_t)(((*pos % n) + n) % n), 127);
                studio_.flushLeds();
            }
        }
        else if (a == "/display/clear")
        {
            auto disp = msg->intAt(0);
            if (auto * s = surfaceFor(disp))
                s->clear((uint16_t)msg->intAt(1).value_or(0));
        }
        else if (a == "/display/text")
        {
            auto disp = msg->intAt(0);
            auto x = msg->intAt(1), y = msg->intAt(2), scale = msg->intAt(3), color = msg->intAt(4);
            auto text = msg->stringAt(5);
            if (auto * s = surfaceFor(disp); s && x && y && scale && color && text)
                s->drawText(*x, *y, *text, (uint16_t)*color, std::max(1, std::min(*scale, 8)));
        }
        else if (a == "/display/rect")
        {
            auto disp = msg->intAt(0);
            auto x = msg->intAt(1), y = msg->intAt(2), w = msg->intAt(3), h = msg->intAt(4),
                 color = msg->intAt(5);
            if (auto * s = surfaceFor(disp); s && x && y && w && h && color)
                s->fillRect(*x, *y, *w, *h, (uint16_t)*color);
        }
        else if (a == "/display/flush")
        {
            auto disp = msg->intAt(0);
            if (auto * s = surfaceFor(disp))
                studio_.drawDisplay(*disp, s->pixels);
        }
        else
            fprintf(stderr, "macchinad: unknown address %s\n", a.c_str());
    }

    /// Call once after the device is connected: enters Waiting mode.
    void begin(int port)
    {
        char buf[64];
        page1_.clear(0);
        page1_.drawText(8, 8, "MACCHINAD", 0xFC00, 3);
        page1_.fillRect(0, 40, page1_.width, 2, 0xFC00);
        snprintf(buf, sizeof(buf), "OSC ON UDP://127.0.0.1:%d", port);
        page1_.drawText(8, 60, buf, 0xFFFF, 2);
        page1_.drawText(8, 90, "START A FRONT-END TO TAKE OVER", 0x8410, 2);
        page1_.drawText(8, 114, "(ABLETON: CONTROL SURFACE 'MACCHINA')", 0x8410, 2);
        studio_.drawDisplay(1, page1_.pixels);
    }

    // Per-frame animation: waiting art, VU ballistics, HUD/mix redraws.
    void tick(double dt)
    {
        clock_ += dt;
        if (mode_ == Mode::Waiting)
        {
            animateWaiting(dt);
            return;
        }
        // VU: fast attack, slower decay — real-meter feel at the daemon's
        // frame rate regardless of how coarsely the front-end sends targets.
        bool vuChanged = false;
        for (int i = 0; i < 2; i++)
        {
            float delta = vuTarget_[i] - vuCurrent_[i];
            float slew  = (float)((delta > 0 ? 14.0 : 4.0) * dt);
            float step  = std::min(std::max(delta, -slew), slew);
            if (step != 0)
            {
                float next = vuCurrent_[i] + step;
                if (lround(next * studio::kVUSegments) != lround(vuCurrent_[i] * studio::kVUSegments))
                    vuChanged = true;
                vuCurrent_[i] = next;
            }
        }
        if (vuChanged)
        {
            studio_.setVU(true,  (size_t)lround(vuCurrent_[0] * studio::kVUSegments));
            studio_.setVU(false, (size_t)lround(vuCurrent_[1] * studio::kVUSegments));
            studio_.flushLeds();
        }

        // HUD: redraw when dirty, capped at ~30 fps.
        hudCooldown_ -= dt;
        if (mode_ == Mode::Home && hud_.active && hudDirty_ && hudCooldown_ <= 0)
        {
            hudDirty_    = false;
            hudCooldown_ = 1.0 / 30;
            renderHud();
        }

        // Mix page: slew the 8 track meters, redraw when anything moved.
        if (mode_ == Mode::Mix)
        {
            for (auto & t : tracks_)
            {
                float delta = t.meterT - t.meterC;
                float slew  = (float)((delta > 0 ? 14.0 : 4.0) * dt);
                float step  = std::min(std::max(delta, -slew), slew);
                if (step != 0)
                {
                    t.meterC += step;
                    if (std::abs(step) > 0.002f)
                        mixDirty_ = true;
                }
            }
            mixCooldown_ -= dt;
            if (mixDirty_ && mixCooldown_ <= 0)
            {
                mixDirty_    = false;
                mixCooldown_ = 1.0 / 30;
                renderMix();
            }
        }

        pageCooldown_ -= dt;
        if (pageCooldown_ <= 0)
        {
            if (mode_ == Mode::Chan && chanDirty_)
            {
                chanDirty_    = false;
                pageCooldown_ = 1.0 / 30;
                renderChannel();
            }
            else if (mode_ == Mode::Plug && plugDirty_)
            {
                plugDirty_    = false;
                pageCooldown_ = 1.0 / 30;
                renderPlugin();
            }
        }
    }

private:
    enum class Mode { Waiting, Home, Mix, Chan, Plug };

    struct ChanDevice
    {
        bool        present = false;
        std::string name;
        bool        selected = false, active = true, isEq8 = false;
    };
    struct ChanState
    {
        std::string track;
        int32_t     color = -1;
        int32_t     count = 0;
        ChanDevice  dev[8];
    };
    struct PlugParam
    {
        bool        present = false;
        std::string name, disp;
        float       norm   = 0;
        double      echoAt = -1000;
    };
    struct Eq8Band
    {
        bool    on = false;
        float   freqN = 0.5f, gainN = 0.5f, qN = 0.3f;
        int32_t type   = 3;
        double  echoAt = -1000;
    };
    struct PlugState
    {
        std::string device;
        bool        isEq8 = false;
        PlugParam   params[8];
        Eq8Band     bands[8];
    };

    void setMode(Mode m)
    {
        mode_     = m;
        muteHeld_ = soloHeld_ = false;
        studio_.setLed(43, (uint8_t)(m == Mode::Mix ? 127 : 8));    // MIX
        studio_.setLed(40, (uint8_t)(m == Mode::Chan ? 127 : 8));   // CHANNEL
        studio_.setLed(41, (uint8_t)(m == Mode::Plug ? 127 : 8));   // PLUG-IN
        studio_.setLed(157, (uint8_t)(m == Mode::Mix ? 8 : 0));     // Mute: gesture available
        studio_.setLed(156, (uint8_t)(m == Mode::Mix ? 8 : 0));     // Solo
        for (int i = 0; i < 8; i++)
            studio_.setLed((size_t)(32 + i), 0);                    // screen row: page-owned
        studio_.flushLeds();
        const char * name = m == Mode::Mix ? "mix" : m == Mode::Chan ? "chan"
                          : m == Mode::Plug ? "plug" : "home";
        send(osc::makeMessage("/page", {osc::Value::ofString(name)}));
        switch (m)
        {
            case Mode::Mix:  mixDirty_ = true; break;
            // Chan/Plug: wipe cached state so entering the page never flashes
            // the previous track/device — the script re-streams within a tick.
            case Mode::Chan: chan_ = ChanState{}; chanDirty_ = true; updatePageLeds(); break;
            case Mode::Plug: plug_ = PlugState{}; plugDirty_ = true; updatePageLeds(); break;
            default:
                hudDirty_ = true;
                // Restore whatever the client staged on the right screen (legend).
                studio_.drawDisplay(1, right_.pixels);
                break;
        }
    }

    // Screen-button row as page state: Channel = device slots (selected
    // bright), Plug-in + EQ8 = band on/off.
    void updatePageLeds()
    {
        for (int i = 0; i < 8; i++)
        {
            uint8_t level = 0;
            if (mode_ == Mode::Chan && chan_.dev[i].present)
                level = chan_.dev[i].selected ? 127 : 25;
            else if (mode_ == Mode::Plug && plug_.isEq8)
                level = plug_.bands[i].on ? 100 : 12;
            studio_.setLed((size_t)(32 + i), level);
        }
        if (mode_ == Mode::Plug && plug_.isEq8)   // arrows = field cycling
        {
            studio_.setLed(46, 60);
            studio_.setLed(47, 60);
        }
        studio_.flushLeds();
    }

    // The Studio's TFTs are narrow-gamut and wash out Live's vibrant colors;
    // push mids down (gamma) and saturation up to compensate. Live-tunable on
    // the Home page: knob 1 = gamma, knob 2 = saturation (readout on the HUD).
    // MACCHINA_GAMMA / MACCHINA_SAT set the startup values.
    void vibrance(uint8_t & r, uint8_t & g, uint8_t & b) const
    {
        double c[3] = {r / 255.0, g / 255.0, b / 255.0};
        for (auto & v : c)
            v = pow(v, gamma_);
        double lum = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        for (auto & v : c)
            v = std::min(1.0, std::max(0.0, lum + (v - lum) * sat_));
        r = (uint8_t)(c[0] * 255);
        g = (uint8_t)(c[1] * 255);
        b = (uint8_t)(c[2] * 255);
    }

    // Mix-page screen-button row. While MUTE/SOLO is held: dim = press to
    // toggle, bright = already muted/solo'd. Otherwise: dim over group strips
    // (press to fold/unfold).
    void updateGestureLeds()
    {
        bool held = muteHeld_ || soloHeld_;
        for (int i = 0; i < 8; i++)
        {
            const auto & t = tracks_[i];
            uint8_t level = 0;
            if (t.present)
            {
                if (held)
                    level = (muteHeld_ ? t.muted : t.solo) ? 127 : 30;
                else if (t.isGroup)
                    level = 20;
            }
            studio_.setLed((size_t)(32 + i), level);
        }
        studio_.flushLeds();
    }

    // --- the loading screen: a bouncing ball with a face -----------------

    static void fillCircle(gfx::Surface & s, int cx, int cy, int r, uint16_t color)
    {
        for (int dy = -r; dy <= r; dy++)
        {
            int half = (int)std::sqrt((double)(r * r - dy * dy));
            s.fillRect(cx - half, cy + dy, half * 2, 1, color);
        }
    }

    void animateWaiting(double dt)
    {
        // Physics the whole time, pixels at 30 fps.
        const int r = 16, floor = 262, top = 96, lw = 12, rw = 468;
        ballVY_ += 900.0 * dt;
        ballX_  += ballVX_ * dt;
        ballY_  += ballVY_ * dt;
        if (ballX_ < lw + r)  { ballX_ = lw + r;  ballVX_ = +std::abs(ballVX_); }
        if (ballX_ > rw - r)  { ballX_ = rw - r;  ballVX_ = -std::abs(ballVX_); }
        if (ballY_ > floor - r) { ballY_ = floor - r; ballVY_ = -560; }   // tireless
        if (ballY_ < top + r)   { ballY_ = top + r;   ballVY_ = +60; }

        // Jog-ring spinner, one revolution per ~1.5 s.
        spinPos_ += dt * (studio::kJogRingSegments / 1.5);
        int seg = (int)spinPos_ % studio::kJogRingSegments;
        if (seg != spinSeg_)
        {
            spinSeg_ = seg;
            for (int i = 0; i < studio::kJogRingSegments; i++)
                studio_.setJogRingSegment((size_t)i, i == seg ? 90 : 0);
            studio_.flushLeds();
        }

        animCooldown_ -= dt;
        if (animCooldown_ > 0)
            return;
        animCooldown_ = 1.0 / 30;

        trail_.push_front({(int)ballX_, (int)ballY_});
        if (trail_.size() > 10)
            trail_.pop_back();

        auto & s = page0_;
        s.clear(0);
        s.drawText((s.width - gfx::Surface::textWidth("M A C C H I N A", 3)) / 2, 20,
                   "M A C C H I N A", 0xFFFF, 3);
        s.drawText((s.width - gfx::Surface::textWidth("WAITING FOR A FRONT-END...", 2)) / 2, 56,
                   "WAITING FOR A FRONT-END...", 0x8410, 2);

        // Trail: shrinking, dimming echoes.
        int n = (int)trail_.size();
        for (int i = n - 1; i >= 1; i--)
        {
            int   shade = 40 - i * 4;
            fillCircle(s, trail_[(size_t)i].first, trail_[(size_t)i].second,
                       r - 4 - i, gfx::rgb565((uint8_t)(shade * 4), (uint8_t)(shade * 2), 0));
        }

        // The ball, with a face.
        int cx = (int)ballX_, cy = (int)ballY_;
        fillCircle(s, cx, cy, r, 0xFC00);
        s.fillRect(cx - 8, cy - 7, 4, 6, 0x0000);   // eyes
        s.fillRect(cx + 4, cy - 7, 4, 6, 0x0000);
        s.fillRect(cx - 9, cy + 2, 3, 4, 0x0000);   // an actual smile: ∪
        s.fillRect(cx - 6, cy + 5, 12, 3, 0x0000);
        s.fillRect(cx + 6, cy + 2, 3, 4, 0x0000);
        s.fillRect(0, floor, s.width, 2, 0x8410);   // the floor

        studio_.drawDisplay(0, s.pixels);
    }

    void renderMix()
    {
        // Styling cribbed from M2's own MixerPage stylesheet: near-black
        // panels, rgb(26,26,26) header bands, a palette-colored channel-number
        // chip, fader HANDLES on a thin track (not filled bars), dimmed-out
        // muted strips.
        const uint16_t kWhite  = 0xFFFF, kOrange = 0xFC00, kGrey = 0x8410,
                       kGreen  = 0x07E0, kRed = 0xF800;
        const uint16_t kHeader = gfx::rgb565(26, 26, 26);
        const uint16_t kTrack  = gfx::rgb565(20, 20, 20);
        const uint16_t kFillDim = gfx::rgb565(90, 50, 0);    // fader fill, subdued
        const uint16_t kSep    = gfx::rgb565(45, 45, 45);
        const uint16_t kBlue   = gfx::rgb565(40, 110, 255);
        char buf[16];

        // A solo anywhere effectively mutes every non-solo'd track.
        bool anySolo = false;
        for (const auto & t : tracks_)
            anySolo = anySolo || (t.present && t.solo);

        for (int screen = 0; screen < 2; screen++)
        {
            auto & s = (screen == 0) ? page0_ : page1_;
            s.clear(0);
            for (int col = 0; col < 4; col++)
            {
                const auto & t  = tracks_[screen * 4 + col];
                int          x0 = col * 120;
                if (col > 0)
                    s.fillRect(x0, 0, 1, s.height, kSep);
                if (!t.present)
                {
                    s.fillRect(x0 + 1, 0, 119, 26, kHeader);
                    s.drawText(x0 + 52, 130, "-", kGrey, 2);
                    continue;
                }

                // Silenced either literally or because something else is solo'd.
                bool silenced = t.muted || (anySolo && !t.solo);

                // Header band in Live's track color (dimmed when silenced);
                // name in black or white, whichever the color's luminance wants.
                uint16_t headerCol = kHeader, nameCol = silenced ? kGrey : kWhite;
                if (t.color >= 0)
                {
                    uint8_t r = (uint8_t)(t.color >> 16), g = (uint8_t)(t.color >> 8),
                            b = (uint8_t)t.color;
                    vibrance(r, g, b);
                    if (silenced) { r /= 3; g /= 3; b /= 3; }
                    headerCol = gfx::rgb565(r, g, b);
                    int lum = (299 * r + 587 * g + 114 * b) / 1000;
                    nameCol = lum > 110 ? 0x0000 : kWhite;
                }
                s.fillRect(x0 + 1, 0, 119, 26, headerCol);
                // Groups get a fold indicator; nested tracks indent by depth.
                int nx = x0 + 8 + std::min((int)t.depth, 3) * 8;
                if (t.isGroup)
                {
                    s.drawText(nx, 6, t.folded ? "+" : "-", nameCol, 2);
                    nx += 14;
                }
                s.drawText(nx, 6, t.name.substr(0, t.isGroup ? 8 : 9), nameCol, 2);

                // Fader: narrow track, subdued fill, bright handle line.
                const int barTop = 38, barH = 186;
                int       vh = (int)(barH * t.vol);
                s.fillRect(x0 + 26, barTop, 12, barH, kTrack);
                s.fillRect(x0 + 26, barTop + barH - vh, 12, vh, silenced ? kTrack : kFillDim);
                s.fillRect(x0 + 21, barTop + barH - vh - 2, 22, 5, silenced ? kGrey : kOrange);

                // Meter: wide green ladder, red cap when hot.
                int mh = (int)(barH * t.meterC);
                s.fillRect(x0 + 56, barTop, 30, barH, kTrack);
                s.fillRect(x0 + 56, barTop + barH - mh, 30, mh, silenced ? kGrey : kGreen);
                if (!silenced && t.meterC > 0.95f)
                    s.fillRect(x0 + 56, barTop, 30, 8, kRed);

                snprintf(buf, sizeof(buf), "%3d%%", (int)lround(t.vol * 100));
                s.drawText(x0 + 8, 238, buf, kGrey, 2);
                if (t.muted)
                {
                    s.fillRect(x0 + 88, 234, 26, 20, kRed);
                    s.drawText(x0 + 96, 237, "M", kWhite, 2);
                }
                else if (t.solo)
                {
                    s.fillRect(x0 + 88, 234, 26, 20, kBlue);
                    s.drawText(x0 + 96, 237, "S", kWhite, 2);
                }
            }

            // Scrollbar: ONE bar spanning both screens (virtual 960 px wide);
            // each panel draws its half.
            if (mixTotal_ > 8)
            {
                const int virtualW = 2 * s.width;
                int thumbW = std::max(48, virtualW * 8 / mixTotal_);
                int thumbX = (virtualW - thumbW) * mixOffset_ / std::max(1, mixTotal_ - 8);
                int off    = screen * s.width;
                s.fillRect(0, s.height - 3, s.width, 3, kTrack);
                s.fillRect(thumbX - off, s.height - 3, thumbW, 3, kOrange);   // clipped by fillRect
            }

            studio_.drawDisplay(screen, s.pixels);
        }
    }

    void renderChannel()
    {
        const uint16_t kWhite = 0xFFFF, kOrange = 0xFC00, kGrey = 0x8410,
                       kGreen = 0x07E0;
        const uint16_t kHeader = gfx::rgb565(26, 26, 26);
        const uint16_t kSep    = gfx::rgb565(45, 45, 45);
        char buf[24];

        for (int screen = 0; screen < 2; screen++)
        {
            auto & s = (screen == 0) ? page0_ : page1_;
            s.clear(0);

            // Header: track color band + name (left screen), device count (right).
            uint16_t headerCol = kHeader, nameCol = kWhite;
            if (chan_.color >= 0)
            {
                uint8_t r = (uint8_t)(chan_.color >> 16), g = (uint8_t)(chan_.color >> 8),
                        b = (uint8_t)chan_.color;
                vibrance(r, g, b);
                headerCol = gfx::rgb565(r, g, b);
                nameCol = (299 * r + 587 * g + 114 * b) / 1000 > 110 ? 0x0000 : kWhite;
            }
            s.fillRect(0, 0, s.width, 30, headerCol);
            if (screen == 0)
                s.drawText(8, 8, chan_.track.substr(0, 20), nameCol, 2);
            else
            {
                snprintf(buf, sizeof(buf), "%d DEVICE%s", chan_.count,
                         chan_.count == 1 ? "" : "S");
                s.drawText(8, 8, buf, nameCol, 2);
            }

            // Device chain: 4 slots per screen, screen button selects.
            for (int col = 0; col < 4; col++)
            {
                const auto & d  = chan_.dev[screen * 4 + col];
                int          x0 = col * 120;
                if (col > 0)
                    s.fillRect(x0, 30, 1, s.height - 30, kSep);
                if (!d.present)
                    continue;

                if (d.selected)   // orange frame around the selected device
                {
                    s.fillRect(x0 + 3, 44, 114, 3, kOrange);
                    s.fillRect(x0 + 3, 205, 114, 3, kOrange);
                    s.fillRect(x0 + 3, 44, 3, 164, kOrange);
                    s.fillRect(x0 + 114, 44, 3, 164, kOrange);
                }
                s.fillRect(x0 + 14, 60, 10, 10, d.active ? kGreen : kGrey);   // on/off dot
                s.drawText(x0 + 14, 84, d.name.substr(0, 8), d.selected ? kWhite : kGrey, 2);
                if (d.name.size() > 8)
                    s.drawText(x0 + 14, 104, d.name.substr(8, 8), d.selected ? kWhite : kGrey, 2);
                if (d.isEq8)
                    s.drawText(x0 + 14, 180, "EQ8!", kOrange, 2);
                s.drawText(x0 + 14, 236, "SELECT", d.selected ? kOrange : kGrey, 2);
            }
            studio_.drawDisplay(screen, s.pixels);
        }
    }

    // Approximate one EQ8 band's dB response at frequency f — visualization
    // grade, not DSP grade. Mappings: freq 10 Hz·2200^n, gain ±15 dB, Q
    // 0.1·180^n; type indices per EQ8's filter-type chooser.
    double bandResponse(const Eq8Band & b, double f) const
    {
        double f0   = 10.0 * pow(2200.0, (double)b.freqN);
        double gain = -15.0 + 30.0 * (double)b.gainN;
        double q    = 0.1 * pow(180.0, (double)b.qN);
        double x    = log2(f / f0);
        switch (b.type)
        {
            case 0:  return x < 0 ? std::max(-60.0, 48.0 * x) : 0;    // low cut 48
            case 1:  return x < 0 ? std::max(-60.0, 12.0 * x) : 0;    // low cut 12
            case 2:  return gain * (0.5 - 0.5 * tanh(1.5 * x));       // low shelf
            default:
            case 3:  return gain / (1.0 + x * x * q * q * 2.0);       // bell
            case 4:  return -30.0 / (1.0 + x * x * q * q * 8.0);      // notch
            case 5:  return gain * (0.5 + 0.5 * tanh(1.5 * x));       // high shelf
            case 6:  return x > 0 ? std::max(-60.0, -12.0 * x) : 0;   // high cut 12
            case 7:  return x > 0 ? std::max(-60.0, -48.0 * x) : 0;   // high cut 48
        }
    }

    void renderPlugin()
    {
        const uint16_t kWhite = 0xFFFF, kOrange = 0xFC00, kGrey = 0x8410;
        const uint16_t kHeader = gfx::rgb565(26, 26, 26);
        const uint16_t kTrack  = gfx::rgb565(20, 20, 20);
        const uint16_t kSep    = gfx::rgb565(45, 45, 45);
        const uint16_t kGridLn = gfx::rgb565(40, 40, 40);
        char buf[40];

        if (!plug_.isEq8)
        {
            // Generic device: 8 parameter strips, knobs 1–8.
            for (int screen = 0; screen < 2; screen++)
            {
                auto & s = (screen == 0) ? page0_ : page1_;
                s.clear(0);
                s.fillRect(0, 0, s.width, 26, kHeader);
                if (screen == 0)
                    s.drawText(8, 6, plug_.device.empty() ? "..." : plug_.device.substr(0, 20),
                               kWhite, 2);
                else
                {
                    snprintf(buf, sizeof(buf), "PARAMS %d-%d / %d", plugOffset_ + 1,
                             std::min(plugOffset_ + 8, plugTotal_), plugTotal_);
                    s.drawText(8, 6, plugTotal_ > 0 ? buf : "...", kWhite, 2);
                }
                for (int col = 0; col < 4; col++)
                {
                    const auto & p  = plug_.params[screen * 4 + col];
                    int          x0 = col * 120;
                    if (col > 0)
                        s.fillRect(x0, 26, 1, s.height - 26, kSep);
                    if (!p.present)
                        continue;
                    s.drawText(x0 + 8, 38, p.name.substr(0, 9), kGrey, 2);
                    const int barTop = 66, barH = 150;
                    s.fillRect(x0 + 44, barTop, 20, barH, kTrack);
                    int vh = (int)(barH * p.norm);
                    s.fillRect(x0 + 44, barTop + barH - vh, 20, vh, kOrange);
                    s.drawText(x0 + 8, 238, p.disp.substr(0, 9), kWhite, 2);
                }
                studio_.drawDisplay(screen, s.pixels);
            }
            return;
        }

        // --- EQ8: left screen = response curve, right = band strips ---------
        auto & s = page0_;
        s.clear(0);
        s.fillRect(0, 0, s.width, 26, kHeader);
        s.drawText(8, 6, "EQ EIGHT", kOrange, 2);
        s.drawText(150, 6, plug_.device.substr(0, 18), kGrey, 2);

        const int midY = 148, pxPerDb = 5;   // ±20 dB fits 26..270
        for (double f : {100.0, 1000.0, 10000.0})   // decade gridlines
        {
            int gx = (int)(log(f / 20.0) / log(1000.0) * s.width);
            s.fillRect(gx, 28, 1, s.height - 28, kGridLn);
        }
        s.fillRect(0, midY, s.width, 1, kGridLn);   // 0 dB

        int prevY = -1;
        for (int x = 0; x < s.width; x++)
        {
            double f = 20.0 * pow(1000.0, x / (double)s.width);
            double db = 0;
            for (const auto & b : plug_.bands)
                if (b.on)
                    db += bandResponse(b, f);
            db = std::min(20.0, std::max(-20.0, db));
            int y = midY - (int)lround(db * pxPerDb);
            y = std::min(s.height - 3, std::max(29, y));
            if (prevY < 0)
                prevY = y;
            s.fillRect(x, std::min(y, prevY), 2, std::abs(y - prevY) + 2, kOrange);
            prevY = y;
        }
        for (int i = 0; i < 8; i++)   // band markers
        {
            const auto & b = plug_.bands[i];
            if (!b.on)
                continue;
            double f0 = 10.0 * pow(2200.0, (double)b.freqN);
            int bx = (int)(log(std::max(20.0, f0) / 20.0) / log(1000.0) * s.width);
            int by = midY - (int)lround((-15.0 + 30.0 * b.gainN) * pxPerDb);
            by = std::min(s.height - 12, std::max(38, by));
            fillCircle(s, bx, by, 8, kWhite);
            snprintf(buf, sizeof(buf), "%d", i + 1);
            s.drawText(bx - 4, by - 6, buf, 0x0000, 2);
        }
        studio_.drawDisplay(0, s.pixels);

        // Right screen: 8 band strips — number, gain bar (center-zero), freq.
        auto & r = page1_;
        r.clear(0);
        r.fillRect(0, 0, r.width, 26, kHeader);
        static const char * kFieldNames[3] = {"GAIN", "FREQ", "Q"};
        snprintf(buf, sizeof(buf), "KNOBS = BAND %s  (< > SWITCH)", kFieldNames[eq8Field_]);
        r.drawText(8, 6, buf, kOrange, 2);
        r.drawText(300, 6, "BTNS = ON/OFF", kGrey, 2);
        for (int i = 0; i < 8; i++)
        {
            const auto & b  = plug_.bands[i];
            int          x0 = i * 60;
            if (i > 0)
                r.fillRect(x0, 26, 1, r.height - 26, kSep);
            snprintf(buf, sizeof(buf), "%d", i + 1);
            r.drawText(x0 + 24, 36, buf, b.on ? kWhite : kGrey, 2);

            const int barTop = 60, barH = 150, mid = barTop + barH / 2;
            r.fillRect(x0 + 18, barTop, 24, barH, kTrack);
            r.fillRect(x0 + 14, mid, 32, 1, kGridLn);
            if (b.on)
            {
                int gh = (int)lround((b.gainN - 0.5) * barH);   // ± half-bar
                if (gh >= 0)
                    r.fillRect(x0 + 18, mid - gh, 24, std::max(1, gh), kOrange);
                else
                    r.fillRect(x0 + 18, mid, 24, -gh, kOrange);
            }
            double f0 = 10.0 * pow(2200.0, (double)b.freqN);
            if (f0 >= 1000)
                snprintf(buf, sizeof(buf), "%.1fK", f0 / 1000.0);
            else
                snprintf(buf, sizeof(buf), "%.0f", f0);
            r.drawText(x0 + 8, 238, buf, b.on ? kWhite : kGrey, 2);
        }
        studio_.drawDisplay(1, r.pixels);
    }

    void renderHud()
    {
        const uint16_t kOrange = 0xFC00, kWhite = 0xFFFF, kGrey = 0x8410,
                       kGreen = 0x07E0, kBarBg = 0x2104;
        char buf[48];

        page0_.clear(0);
        page0_.drawText(8, 8, "ABLETON LIVE", kOrange, 2);
        page0_.fillRect(0, 30, page0_.width, 2, kOrange);
        snprintf(buf, sizeof(buf), "%.1f BPM", hud_.tempo);
        page0_.drawText(8, 52, buf, kWhite, 6);
        snprintf(buf, sizeof(buf), "%s  %s", hud_.playing ? "PLAYING" : "STOPPED", hud_.pos.c_str());
        page0_.drawText(8, 120, buf, hud_.playing ? kGreen : kGrey, 4);
        page0_.drawText(8, 170, "MASTER VOL", kGrey, 3);
        page0_.fillRect(8, 205, 300, 18, kBarBg);
        page0_.fillRect(8, 205, (int)(300 * hud_.vol), 18, kOrange);
        snprintf(buf, sizeof(buf), "COLOR TUNE  K1 GAMMA %.2f  K2 SAT %.2f", gamma_, sat_);
        page0_.drawText(8, 250, buf, kGrey, 2);
        studio_.drawDisplay(0, page0_.pixels);
    }

    gfx::Surface * surfaceFor(std::optional<int32_t> disp)
    {
        if (disp == 0) return &left_;
        if (disp == 1) return &right_;
        return nullptr;
    }

    void send(const osc::Message & msg)
    {
        if (!hasSubscriber_)
            return;
        auto bytes = msg.encode();
        sendto(fd_, bytes.data(), bytes.size(), 0,
               (const sockaddr *)&subscriber_, sizeof(subscriber_));
    }

    struct HudState
    {
        bool        active = false;
        float       tempo  = 120;
        bool        playing = false;
        std::string pos    = "1.1.1";
        float       vol    = 0;
    };

    struct TrackState
    {
        bool        present = false;
        std::string name;
        float       vol    = 0;
        float       meterT = 0, meterC = 0;   // target / animated current
        bool        muted  = false;
        bool        solo   = false;
        int32_t     color  = -1;   // Live's 0xRRGGBB track color, -1 = none
        bool        isGroup = false;
        bool        folded  = false;
        int32_t     depth   = 0;
        double      echoAt = -1000;           // clock_ of our last local echo
    };

    studio::StudioController & studio_;
    int                        fd_;
    gfx::Surface               left_, right_;    // client-staged (/display/*)
    gfx::Surface               page0_, page1_;   // daemon-rendered pages
    sockaddr_in                subscriber_{};
    bool                       hasSubscriber_ = false;
    Mode                       mode_ = Mode::Waiting;
    float                      vuTarget_[2]  = {0, 0};
    float                      vuCurrent_[2] = {0, 0};
    HudState                   hud_;
    bool                       hudDirty_    = false;
    double                     hudCooldown_ = 0;
    double                     hudEchoAt_   = -1000;
    double                     clock_       = 0;   // daemon uptime, ticks' timebase
    TrackState                 tracks_[8];
    bool                       mixDirty_    = false;
    double                     mixCooldown_ = 0;
    int                        mixOffset_   = 0;
    int                        mixTotal_    = 0;
    bool                       muteHeld_    = false;
    bool                       soloHeld_    = false;
    ChanState                  chan_;
    PlugState                  plug_;
    bool                       chanDirty_   = false;
    bool                       plugDirty_   = false;
    int                        plugOffset_  = 0;
    int                        plugTotal_   = 0;
    int                        eq8Field_    = 0;   // 0 gain, 1 freq, 2 Q (arrows cycle)
    double                     pageCooldown_ = 0;
    // Defaults dialed in on real glass (2026-07-23, the dev unit's panels).
    double gamma_ = getenv("MACCHINA_GAMMA") ? atof(getenv("MACCHINA_GAMMA")) : 1.32;
    double sat_   = getenv("MACCHINA_SAT")   ? atof(getenv("MACCHINA_SAT"))   : 1.44;
    double                     ballX_ = 100, ballY_ = 120, ballVX_ = 170, ballVY_ = 0;
    std::deque<std::pair<int, int>> trail_;
    double                     spinPos_      = 0;
    int                        spinSeg_      = -1;
    double                     animCooldown_ = 0;
};

} // namespace


int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : "39195855";
    int         port   = getenv("MACCHINA_OSC_PORT") ? atoi(getenv("MACCHINA_OSC_PORT")) : 7000;

    // UDP socket, loopback only — front-ends live on this machine.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("macchinad: socket");
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("macchinad: bind (is another macchinad running?)");
        return 1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    CFMessagePortTransport   transport;
    studio::StudioController studio(transport, serial);
    Daemon                   daemon(studio, fd);
    studio.onInput = [&daemon](const InputEvent & e) { daemon.onInput(e); };

    fprintf(stderr, "macchinad: connecting to Maschine Studio (serial %s)…\n", serial.c_str());
    if (!studio.connect())
    {
        fprintf(stderr, "macchinad: connect failed — is NIHardwareAgent running and the Studio on?\n");
        return 1;
    }
    daemon.begin(port);   // waiting mode: info panel + the bouncing ball
    fprintf(stderr, "macchinad: ready — OSC on udp://127.0.0.1:%d\n", port);

    signal(SIGINT, onSigint);

    uint8_t     packet[2048];
    sockaddr_in from{};
    socklen_t   fromLen;

    while (!gStopRequested)
    {
        transport.runFor(1.0 / 60);   // device events dispatch here
        daemon.tick(1.0 / 60);        // VU ballistics + HUD redraws

        // Drain every pending UDP packet.
        for (;;)
        {
            fromLen = sizeof(from);
            ssize_t got = recvfrom(fd, packet, sizeof(packet), 0, (sockaddr *)&from, &fromLen);
            if (got <= 0)
                break;
            daemon.handlePacket(packet, (size_t)got, from);
        }
    }

    // Blank the device on the way out.
    studio.setAllLeds(0);
    studio.flushLeds();
    for (int i = 0; i < studio.displayCount(); i++)
        studio.fillDisplay(i, 0x0000);
    close(fd);
    fprintf(stderr, "\nmacchinad: blanked, bye.\n");
    return 0;
}
