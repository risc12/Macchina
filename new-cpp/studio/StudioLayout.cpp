//
//  StudioLayout.cpp
//  Macchina — new-cpp/studio
//
//  Channel values are the true (un-shifted) indices: the led_map.tsv probe
//  ran under the old +4 shift, so tsv channel − 4 = the value here.
//

#include "studio/StudioLayout.hpp"

#include <array>
#include <cstdio>

namespace macchina::studio {

namespace {

// clang-format off
constexpr std::array kControls = std::to_array<ControlInfo>({
    // --- buttons above the screens (mono, ch 32–39; ids captured live
    //     2026-07-22, left-to-right sweep — scan order, not spatial) ----------
    { "Screen 1",    "SCREEN_1",         0x40,  32, false },
    { "Screen 2",    "SCREEN_2",         0x47,  33, false },
    { "Screen 3",    "SCREEN_3",         0x46,  34, false },
    { "Screen 4",    "SCREEN_4",         0x41,  35, false },
    { "Screen 5",    "SCREEN_5",         0x45,  36, false },
    { "Screen 6",    "SCREEN_6",         0x42,  37, false },
    { "Screen 7",    "SCREEN_7",         0x44,  38, false },
    { "Screen 8",    "SCREEN_8",         0x43,  39, false },

    // --- left column (mono, ch 40–49; ids captured 2026-07-22, top-to-bottom
    //     sweep in panel order) ----------------------------------------------
    { "Channel",     "CHANNEL",          0x07,  40, false },
    { "Plug-in",     "PLUGIN",           0x00,  41, false },
    { "Arrange",     "ARRANGE",          0x06,  42, false },
    { "Mix",         "MIX",              0x01,  43, false },
    { "Browse",      "BROWSE",           0x05,  44, false },
    { "Sampling",    "SAMPLE",           0x02,  45, false },
    { "Left Arrow",  "LEFT",             0x04,  46, false },
    { "Right Arrow", "RIGHT",            0x03,  47, false },
    { "All",         "ALL",              0x0d,  48, false },
    { "Auto",        "AUTO_WRITE",       0x0c,  49, false },

    // --- right meter-section buttons (mono, ch 54–61) -----------------------
    { "In 1",        "LEVEL_IN1",        0x48,  54, false },
    { "In 2",        "LEVEL_IN2",        0x49,  55, false },
    { "In 3",        "LEVEL_IN3",        0x4a,  56, false },
    { "In 4",        "LEVEL_IN4",        0x4b,  57, false },
    { "MST",         "LEVEL_MASTER",     0x4f,  58, false },
    { "GRP",         "LEVEL_GROUP",      0x4e,  59, false },
    { "SND",         "LEVEL_SOUND",      0x4d,  60, false },
    { "CUE",         "LEVEL_CUE",        0x4c,  61, false },

    // --- edit row (mono, ch 94–102) ----------------------------------------
    { "Copy",        "EDIT_COPY",        0x2b,  94, false },
    { "Paste",       "EDIT_PASTE",       0x28,  95, false },
    { "Note",        "EDIT_NOTE",        0x2f,  96, false },
    { "Nudge",       "EDIT_NUDGE",       0x2c,  97, false },
    { "Undo",        "EDIT_UNDO",        0x2a,  98, false },
    { "Redo",        "EDIT_REDO",        0x29,  99, false },
    { "Quantize",    "EDIT_QUANTIZE",    0x2e, 100, false },
    { "Clear",       "EDIT_CLEAR",       0x2d, 101, false },
    { "Back",        "BACK",             0x37, 102, false },

    // --- wheel-area extras (channels via s_ledToNHLMap; ids live-verified
    //     2026-07-22) --------------------------------------------------------
    // Enter is a real BUTTON next to the wheel (0x34, owns LED_ENTER=105);
    // pressing the wheel itself is a separate control (0x33, BUTTON_WHEEL,
    // no LED — the M2 enum has no LED_WHEEL either).
    { "Prev",        "TRANSPORT_PREV",   0x36, 103, false },
    { "Next",        "TRANSPORT_NEXT",   0x35, 104, false },
    { "Enter",       "ENTER",            0x34, 105, false },
    { "Wheel Push",  "WHEEL",            0x33,  -1, false },

    // --- groups A–H (RGB, ch 106+3g; whites at kGroupWhiteBase) --------------
    // Ids 0x10–0x17 but NOT in spatial order (hardware scan order) — captured
    // live 2026-07-22 by pressing A→H: 10,13,14,17,11,12,15,16. Consistent
    // with the old tsv captures (A=0x10, B=0x13, E=0x11), which only *looked*
    // contradictory under a sequential-ids assumption. Press-to-light in
    // StudioProbe verifies each pair visually.
    { "Group A",     "GROUP_A",          0x10, 106, true  },
    { "Group B",     "GROUP_B",          0x13, 109, true  },
    { "Group C",     "GROUP_C",          0x14, 112, true  },
    { "Group D",     "GROUP_D",          0x17, 115, true  },
    { "Group E",     "GROUP_E",          0x11, 118, true  },
    { "Group F",     "GROUP_F",          0x12, 121, true  },
    { "Group G",     "GROUP_G",          0x15, 124, true  },
    { "Group H",     "GROUP_H",          0x16, 127, true  },

    // --- performance row (mono, ch 138–141) ---------------------------------
    { "Tap",         "TAP",              0x08, 138, false },
    { "Step Mode",   "STEP",             0x09, 139, false },
    { "Macro",       "MACRO",            0x0a, 140, false },
    { "Note Repeat", "NOTE_REPEAT",      0x0b, 141, false },

    // --- transport (mono; channels via s_ledToNHLMap, ids captured live
    //     2026-07-22, reading-order sweep; scan pattern matches the groups') --
    { "Restart",     "TRANSPORT_LOOP",   0x1c, 142, false },
    { "Metro",       "TRANSPORT_METRO",  0x1f, 143, false },
    { "Events",      "TRANSPORT_EVENTS", 0x18, 144, false },
    { "Grid",        "TRANSPORT_GRID",   0x1b, 145, false },
    { "Play",        "TRANSPORT_PLAY",   0x1d, 146, false },
    { "Rec",         "TRANSPORT_RECORD", 0x1e, 147, false },
    { "Erase",       "TRANSPORT_ERASE",  0x19, 148, false },
    { "Shift",       "SHIFT",            0x1a, 149, false },

    // --- mode column (mono, ch 150–157) --------------------------------------
    { "Scene",       "SCENE",            0x23, 150, false },
    { "Pattern",     "PATTERN",          0x22, 151, false },
    { "Pad Mode",    "PAD_MODE",         0x21, 152, false },
    { "Navigate",    "NAVIGATE",         0x20, 153, false },
    { "Duplicate",   "DUPLICATE",        0x27, 154, false },
    { "Select",      "SELECT",           0x26, 155, false },
    { "Solo",        "SOLO",             0x25, 156, false },
    { "Mute",        "MUTE",             0x24, 157, false },

    // --- knob touch caps (no LEDs; ids captured 2026-07-22, knobs 1–8
    //     left-to-right — the one tidy sequential run on the whole panel) -----
    { "Knob 1 touch", "CAP_1",           0x58,  -1, false },
    { "Knob 2 touch", "CAP_2",           0x59,  -1, false },
    { "Knob 3 touch", "CAP_3",           0x5a,  -1, false },
    { "Knob 4 touch", "CAP_4",           0x5b,  -1, false },
    { "Knob 5 touch", "CAP_5",           0x5c,  -1, false },
    { "Knob 6 touch", "CAP_6",           0x5d,  -1, false },
    { "Knob 7 touch", "CAP_7",           0x5e,  -1, false },
    { "Knob 8 touch", "CAP_8",           0x5f,  -1, false },

    // --- other known switch ids with NO LED ---------------------------------
    { "Snap",        "SNAP",               -1,  -1, false },  // has a button, no LED (per s_ledToNHLMap)
});
// clang-format on

// padId → pad index (0–15). Pads 1–8: wire ids 12,13,14,15,8,9,10,11;
// pads 9–16: 4,5,6,7,0,1,2,3 (live-mapped, protocol.md "LEDs").
constexpr int8_t kPadIndexForId[16] = {
    12, 13, 14, 15,   // ids 0–3  → pads 13–16
     8,  9, 10, 11,   // ids 4–7  → pads 9–12
     4,  5,  6,  7,   // ids 8–11 → pads 5–8
     0,  1,  2,  3,   // ids 12–15→ pads 1–4
};

const char * kRgbSuffix[3] = {": Red", ": Green", ": Blue"};

} // namespace


std::span<const ControlInfo> controls()
{
    return kControls;
}

const ControlInfo * controlForButton(uint32_t buttonId)
{
    for (const auto & c : kControls)
        if (c.buttonId == (int32_t)buttonId)
            return &c;
    return nullptr;
}

int padIndexForInputId(uint32_t padId)
{
    return padId < 16 ? kPadIndexForId[padId] : -1;
}

int padInputIdForIndex(int pad)
{
    for (int id = 0; id < 16; id++)
        if (kPadIndexForId[id] == pad)
            return id;
    return -1;
}

std::string describeChannel(size_t channel)
{
    char buf[64];

    // Pads: two RGB runs + two white runs.
    for (int run = 0; run < 2; run++)
    {
        size_t base = kPadLedBase[run], span = 8 * 3;
        if (channel >= base && channel < base + span)
        {
            size_t off = channel - base;
            snprintf(buf, sizeof(buf), "Pad %zu%s", run * 8 + off / 3 + 1, kRgbSuffix[off % 3]);
            return buf;
        }
        size_t wbase = kPadWhiteBase[run];
        if (channel >= wbase && channel < wbase + 8)
        {
            snprintf(buf, sizeof(buf), "Pad %zu: White", run * 8 + (channel - wbase) + 1);
            return buf;
        }
    }

    // Group whites.
    if (channel >= kGroupWhiteBase && channel < kGroupWhiteBase + 8)
    {
        snprintf(buf, sizeof(buf), "Group %c: White", (char)('A' + channel - kGroupWhiteBase));
        return buf;
    }

    // Table controls (mono + group RGB).
    for (const auto & c : kControls)
    {
        if (c.ledChannel < 0)
            continue;
        auto base = (size_t)c.ledChannel;
        if (!c.rgb && channel == base)
            return c.name;
        if (c.rgb && channel >= base && channel < base + 3)
            return std::string(c.name) + kRgbSuffix[channel - base];
    }

    // VU ladders + colour, jog ring.
    if (channel >= kVULeftBase && channel < kVULeftBase + kVUSegments)
    {
        snprintf(buf, sizeof(buf), "Left VU seg %zu", channel - kVULeftBase + 1);
        return buf;
    }
    if (channel >= kVURightBase && channel < kVURightBase + kVUSegments)
    {
        snprintf(buf, sizeof(buf), "Right VU seg %zu", channel - kVURightBase + 1);
        return buf;
    }
    if (channel == kVUColorChannel)
        return "VU colour (LED_LEVEL_COLOR)";
    if (channel >= kJogLabelBase && channel < kJogLabelBase + (size_t)kJogLabelCount)
    {
        static const char * kJogLabels[kJogLabelCount] =
            {"Edit", "Channel", "Browse", "Tune", "Swing", "Volume"};
        snprintf(buf, sizeof(buf), "Jog label: %s", kJogLabels[channel - kJogLabelBase]);
        return buf;
    }
    if (channel >= kJogRingBase && channel < kJogRingBase + (size_t)kJogRingSegments)
    {
        snprintf(buf, sizeof(buf), "Jog ring seg %zu", channel - kJogRingBase + 1);
        return buf;
    }

    return "(unmapped)";
}

} // namespace macchina::studio
