//
//  HexDump.cpp
//  Macchina — new-cpp/core/transport
//

#include "core/transport/HexDump.hpp"

#include <cstdio>

namespace macchina {

std::string hexDump(const uint8_t * bytes, size_t length)
{
    std::string out;
    out.reserve(length * 4);

    char buf[16];

    for (size_t row = 0; row < length; row += 16)
    {
        snprintf(buf, sizeof(buf), "%08zx  ", row);
        out += buf;

        // Hex columns, split into two groups of 8 for readability.
        for (size_t col = 0; col < 16; col++)
        {
            if (row + col < length)
            {
                snprintf(buf, sizeof(buf), "%02x ", bytes[row + col]);
                out += buf;
            }
            else
                out += "   ";

            if (col == 7)
                out += " ";
        }

        // ASCII gutter.
        out += " |";
        for (size_t col = 0; col < 16 && row + col < length; col++)
        {
            uint8_t c = bytes[row + col];
            out += (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        out += "|\n";
    }

    return out;
}

std::string hexDump(const Frame & data)
{
    return hexDump(data.data(), data.size());
}

std::string describeFrame(const Frame & data)
{
    char buf[64];

    if (data.size() < 4)
    {
        snprintf(buf, sizeof(buf), "(short frame, %zu bytes)\n", data.size());
        return buf + hexDump(data);
    }

    uint32_t messageId = (uint32_t)data[0]
                       | (uint32_t)data[1] << 8
                       | (uint32_t)data[2] << 16
                       | (uint32_t)data[3] << 24;

    // Little-endian message-id tag as a 4CC (printable bytes), for eyeballing.
    char tag[5] = {0};
    for (int i = 0; i < 4; i++)
    {
        uint8_t c = (uint8_t)(messageId >> (i * 8));
        tag[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }

    snprintf(buf, sizeof(buf), "%08x  '%s'  (%zu bytes)\n", messageId, tag, data.size());
    return buf + hexDump(data);
}

std::string describeFrameBounded(const Frame & data)
{
    static const size_t kMaxDumpedBytes = 128;

    if (data.empty())
        return "(empty)";
    if (data.size() <= kMaxDumpedBytes)
        return describeFrame(data);

    Frame head(data.begin(), data.begin() + kMaxDumpedBytes);
    char buf[80];
    snprintf(buf, sizeof(buf), "         … (%zu bytes total, first %zu shown)",
             data.size(), kMaxDumpedBytes);
    return describeFrame(head) + buf;
}

} // namespace macchina
