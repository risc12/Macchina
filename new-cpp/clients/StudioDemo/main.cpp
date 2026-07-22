//
//  main.cpp
//  Macchina — new-cpp/clients/StudioDemo
//
//  Demo of StudioController through the Controller interface: connect to the
//  real NIHardwareAgent as a v3 Studio client, drive the displays + LEDs and
//  print pad/knob/button/wheel input. C++ port of new/clients/StudioDemo.
//
//  Usage:   StudioDemoCpp [serial]        (default serial 39195855)
//  Env:
//    (default)             all LEDs "breathe" (brightness ramps up/down)
//    MACCHINA_PADS=1       pads+groups RGB rainbow
//    MACCHINA_VU=1         bouncing VU meters (+MACCHINA_VU_SWEEP=1: colour ramp)
//    MACCHINA_JOG=1        fill the jog ring (+MACCHINA_JOG_CHASE=1: walk it)
//    MACCHINA_LED_ALL=NN   static hex level 00–7f for every LED
//    MACCHINA_LED_CHASE=1  walk lit channels (FROM/TO/MS/WIDTH as in the ObjC demo)
//    MACCHINA_NO_DRAW=1    skip the initial display fill
//    MACCHINA_RUN_SECS=N   exit (LEDs blanked) after N seconds — for scripted runs
//
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "studio/StudioController.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace macchina;

namespace {

// Simple hue sweep → RGB, components 0–127.
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

long envLong(const char * name, long fallback)
{
    const char * v = getenv(name);
    return v ? strtol(v, nullptr, 10) : fallback;
}

bool envFlag(const char * name)
{
    const char * v = getenv(name);
    return v && *v && *v != '0';
}

// Ctrl-C requests a clean exit (LEDs + displays blanked) instead of dying
// mid-animation with the panel lit.
volatile sig_atomic_t gStopRequested = 0;
void onSigint(int) { gStopRequested = 1; }

void logInput(const InputEvent & e)
{
    switch (e.type)
    {
        case InputEvent::Type::Pad:
        {
            const char * what = e.padState == studio::kPadHit      ? "hit    " :
                                e.padState == studio::kPadRelease  ? "release" :
                                e.padState == studio::kPadPressure ? "press  " : "?      ";
            printf("pad %2u  %s  pressure %.3f\n", e.id, what, e.pressure);
            break;
        }
        case InputEvent::Type::Button:
            printf("button 0x%02x  %s\n", e.id, e.down ? "DOWN" : "up");
            break;
        case InputEvent::Type::Knob:
            printf("knob %u  delta %+.4f\n", e.id, e.delta);
            break;
        case InputEvent::Type::Wheel:
            printf("wheel %u  step %+d\n", e.id, e.step);
            break;
    }
    fflush(stdout);
}

} // namespace


