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
    if (t == "cas") return Verb::cas;
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
    std::array<std::string_view, 7> tok{}; // cas needs up to 7: cas <key> <f> <e> <b> <cas> [noreply]
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
        case Verb::cas: // cas <key> <flags> <exptime> <bytes> <cas> [noreply]
            if (ntok < 6)
                return err(Errc::invalid_protocol, "cas needs <key> <flags> <exptime> <bytes> <cas>");
            c.key = tok[1];
            if (!to_int(tok[2], c.flags) || !to_int(tok[3], c.exptime) || !to_int(tok[4], c.bytes) ||
                !to_int(tok[5], c.cas))
                return err(Errc::invalid_protocol, "bad numeric field");
            c.noreply = (ntok >= 7 && tok[6] == "noreply");
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

Result<MetaCommand> parse_meta(std::string_view line) {
    std::size_t i = 0;
    auto next = [&]() -> std::string_view {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t s = i;
        while (i < line.size() && line[i] != ' ') ++i;
        return line.substr(s, i - s);
    };
    MetaCommand m;
    const std::string_view v = next();
    if (v == "mn") { m.verb = MetaVerb::mn; return m; } // no key/flags
    else if (v == "mg") m.verb = MetaVerb::mg;
    else if (v == "ms") m.verb = MetaVerb::ms;
    else if (v == "md") m.verb = MetaVerb::md;
    else return err(Errc::invalid_protocol, "unknown meta command");

    m.key = next();
    if (m.key.empty()) return err(Errc::invalid_protocol, "meta needs a key");
    if (m.verb == MetaVerb::ms && !to_int(next(), m.datalen))
        return err(Errc::invalid_protocol, "ms needs <datalen>");

    for (std::string_view t = next(); !t.empty(); t = next()) {
        const char c = t[0];
        const std::string_view arg = t.substr(1);
        switch (c) {
            case 'v': m.rf_value = true; break;
            case 'f': m.rf_flags = true; break;
            case 's': m.rf_size = true; break;
            case 't': m.rf_ttl = true; break;
            case 'c': m.rf_cas = true; break;
            case 'k': m.rf_key = true; break;
            case 'q': m.quiet = true; break;
            case 'F': if (!to_int(arg, m.set_flags)) return err(Errc::invalid_protocol, "bad F"); m.has_set_flags = true; break;
            case 'T': if (!to_int(arg, m.ttl)) return err(Errc::invalid_protocol, "bad T"); m.has_ttl = true; break;
            case 'C': if (!to_int(arg, m.cas)) return err(Errc::invalid_protocol, "bad C"); m.has_cas = true; break;
            case 'M': if (arg.empty()) return err(Errc::invalid_protocol, "bad M"); m.mode = arg[0]; break;
            case 'O': m.opaque = arg; break;
            default: return err(Errc::invalid_protocol, "unsupported meta flag");
        }
    }
    return m;
}

std::uint32_t exptime_to_expiry(std::uint32_t exptime, std::uint32_t now) {
    constexpr std::uint32_t kRelativeMax = 60u * 60u * 24u * 30u; // 30 days (memcache convention)
    if (exptime == 0) return 0;                 // never expires
    if (exptime > kRelativeMax) return exptime; // already an absolute Unix timestamp
    return now + exptime;                       // relative to now
}

std::string value_header(std::string_view key, std::uint32_t flags, std::uint64_t bytes) {
    return std::format("VALUE {} {} {}\r\n", key, flags, bytes);
}

std::string value_header_cas(std::string_view key, std::uint32_t flags, std::uint64_t bytes,
                             std::uint64_t cas) {
    return std::format("VALUE {} {} {} {}\r\n", key, flags, bytes, cas);
}

} // namespace goblin::memcache
