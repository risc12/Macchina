//
//  Protocol.hpp
//  Macchina — new-cpp/core/protocol
//
//  NHL2 v3 wire vocabulary: message tags, well-known 4CCs, frame
//  building/reading helpers, and the parsed reply shapes. Pure data — no
//  transport, no platform. See new/docs/protocol.md for the ground truth.
//

#pragma once

#include "core/Frame.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace macchina::nhl2 {

// ---- 4CCs -------------------------------------------------------------------
// Stored as big-endian-reading constants ('true' == 0x74727565) and serialized
// little-endian, which reproduces the exact wire bytes the ObjC code sent.

constexpr uint32_t fourcc(const char (&s)[5])
{
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16
         | (uint32_t)(uint8_t)s[2] << 8  | (uint32_t)(uint8_t)s[3];
}

constexpr uint32_t kClientTagMaschine2 = fourcc("NiM2");   // Maschine 2 software
constexpr uint32_t kClientTagMaschine1 = fourcc("NiMS");   // Maschine 1 software
constexpr uint32_t kClientRolePrimary  = fourcc("prmy");
constexpr uint32_t kReplyTrue          = fourcc("true");
constexpr uint32_t kBodyStart          = fourcc("strt");   // ControllerAcquire body

// ---- Message tags -----------------------------------------------------------
// 3-byte tags; dispatch is on messageId & 0xffffff. The top byte is the
// negotiated protocol version (0x02 request-era, 0x03 = v3).

enum Tag : uint32_t
{
    kTagGetServiceVersion  = 0x536756,
    kTagDeviceSubscribe    = 0x447500,   // "Du"
    kTagDeviceConnect      = 0x444900,
    kTagSetAsciiString     = 0x404300,
    kTagSubscribeVoidOp    = 0x447143,
    kTagControllerAcquire  = 0x434300,   // body 'strt' → claim display+input focus
    kTagGetSerialNumber    = 0x436753,
    kTagGetFirmwareVersion = 0x436746,
    kTagGetDriverVersion   = 0x446744,
    kTagDisplayDraw        = 0x647344,   // "Dsd"
    kTagSetLedState        = 0x6c7500,
};

// ---- Frame building / reading (all little-endian on the wire) --------------

void appendU32(Frame & frame, uint32_t value);

/// Read a u32 at `offset`; nullopt if out of bounds.
std::optional<uint32_t> readU32(const Frame & frame, size_t offset);

/// The messageId of a frame (first 4 bytes); nullopt on short frames.
std::optional<uint32_t> messageId(const Frame & frame);

// ---- Reply shapes -----------------------------------------------------------

/// The connect/subscribe reply: 'true' + a request/notification port-name
/// pair (layout decoded in new/core/NIResponse.m — [u32 'true'][u32 inLen]
/// [inName+NUL][u32 outLen][outName+NUL]).
struct PortPairReply
{
    bool        success = false;
    std::string requestPortName;        // agent-owned; we send here
    std::string notificationPortName;   // ours to create; agent pushes here

    static PortPairReply parse(const Frame & reply);
};

/// A [u32 length][ASCII + NUL] string reply (GetSerialNumber et al).
std::optional<std::string> parseLengthPrefixedString(const Frame & reply);

} // namespace macchina::nhl2
