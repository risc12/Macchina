//
//  Osc.hpp
//  Macchina — new-cpp/osc
//
//  Minimal OSC 1.0 codec: plain messages with i/f/s/b arguments. Pure data —
//  no sockets, no platform. Bundles ("#bundle") are not supported; the
//  daemon's clients send plain messages. Wire format: address (NUL-padded to
//  4), ","+typetags (padded), then big-endian args.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace macchina::osc {

struct Value
{
    char                 tag;   // 'i', 'f', 's', 'b'
    int32_t              i = 0;
    float                f = 0;
    std::string          s;
    std::vector<uint8_t> b;

    static Value ofInt(int32_t v)             { Value x; x.tag = 'i'; x.i = v; return x; }
    static Value ofFloat(float v)             { Value x; x.tag = 'f'; x.f = v; return x; }
    static Value ofString(std::string v)      { Value x; x.tag = 's'; x.s = std::move(v); return x; }
};

struct Message
{
    std::string        address;
    std::vector<Value> args;

    /// nullopt on malformed packets (never throws — packets come off a socket).
    static std::optional<Message> parse(const uint8_t * data, size_t length);

    std::vector<uint8_t> encode() const;

    // Convenience accessors: type-checked, nullopt when absent/mismatched.
    std::optional<int32_t>     intAt(size_t index) const;
    std::optional<float>       floatAt(size_t index) const;   // accepts 'i' too
    std::optional<std::string> stringAt(size_t index) const;
};

/// Build a message in one line: makeMessage("/led", {Value::ofInt(3), …}).
Message makeMessage(std::string address, std::vector<Value> args);

} // namespace macchina::osc
