//
//  Frame.hpp
//  Macchina — new-cpp/core
//
//  The one type every layer shares: a wire message, verbatim. The first
//  4 bytes are the little-endian messageId (see core/protocol/).
//

#pragma once

#include <cstdint>
#include <vector>

namespace macchina {

using Frame = std::vector<uint8_t>;

} // namespace macchina
