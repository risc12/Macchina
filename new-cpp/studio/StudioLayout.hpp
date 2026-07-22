//
//  StudioLayout.hpp
//  Macchina — new-cpp/studio
//
//  The Studio's control layout: which physical control has which switch
//  input id and which LED channel. This table IS the map — extend it as the
//  probe tool (clients/StudioProbe) captures more pairs.
//
//  Naming follows Maschine 2's own LED_*/BUTTON_* vocabulary (the Lua
//  scripting reference inside the M2 app) — m2Name is the shared token
//  ("TRANSPORT_PLAY" covers LED_TRANSPORT_PLAY + BUTTON_TRANSPORT_PLAY), so
//  the table cross-references cleanly against NI's own scripts.
//
//  Channel sources: new/docs/reference/studio_led_map_decoded.md — the
//  s_ledToNHLMap dump joined with the M2 enum order (authoritative), plus
//  the live probe led_map.tsv (whose channels carry a +4 shift; ids are
//  unaffected). buttonId or ledChannel −1 = not yet captured / doesn't exist.
//

#pragma once

#include "studio/StudioController.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace macchina::studio {

// RGB pads and groups have a FOURTH, separately-addressable white LED
// (decoded from s_ledToNHLMap — see studio_led_map_decoded.md).
constexpr size_t kPadWhiteBase[2] = {24, 86};   // pads 1–8, pads 9–16
constexpr size_t kGroupWhiteBase  = 130;        // groups A–H

// The six jog function labels (Edit, Channel, Browse, Tune, Swing, Volume) —
// mono, separate from the ring (kJogRingBase).
constexpr size_t kJogLabelBase  = 192;
constexpr int    kJogLabelCount = 6;

struct ControlInfo
{
    const char * name;         // friendly, as printed on the hardware
    const char * m2Name;       // official M2 token (sans LED_/BUTTON_ prefix)
    int32_t      buttonId;     // switch input id (InputEvent::id), −1 = unknown
    int32_t      ledChannel;   // base LED channel, −1 = unknown/none
    bool         rgb;          // 3 channels R,G,B at ledChannel (else mono)
};

/// Every button-like control we know of (pads are separate — see below).
std::span<const ControlInfo> controls();

/// The control a switch event belongs to; nullptr if unmapped.
const ControlInfo * controlForButton(uint32_t buttonId);

/// Pads arrive as pad events, not switches. Maps the wire padId to the pad
/// index 0–15 (pad 1–16 in Studio speak); −1 if out of range.
int padIndexForInputId(uint32_t padId);

/// The inverse: wire padId for a pad index 0–15; −1 if out of range.
int padInputIdForIndex(int pad);

/// Knob-event id space: ids 0–7 are the eight screen knobs; id 8 is the big
/// MASTER level encoder in the meter section (endless, continuous deltas —
/// old tsv hint "cue … knob 8", confirmed live 2026-07-22). It has no LED
/// and, unlike the screen knobs, no touch cap (verified live).
constexpr uint32_t kMasterKnobId = 8;

/// Best-effort human name for any LED channel ("Play", "Pad 3: Green",
/// "Left VU seg 7", …) for the poke side of the probe tool.
std::string describeChannel(size_t channel);

} // namespace macchina::studio
