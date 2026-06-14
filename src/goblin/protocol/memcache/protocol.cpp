#include "goblin/protocol/memcache/protocol.hpp"

#include <array>
#include <charconv>
#include <format>

namespace goblin::memcache {
namespace {

Verb verb_of(std::string_view t) {
    if (t == "get") return Verb::get;
    if (t == "gets") return Verb::gets;
    if (t == "set") return Verb::set;
    if (t == "add") return Verb::add;
    if (t == "replace") return Verb::replace;
    if (t == "delete") return Verb::del;
    if (t == "version") return Verb::version;
    if (t == "quit") return Verb::quit;
    if (t == "stats") return Verb::stats;
    return Verb::unknown;
}

template <class T>
bool to_int(std::string_view s, T& out) {
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && p == end;
}

} // namespace

Result<Command> parse_command(std::string_view line) {
    std::array<std::string_view, 6> tok{};
    std::size_t ntok = 0;
    std::size_t i = 0;
    while (i < line.size() && ntok < tok.size()) {
        while (i < line.size() && line[i] == ' ') ++i; // skip spaces
        if (i >= line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        tok[ntok++] = line.substr(start, i - start);
    }
    if (ntok == 0) return err(Errc::invalid_protocol, "empty command");

    Command c;
    c.verb = verb_of(tok[0]);
    switch (c.verb) {
        case Verb::get:
        case Verb::gets:
            if (ntok < 2) return err(Errc::invalid_protocol, "get needs a key");
            c.key = tok[1];
            return c;
        case Verb::del:
            if (ntok < 2) return err(Errc::invalid_protocol, "delete needs a key");
            c.key = tok[1];
            c.noreply = (ntok >= 3 && tok[2] == "noreply");
            return c;
        case Verb::set:
        case Verb::add:
        case Verb::replace:
            if (ntok < 5)
                return err(Errc::invalid_protocol, "storage needs <key> <flags> <exptime> <bytes>");
            c.key = tok[1];
            if (!to_int(tok[2], c.flags) || !to_int(tok[3], c.exptime) || !to_int(tok[4], c.bytes))
                return err(Errc::invalid_protocol, "bad numeric field");
            c.noreply = (ntok >= 6 && tok[5] == "noreply");
            return c;
        case Verb::version:
        case Verb::quit:
        case Verb::stats: // optional sub-command (e.g. `stats items`) is accepted but ignored (v1)
            return c;
        case Verb::unknown:
            return err(Errc::invalid_protocol, "unknown command");
    }
    return err(Errc::invalid_protocol, "unhandled command");
}

std::string value_header(std::string_view key, std::uint32_t flags, std::uint64_t bytes) {
    return std::format("VALUE {} {} {}\r\n", key, flags, bytes);
}

} // namespace goblin::memcache