int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : "39195855";

    CFMessagePortTransport   transport;
    studio::StudioController studio(transport, serial);
    studio.onInput = logInput;

    fprintf(stderr, "connecting to Maschine Studio (serial %s)…\n", serial.c_str());
    if (!studio.connect())
    {
        fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
        return 1;
    }
    fprintf(stderr, "connected + focused. Press pads / buttons / knobs / the wheel to see events.\n");

    // Displays: fill left red, right blue (unmissable proof of the draw path).
    if (!envFlag("MACCHINA_NO_DRAW"))
    {
        studio.fillDisplay(0, 0xF800);   // red
        studio.fillDisplay(1, 0x001F);   // blue
    }

    // Pick the LED animation; each is a function of elapsed wall-clock seconds
    // so timing stays smooth even if a tick is late.
    enum class Mode { Breathe, Pads, VU, Jog, All, Chase } mode = Mode::Breathe;
    const char * allEnv = getenv("MACCHINA_LED_ALL");
    if      (envFlag("MACCHINA_PADS"))      { mode = Mode::Pads;  fprintf(stderr, "painting pads+groups a rainbow\n"); }
    else if (envFlag("MACCHINA_VU"))        { mode = Mode::VU;    fprintf(stderr, "bouncing both VU meters\n"); }
    else if (envFlag("MACCHINA_JOG"))       { mode = Mode::Jog;   fprintf(stderr, "driving the jog ring\n"); }
    else if (allEnv)                        { mode = Mode::All;   fprintf(stderr, "holding ALL LEDs at level 0x%02lx\n", strtoul(allEnv, nullptr, 16)); }
    else if (envFlag("MACCHINA_LED_CHASE")) { mode = Mode::Chase; }

    const bool    vuSweep    = envFlag("MACCHINA_VU_SWEEP");
    const bool    jogChase   = envFlag("MACCHINA_JOG_CHASE");
    const uint8_t allLevel   = allEnv ? (uint8_t)strtoul(allEnv, nullptr, 16) : 0;
    const long    chaseFrom  = envLong("MACCHINA_LED_CHASE_FROM", 0);
    const long    chaseTo    = envLong("MACCHINA_LED_CHASE_TO", (long)studio::kLedChannelCount - 1);
    const double  chaseMs    = (double)envLong("MACCHINA_LED_CHASE_MS", 400);
    const long    chaseWidth = std::max(1L, envLong("MACCHINA_LED_CHASE_WIDTH", 1));
    const long    runSecs    = envLong("MACCHINA_RUN_SECS", 0);

    signal(SIGINT, onSigint);

    long lastChaseBlock = -1, lastSweepStep = -1;
    auto t0 = std::chrono::steady_clock::now();

    while (!gStopRequested)
    {
        // Service the event port (input, notifications), then animate.
        transport.runFor(1.0 / 60);

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (runSecs > 0 && secs >= (double)runSecs)
            break;

        switch (mode)
        {
            case Mode::Breathe:
            {
                // Brightness follows a smooth cosine of wall-clock time, 0→127→0.
                const double period = 2.4;
                auto level = (uint8_t)lround(63.5 * (1.0 - cos(2.0 * M_PI * secs / period)));
                studio.setAllLeds(level);
                break;
            }
            case Mode::Pads:
            {
                studio.setAllLeds(0);
                uint8_t r, g, b;
                for (int i = 0; i < studio::kPadCount; i++)
                {
                    hueToRGB((double)i / studio::kPadCount, r, g, b);
                    studio.setPadLed((size_t)i, r, g, b);
                }
                for (int i = 0; i < studio::kGroupCount; i++)
                {
                    hueToRGB((double)i / studio::kGroupCount, r, g, b);
                    studio.setGroupLed((size_t)i, r, g, b);
                }
                break;
            }
            case Mode::VU:
            {
                studio.setAllLeds(0);
                if (vuSweep)
                {
                    // Hold both meters full; ramp the colour channel 0→127 (~8 s)
                    // to reveal every colour LED_LEVEL_COLOR can produce.
                    studio.setVU(true,  studio::kVUSegments);
                    studio.setVU(false, studio::kVUSegments);
                    long lvl = (long)fmod(secs * 16.0, 128.0);
                    studio.setLed(studio::kVUColorChannel, (uint8_t)lvl);
                    if (lvl / 8 != lastSweepStep)
                    {
                        lastSweepStep = lvl / 8;
                        fprintf(stderr, "LED_LEVEL_COLOR = %ld\n", lvl);
                    }
                }
                else
                {
                    // Left/right bounce out of phase.
                    auto l = (size_t)lround(studio::kVUSegments * 0.5 * (1 - cos(2 * M_PI * secs / 1.5)));
                    auto r = (size_t)lround(studio::kVUSegments * 0.5 * (1 - cos(2 * M_PI * secs / 1.5 + M_PI)));
                    studio.setVU(true, l);
                    studio.setVU(false, r);
                    studio.setVUBlue(((long)(secs / 3.0) & 1) == 0);
                }
                break;
            }
            case Mode::Jog:
            {
                studio.setAllLeds(0);
                if (jogChase)
                    studio.setJogRingSegment((size_t)(secs / 0.15) % studio::kJogRingSegments, 127);
                else
                    for (int i = 0; i < studio::kJogRingSegments; i++)
                        studio.setJogRingSegment((size_t)i, 127);
                break;
            }
            case Mode::All:
                studio.setAllLeds(allLevel);
                break;
            case Mode::Chase:
            {
                // Walk `width` lit channels from..to, advancing every chaseMs.
                long span    = (chaseTo >= chaseFrom) ? (chaseTo - chaseFrom + 1) : 1;
                long nblocks = (span + chaseWidth - 1) / chaseWidth;
                long block   = (long)(secs * 1000.0 / chaseMs) % nblocks;
                long ch      = chaseFrom + block * chaseWidth;
                studio.setAllLeds(0);
                for (long i = 0; i < chaseWidth && ch + i <= chaseTo; i++)
                    studio.setLed((size_t)(ch + i), 127);
                if (block != lastChaseBlock)
                {
                    lastChaseBlock = block;
                    fprintf(stderr, "LED chase: channels %ld–%ld\n", ch,
                            std::min(ch + chaseWidth - 1, chaseTo));
                }
                break;
            }
        }
        studio.flushLeds();
    }

    // Good citizen on exit (Ctrl-C or MACCHINA_RUN_SECS): leave the panel
    // dark, not mid-animation.
    studio.setAllLeds(0);
    studio.flushLeds();
    for (int i = 0; i < studio.displayCount(); i++)
        studio.fillDisplay(i, 0x0000);   // black
    fprintf(stderr, "LEDs + displays blanked, exiting.\n");
    return 0;
}
