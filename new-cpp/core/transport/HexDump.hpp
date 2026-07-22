//
//  HexDump.hpp
//  Macchina — new-cpp/core/transport
//
//  Frame logging helpers, platform-agnostic. Port of new/core/NIHexDump.
//

#pragma once

#include "core/Frame.hpp"

#include <string>

namespace macchina {

/// Classic offset / hex / ASCII-gutter dump.
std::string hexDump(const uint8_t * bytes, size_t length);
std::string hexDump(const Frame & data);

/// hexDump prefixed with the messageId + its bytes read as a 4CC, for eyeballing.
std::string describeFrame(const Frame & data);

/// describeFrame, but large frames (display draws are ~256 KiB) are truncated
/// to the first 128 bytes so they don't drown the log.
std::string describeFrameBounded(const Frame & data);

} // namespace macchina
