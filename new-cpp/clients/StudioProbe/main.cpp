//
//  main.cpp
//  Macchina — new-cpp/clients/StudioProbe
//
//  Interactive probe: press any control on the Studio and it lights up +
//  prints its name (or its raw id, if the StudioLayout table doesn't know it
//  yet — that's your cue to extend the table). The terminal side is a small
//  LedPoke-style REPL for poking LED channels by hand. The two together are
//  the instrument for finishing new-cpp/studio/StudioLayout.cpp.
//
//  Usage:   StudioProbe [serial]        (default serial 39195855)
//  Type 'help' at the prompt for commands. Ctrl-C / 'q' blanks and exits.
//
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "studio/StudioController.hpp"
#include "studio/StudioLayout.hpp"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <ctime>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace macchina;

namespace {

volatile sig_atomic_t gStopRequested = 0;
void onSigint(int) { gStopRequested = 1; }

struct Probe
{
    studio::StudioController & studio;

    // Press-lighting saves the poked levels underneath and restores on
    // release, so REPL experiments survive you mashing buttons.
    std::array<uint8_t, studio::kLedChannelCount> saved{};
    int jogCursor = 0;

    void lightControl(const studio::ControlInfo & c, bool down)
    {
        if (c.ledChannel < 0)
            return;
        int n = c.rgb ? 3 : 1;
        for (int i = 0; i < n; i++)
        {
            auto ch = (size_t)c.ledChannel + i;
            if (down)
            {
                saved[ch] = ledLevel(ch);
                studio.setLed(ch, 127);
            }
            else
                studio.setLed(ch, saved[ch]);
        }
        studio.flushLeds();
    }

    // The controller doesn't expose its buffer; shadow the levels we set.
    std::array<uint8_t, studio::kLedChannelCount> shadow{};
    uint8_t ledLevel(size_t ch) const { return ch < shadow.size() ? shadow[ch] : 0; }
    void    setLed(size_t ch, uint8_t lvl)
    {
        if (ch < shadow.size())
            shadow[ch] = lvl;
        studio.setLed(ch, lvl);
    }

    void handle(const InputEvent & e)
    {
        switch (e.type)
        {
            case InputEvent::Type::Button:
            {
                const auto * c = studio::controlForButton(e.id);
                if (c)
                    printf("button 0x%02x  %-12s %s%s\n", e.id, c->name,
                           e.down ? "DOWN" : "up",
                           c->ledChannel < 0 ? "   (no LED channel yet — find it via poke!)" : "");
                else
                    printf("button 0x%02x  %-12s %s   << NOT IN StudioLayout — add it!\n",
                           e.id, "?", e.down ? "DOWN" : "up");
                if (c)
                    lightControl(*c, e.down);
                break;
            }
            case InputEvent::Type::Pad:
            {
                int pad = studio::padIndexForInputId(e.id);
                const char * what = e.padState == studio::kPadHit      ? "hit    " :
                                    e.padState == studio::kPadRelease  ? "release" :
                                    e.padState == studio::kPadPressure ? "press  " : "?      ";
                printf("pad id %2u = Pad %-2d  %s  pressure %.3f\n", e.id, pad + 1, what, e.pressure);
                if (pad >= 0)
                {
                    // Light white, brightness tracking pressure; off on release.
                    auto lvl = (uint8_t)(e.padState == studio::kPadRelease
                                             ? 0 : 32 + e.pressure * 95);
                    studio.setPadLed((size_t)pad, lvl, lvl, lvl);
                    studio.flushLeds();
                }
                break;
            }
            case InputEvent::Type::Knob:
                printf("knob %u  delta %+.4f\n", e.id, e.delta);
                break;
            case InputEvent::Type::Wheel:
            {
                // Walk a lit cursor around the jog ring, one segment per detent.
                int n = studio::kJogRingSegments;
                studio.setJogRingSegment((size_t)jogCursor, 0);
                jogCursor = ((jogCursor + e.step) % n + n) % n;
                studio.setJogRingSegment((size_t)jogCursor, 127);
                studio.flushLeds();
                printf("wheel %u  step %+d  (ring seg %d = ch %zu)\n",
                       e.id, e.step, jogCursor + 1, studio::kJogRingBase + jogCursor);
                break;
            }
        }
        fflush(stdout);
    }
};

void printHelp()
{
    printf("commands:\n"
           "  <chan> <level>   set LED channel (0-212) to level (0-127)\n"
           "  all <level>      set every channel\n"
           "  off              everything dark\n"
           "  what <chan>      name a channel\n"
           "  list             known button-id <-> channel map (-1 = unmapped)\n"
           "  help             this text\n"
           "  q                blank + quit\n");
}

void printList()
{
    printf("%-14s %-18s %-9s %s\n", "control", "m2 name", "buttonId", "ledChannel");
    for (const auto & c : studio::controls())
    {
        char id[8] = "-1", ch[8] = "-1";
        if (c.buttonId >= 0)   snprintf(id, sizeof(id), "0x%02x", c.buttonId);
        if (c.ledChannel >= 0) snprintf(ch, sizeof(ch), "%d", c.ledChannel);
        printf("%-14s %-18s %-9s %s%s\n", c.name, c.m2Name, id, ch, c.rgb ? " (RGB)" : "");
    }

    // Pads are their own id space (pad events, not switches).
    printf("\n%-14s %-18s %-9s %s\n", "pad", "m2 name", "padId", "ledChannel");
    for (int p = 0; p < studio::kPadCount; p++)
    {
        size_t rgbBase   = (p < 8) ? studio::kPadLedBase[0] + (size_t)p * 3
                                   : studio::kPadLedBase[1] + (size_t)(p - 8) * 3;
        size_t whiteChan = (p < 8) ? studio::kPadWhiteBase[0] + (size_t)p
                                   : studio::kPadWhiteBase[1] + (size_t)(p - 8);
        char name[16], m2[16];
        snprintf(name, sizeof(name), "Pad %d", p + 1);
        snprintf(m2, sizeof(m2), "PAD_%d", p + 1);
        printf("%-14s %-18s %-9d %zu (RGB) + %zu (white)\n",
               name, m2, studio::padInputIdForIndex(p), rgbBase, whiteChan);
    }
}

// One REPL line. Returns false on quit.
bool handleCommand(Probe & probe, char * line)
{
    char cmd[16] = {0};
    long a = -1, b = -1;
    int  n = sscanf(line, "%15s %ld %ld", cmd, &a, &b);
    if (n < 1)
        return true;

    if (!strcmp(cmd, "q") || !strcmp(cmd, "quit"))
        return false;
    if (!strcmp(cmd, "help"))
        printHelp();
    else if (!strcmp(cmd, "list"))
        printList();
    else if (!strcmp(cmd, "off"))
    {
        probe.shadow.fill(0);
        probe.studio.setAllLeds(0);
        probe.studio.flushLeds();
    }
    else if (!strcmp(cmd, "all") && a >= 0 && a <= 127)
    {
        probe.shadow.fill((uint8_t)a);
        probe.studio.setAllLeds((uint8_t)a);
        probe.studio.flushLeds();
    }
    else if (!strcmp(cmd, "what") && a >= 0 && a < (long)studio::kLedChannelCount)
        printf("channel %ld = %s\n", a, studio::describeChannel((size_t)a).c_str());
    else
    {
        // "<chan> <level>"
        char * end   = nullptr;
        long   chan  = strtol(cmd, &end, 10);
        long   level = a;
        if (end && *end == 0 && n >= 2 &&
            chan >= 0 && chan < (long)studio::kLedChannelCount && level >= 0 && level <= 127)
        {
            probe.setLed((size_t)chan, (uint8_t)level);
            probe.studio.flushLeds();
            printf("channel %ld (%s) = %ld\n", chan,
                   studio::describeChannel((size_t)chan).c_str(), level);
        }
        else
            printf("? — try 'help'\n");
    }
    fflush(stdout);
    return true;
}

} // namespace


