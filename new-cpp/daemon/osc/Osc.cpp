//
//  Osc.cpp
//  Macchina — new-cpp/osc
//

#include "daemon/osc/Osc.hpp"

#include <cstring>

namespace macchina::osc {

namespace {

// OSC strings include a NUL and pad the TOTAL to a multiple of 4.
void appendString(std::vector<uint8_t> & out, const std::string & s)
{
    out.insert(out.end(), s.begin(), s.end());
    size_t total = s.size() + 1;                    // + NUL
    out.insert(out.end(), 4 - ((total - 1) % 4), 0);
}

void appendBE32(std::vector<uint8_t> & out, uint32_t v)
{
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}

std::optional<uint32_t> readBE32(const uint8_t * data, size_t length, size_t & cursor)
{
    if (cursor + 4 > length)
        return std::nullopt;
    uint32_t v = (uint32_t)data[cursor] << 24 | (uint32_t)data[cursor + 1] << 16
               | (uint32_t)data[cursor + 2] << 8 | (uint32_t)data[cursor + 3];
    cursor += 4;
    return v;
}

std::optional<std::string> readPaddedString(const uint8_t * data, size_t length, size_t & cursor)
{
    const auto * nul = (const uint8_t *)memchr(data + cursor, 0, length - cursor);
    if (nul == nullptr)
        return std::nullopt;
    std::string s((const char *)data + cursor, (size_t)(nul - data - cursor));
    cursor += ((s.size() + 1 + 3) / 4) * 4;   // string + NUL, padded to 4
    if (cursor > length)
        return std::nullopt;
    return s;
}

} // namespace


std::optional<Message> Message::parse(const uint8_t * data, size_t length)
{
    size_t cursor = 0;

    Message msg;
    auto address = readPaddedString(data, length, cursor);
    if (!address || address->empty() || (*address)[0] != '/')
        return std::nullopt;
    msg.address = *address;

    auto tags = readPaddedString(data, length, cursor);
    if (!tags || tags->empty() || (*tags)[0] != ',')
        return std::nullopt;

    for (size_t t = 1; t < tags->size(); t++)
    {
        Value v;
        v.tag = (*tags)[t];
        switch (v.tag)
        {
            case 'i':
            {
                auto raw = readBE32(data, length, cursor);
                if (!raw) return std::nullopt;
                v.i = (int32_t)*raw;
                break;
            }
            case 'f':
            {
                auto raw = readBE32(data, length, cursor);
                if (!raw) return std::nullopt;
                uint32_t bits = *raw;
                memcpy(&v.f, &bits, sizeof(v.f));
                break;
            }
            case 's':
            {
                auto s = readPaddedString(data, length, cursor);
                if (!s) return std::nullopt;
                v.s = *s;
                break;
            }
            case 'b':
            {
                auto n = readBE32(data, length, cursor);
                if (!n || cursor + *n > length) return std::nullopt;
                v.b.assign(data + cursor, data + cursor + *n);
                cursor += ((*n + 3) / 4) * 4;
                break;
            }
            case 'T': v.tag = 'i'; v.i = 1; break;   // fold booleans into ints
            case 'F': v.tag = 'i'; v.i = 0; break;
            case 'N': break;
            default:  return std::nullopt;           // unknown tag — refuse whole packet
        }
        msg.args.push_back(std::move(v));
    }
    return msg;
}

std::vector<uint8_t> Message::encode() const
{
    std::vector<uint8_t> out;
    appendString(out, address);

    std::string tags = ",";
    for (const auto & a : args)
        tags += a.tag;
    appendString(out, tags);

    for (const auto & a : args)
        switch (a.tag)
        {
            case 'i': appendBE32(out, (uint32_t)a.i); break;
            case 'f':
            {
                uint32_t bits;
                memcpy(&bits, &a.f, sizeof(bits));
                appendBE32(out, bits);
                break;
            }
            case 's': appendString(out, a.s); break;
            case 'b':
                appendBE32(out, (uint32_t)a.b.size());
                out.insert(out.end(), a.b.begin(), a.b.end());
                out.insert(out.end(), (4 - a.b.size() % 4) % 4, 0);
                break;
        }
    return out;
}

std::optional<int32_t> Message::intAt(size_t index) const
{
    if (index >= args.size())
        return std::nullopt;
    const auto & a = args[index];
    if (a.tag == 'i') return a.i;
    if (a.tag == 'f') return (int32_t)a.f;
    return std::nullopt;
}

std::optional<float> Message::floatAt(size_t index) const
{
    if (index >= args.size())
        return std::nullopt;
    const auto & a = args[index];
    if (a.tag == 'f') return a.f;
    if (a.tag == 'i') return (float)a.i;
    return std::nullopt;
}

std::optional<std::string> Message::stringAt(size_t index) const
{
    if (index >= args.size() || args[index].tag != 's')
        return std::nullopt;
    return args[index].s;
}

Message makeMessage(std::string address, std::vector<Value> args)
{
    Message m;
    m.address = std::move(address);
    m.args    = std::move(args);
    return m;
}

} // namespace macchina::osc
