#include "goblin/common/error.hpp"

namespace goblin {

std::string_view to_string(Errc c) noexcept {
    switch (c) {
        case Errc::ok:                   return "ok";
        case Errc::not_found:            return "not_found";
        case Errc::already_exists:       return "already_exists";
        case Errc::cas_mismatch:         return "cas_mismatch";
        case Errc::out_of_space:         return "out_of_space";
        case Errc::out_of_memory:        return "out_of_memory";
        case Errc::too_large:            return "too_large";
        case Errc::invalid_argument:     return "invalid_argument";
        case Errc::invalid_protocol:     return "invalid_protocol";
        case Errc::io_error:             return "io_error";
        case Errc::unsupported:          return "unsupported";
        case Errc::backpressure_timeout: return "backpressure_timeout";
        case Errc::would_block:          return "would_block";
    }
    return "unknown";
}

} // namespace goblin