int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : "39195855";

    CFMessagePortTransport   transport;
    studio::StudioController studio(transport, serial);
    Probe                    probe{studio};
    studio.onInput = [&probe](const InputEvent & e) { probe.handle(e); };

    fprintf(stderr, "connecting to Maschine Studio (serial %s)…\n", serial.c_str());
    if (!studio.connect())
    {
        fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
        return 1;
    }
    fprintf(stderr, "connected + focused. Press controls on the device, or poke LEDs here.\n");
    printHelp();
    printf("> ");
    fflush(stdout);

    signal(SIGINT, onSigint);

    // Line-buffered stdin, polled between event-loop slices so device events
    // and typed commands interleave on one thread.
    char   linebuf[256];
    size_t linelen = 0;
    long   runSecs = getenv("MACCHINA_RUN_SECS") ? strtol(getenv("MACCHINA_RUN_SECS"), nullptr, 10) : 0;
    auto   start   = time(nullptr);

    while (!gStopRequested)
    {
        transport.runFor(1.0 / 30);
        if (runSecs > 0 && time(nullptr) - start >= runSecs)
            break;

        struct pollfd pfd = { .fd = 0, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
            continue;

        ssize_t got = read(0, linebuf + linelen, sizeof(linebuf) - linelen - 1);
        if (got <= 0)   // EOF (piped input ran out): keep serving the device
            continue;
        linelen += (size_t)got;
        linebuf[linelen] = 0;

        char * nl;
        bool   keepGoing = true;
        while (keepGoing && (nl = (char *)memchr(linebuf, '\n', linelen)) != nullptr)
        {
            *nl = 0;
            keepGoing = handleCommand(probe, linebuf);
            size_t rest = linelen - (size_t)(nl + 1 - linebuf);
            memmove(linebuf, nl + 1, rest);
            linelen = rest;
            if (keepGoing)
            {
                printf("> ");
                fflush(stdout);
            }
        }
        if (!keepGoing)
            break;
    }

    // Leave the panel dark.
    studio.setAllLeds(0);
    studio.flushLeds();
    for (int i = 0; i < studio.displayCount(); i++)
        studio.fillDisplay(i, 0x0000);
    fprintf(stderr, "\nLEDs + displays blanked, bye.\n");
    return 0;
}
