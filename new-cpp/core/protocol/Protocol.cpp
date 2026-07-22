//
//  Protocol.cpp
//  Macchina — new-cpp/core/protocol
//

#include "core/protocol/Protocol.hpp"

namespace macchina::nhl2 {

void appendU32(Frame & frame, uint32_t value)
{
    frame.push_back((uint8_t)(value));
    frame.push_back((uint8_t)(value >> 8));
    frame.push_back((uint8_t)(value >> 16));
    frame.push_back((uint8_t)(value >> 24));
}

std::optional<uint32_t> readU32(const Frame & frame, size_t offset)
{
    if (offset + 4 > frame.size())
        return std::nullopt;
    return (uint32_t)frame[offset]
         | (uint32_t)frame[offset + 1] << 8
         | (uint32_t)frame[offset + 2] << 16
         | (uint32_t)frame[offset + 3] << 24;
}

std::optional<uint32_t> messageId(const Frame & frame)
{
    return readU32(frame, 0);
}

PortPairReply PortPairReply::parse(const Frame & reply)
{
    PortPairReply result;

    if (readU32(reply, 0) != kReplyTrue)
        return result;   // 0x00000000 (or anything else) = refused

    // [u32 'true'][u32 inLen][inName+NUL][u32 outLen][outName+NUL]
    // The length fields count the NUL terminator.
    auto inLen = readU32(reply, 4);
    if (!inLen || *inLen == 0 || 8 + *inLen + 4 > reply.size())
        return result;

    auto outLen = readU32(reply, 8 + *inLen);
    if (!outLen || *outLen == 0 || 12 + *inLen + *outLen > reply.size())
        return result;

    result.requestPortName.assign(reply.begin() + 8,
                                  reply.begin() + 8 + *inLen - 1);
    result.notificationPortName.assign(reply.begin() + 12 + *inLen,
                                       reply.begin() + 12 + *inLen + *outLen - 1);
    result.success = true;
    return result;
}

std::optional<std::string> parseLengthPrefixedString(const Frame & reply)
{
    auto len = readU32(reply, 0);
    if (!len || *len == 0 || 4 + *len > reply.size())
        return std::nullopt;
    // len counts the NUL terminator.
    return std::string(reply.begin() + 4, reply.begin() + 4 + *len - 1);
}

} // namespace macchina::nhl2
