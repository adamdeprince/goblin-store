#include "goblin/protocol/memcache/protocol.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace goblin::memcache {
namespace {

Verb verb_of(std::string_view t) {
    // Length + first-char filter before full compare (small-object parse path).
    switch (t.size()) {
        case 3:
            if (t[0] == 'g' && t == "get") return Verb::get;
            if (t[0] == 'g' && t == "gat") return Verb::gat;
            if (t[0] == 's' && t == "set") return Verb::set;
            if (t[0] == 'a' && t == "add") return Verb::add;
            if (t[0] == 'c' && t == "cas") return Verb::cas;
            break;
        case 4:
            if (t[0] == 'g' && t == "gets") return Verb::gets;
            if (t[0] == 'g' && t == "gats") return Verb::gats;
            if (t[0] == 'i' && t == "incr") return Verb::incr;
            if (t[0] == 'd' && t == "decr") return Verb::decr;
            if (t[0] == 'q' && t == "quit") return Verb::quit;
            break;
        case 5:
            if (t[0] == 's' && t == "stats") return Verb::stats;
            if (t[0] == 't' && t == "touch") return Verb::touch;
            break;
        case 6:
            if (t[0] == 'd' && t == "delete") return Verb::del;
            if (t[0] == 'a' && t == "append") return Verb::append;
            break;
        case 7:
            if (t[0] == 'r' && t == "replace") return Verb::replace;
            if (t[0] == 'v' && t == "version") return Verb::version;
            if (t[0] == 'p' && t == "prepend") return Verb::prepend;
            break;
        case 9:
            if (t[0] == 'f' && t == "flush_all") return Verb::flush_all;
            break;
        default:
            break;
    }
    return Verb::unknown;
}

template <class T>
bool to_int(std::string_view s, T& out) {
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && p == end;
}

bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > 250) return false;
    for (const unsigned char ch : key)
        if (ch <= 0x20 || ch == 0x7f) return false; // ASCII control characters and whitespace
    return true;
}

bool valid_meta_key_token(std::string_view key) {
    if (key.empty() || key.size() > 344) return false; // ceil(250/3)*4 plus modest padding room
    for (const unsigned char ch : key)
        if (ch <= 0x20 || ch == 0x7f) return false;
    return true;
}

Result<std::string> decode_base64(std::string_view encoded) {
    auto digit = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    if (encoded.empty() || encoded.size() % 4 == 1)
        return err(Errc::invalid_protocol, "bad base64 key");
    std::string out;
    out.reserve((encoded.size() * 3) / 4);
    std::uint32_t bits = 0;
    unsigned nbits = 0;
    bool padding = false;
    unsigned padding_count = 0;
    for (const unsigned char ch : encoded) {
        if (ch == '=') {
            padding = true;
            ++padding_count;
            if (padding_count > 2) return err(Errc::invalid_protocol, "bad base64 key");
            continue;
        }
        if (padding) return err(Errc::invalid_protocol, "bad base64 key");
        const int value = digit(ch);
        if (value < 0) return err(Errc::invalid_protocol, "bad base64 key");
        bits = (bits << 6) | static_cast<std::uint32_t>(value);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<char>((bits >> nbits) & 0xff));
        }
    }
    if (padding_count != 0 && encoded.size() % 4 != 0)
        return err(Errc::invalid_protocol, "bad base64 key");
    if ((padding_count == 1 && nbits != 2) || (padding_count == 2 && nbits != 4) ||
        (nbits != 0 && (bits & ((std::uint32_t{1} << nbits) - 1)) != 0))
        return err(Errc::invalid_protocol, "bad base64 key");
    const std::size_t expected = (encoded.size() / 4) * 3 - padding_count +
                                 (encoded.size() % 4 == 2 ? 1 : encoded.size() % 4 == 3 ? 2 : 0);
    if (out.size() != expected || out.empty() || out.size() > 250)
        return err(Errc::invalid_protocol, "bad base64 key");
    return out;
}

void append_u64(std::string& out, std::uint64_t v) {
    char buf[24];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(p - buf));
}

void append_u32(std::string& out, std::uint32_t v) {
    char buf[12];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(p - buf));
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
    const auto has_more_tokens = [&] {
        std::size_t p = i;
        while (p < line.size() && line[p] == ' ') ++p;
        return p != line.size();
    };
    if (ntok == 0) return err(Errc::invalid_protocol, "empty command");

    Command c;
    c.verb = verb_of(tok[0]);
    const auto collect_keys = [&](std::size_t first) -> Status {
        if (ntok <= first) return err(Errc::invalid_protocol, "retrieval needs a key");
        c.key = tok[first];
        if (!valid_key(c.key)) return err(Errc::invalid_protocol, "invalid key");
        for (std::size_t j = first + 1; j < ntok; ++j) {
            if (!valid_key(tok[j])) return err(Errc::invalid_protocol, "invalid key");
            c.extra_keys.push_back(tok[j]);
        }
        while (i < line.size()) {
            while (i < line.size() && line[i] == ' ') ++i;
            if (i >= line.size()) break;
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ') ++i;
            const auto key = line.substr(start, i - start);
            if (!valid_key(key)) return err(Errc::invalid_protocol, "invalid key");
            c.extra_keys.push_back(key);
        }
        return {};
    };
    switch (c.verb) {
        case Verb::get:
        case Verb::gets:
            if (auto status = collect_keys(1); !status) return std::unexpected(status.error());
            return c;
        case Verb::gat:
        case Verb::gats:
            if (ntok < 3 || !to_int(tok[1], c.exptime) ||
                c.exptime > std::numeric_limits<std::uint32_t>::max())
                return err(Errc::invalid_protocol, "gat needs <exptime> <key>...");
            if (auto status = collect_keys(2); !status) return std::unexpected(status.error());
            return c;
        case Verb::del:
            if (ntok < 2 || ntok > 3 || has_more_tokens())
                return err(Errc::invalid_protocol, "delete needs <key> [noreply]");
            c.key = tok[1];
            if (!valid_key(c.key)) return err(Errc::invalid_protocol, "invalid key");
            if (ntok == 3 && tok[2] != "noreply")
                return err(Errc::invalid_protocol, "invalid delete option");
            c.noreply = ntok == 3;
            return c;
        case Verb::set:
        case Verb::add:
        case Verb::replace:
        case Verb::append:
        case Verb::prepend:
            if (ntok < 5 || ntok > 6 || has_more_tokens())
                return err(Errc::invalid_protocol, "storage needs <key> <flags> <exptime> <bytes>");
            c.key = tok[1];
            if (!valid_key(c.key)) return err(Errc::invalid_protocol, "invalid key");
            if (!to_int(tok[2], c.flags) || !to_int(tok[3], c.exptime) || !to_int(tok[4], c.bytes))
                return err(Errc::invalid_protocol, "bad numeric field");
            if (c.exptime > std::numeric_limits<std::uint32_t>::max())
                return err(Errc::invalid_protocol, "exptime is out of range");
            if (ntok == 6 && tok[5] != "noreply")
                return err(Errc::invalid_protocol, "invalid storage option");
            c.noreply = ntok == 6;
            return c;
        case Verb::touch:
            if (ntok < 3 || ntok > 4 || has_more_tokens())
                return err(Errc::invalid_protocol, "touch needs <key> <exptime> [noreply]");
            c.key = tok[1];
            if (!valid_key(c.key) || !to_int(tok[2], c.exptime) ||
                c.exptime > std::numeric_limits<std::uint32_t>::max())
                return err(Errc::invalid_protocol, "bad touch command");
            if (ntok == 4 && tok[3] != "noreply")
                return err(Errc::invalid_protocol, "invalid touch option");
            c.noreply = ntok == 4;
            return c;
        case Verb::incr:
        case Verb::decr:
            if (ntok < 3 || ntok > 4 || has_more_tokens())
                return err(Errc::invalid_protocol, "arithmetic needs <key> <delta> [noreply]");
            c.key = tok[1];
            if (!valid_key(c.key) || !to_int(tok[2], c.delta))
                return err(Errc::invalid_protocol, "bad arithmetic command");
            if (ntok == 4 && tok[3] != "noreply")
                return err(Errc::invalid_protocol, "invalid arithmetic option");
            c.noreply = ntok == 4;
            return c;
        case Verb::flush_all:
            if (ntok > 3 || has_more_tokens())
                return err(Errc::invalid_protocol, "flush_all takes [delay] [noreply]");
            if (ntok >= 2 && tok[1] != "noreply" && !to_int(tok[1], c.delay))
                return err(Errc::invalid_protocol, "bad flush_all delay");
            if (ntok == 2) c.noreply = tok[1] == "noreply";
            if (ntok == 3) {
                if (tok[2] != "noreply" || tok[1] == "noreply")
                    return err(Errc::invalid_protocol, "bad flush_all option");
                c.noreply = true;
            }
            return c;
        case Verb::cas: // cas <key> <flags> <exptime> <bytes> <cas> [noreply]
            if (ntok < 6 || ntok > 7 || has_more_tokens())
                return err(Errc::invalid_protocol, "cas needs <key> <flags> <exptime> <bytes> <cas>");
            c.key = tok[1];
            if (!valid_key(c.key)) return err(Errc::invalid_protocol, "invalid key");
            if (!to_int(tok[2], c.flags) || !to_int(tok[3], c.exptime) || !to_int(tok[4], c.bytes) ||
                !to_int(tok[5], c.cas))
                return err(Errc::invalid_protocol, "bad numeric field");
            if (c.exptime > std::numeric_limits<std::uint32_t>::max())
                return err(Errc::invalid_protocol, "exptime is out of range");
            if (ntok == 7 && tok[6] != "noreply")
                return err(Errc::invalid_protocol, "invalid cas option");
            c.noreply = ntok == 7;
            return c;
        case Verb::version:
        case Verb::quit:
            if (ntok != 1 || has_more_tokens())
                return err(Errc::invalid_protocol, "command takes no arguments");
            return c;
        case Verb::stats:
            if (has_more_tokens() || ntok > 2)
                return err(Errc::invalid_protocol, "bad stats command");
            if (ntok == 1) return c;
            if (tok[1] == "reset") {
                c.stats_verb = StatsVerb::reset;
                return c;
            }
            if (tok[1] == "settings") {
                c.stats_verb = StatsVerb::settings;
                return c;
            }
            // Goblin has no memcached slab classes, so class-specific item variants remain
            // unsupported rather than returning unrelated process statistics.
            return err(Errc::unsupported, "unsupported stats command");
        case Verb::unknown:
            return err(Errc::unsupported, "unknown command");
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
    if (v == "mn") {
        m.verb = MetaVerb::mn;
        if (!next().empty()) return err(Errc::invalid_protocol, "mn takes no arguments");
        return m;
    }
    else if (v == "mg") m.verb = MetaVerb::mg;
    else if (v == "ms") m.verb = MetaVerb::ms;
    else if (v == "md") m.verb = MetaVerb::md;
    else if (v == "ma") { m.verb = MetaVerb::ma; m.mode = 'I'; }
    else if (v == "me") m.verb = MetaVerb::me;
    else return err(Errc::invalid_protocol, "unknown meta command");

    m.key = next();
    if (!valid_meta_key_token(m.key)) return err(Errc::invalid_protocol, "invalid meta key");
    if (m.verb == MetaVerb::ms && !to_int(next(), m.datalen))
        return err(Errc::invalid_protocol, "ms needs <datalen>");

    for (std::string_view t = next(); !t.empty(); t = next()) {
        const char c = t[0];
        const std::string_view arg = t.substr(1);
        switch (c) {
            case 'v': if (!arg.empty()) return err(Errc::invalid_protocol, "bad v"); m.rf_value = true; m.return_order += 'v'; break;
            case 'f': if (!arg.empty()) return err(Errc::invalid_protocol, "bad f"); m.rf_flags = true; m.return_order += 'f'; break;
            case 's': if (!arg.empty()) return err(Errc::invalid_protocol, "bad s"); m.rf_size = true; m.return_order += 's'; break;
            case 't': if (!arg.empty()) return err(Errc::invalid_protocol, "bad t"); m.rf_ttl = true; m.return_order += 't'; break;
            case 'c': if (!arg.empty()) return err(Errc::invalid_protocol, "bad c"); m.rf_cas = true; m.return_order += 'c'; break;
            case 'k': if (!arg.empty()) return err(Errc::invalid_protocol, "bad k"); m.rf_key = true; m.return_order += 'k'; break;
            case 'h': if (!arg.empty()) return err(Errc::invalid_protocol, "bad h"); m.rf_hit = true; m.return_order += 'h'; break;
            case 'l': if (!arg.empty()) return err(Errc::invalid_protocol, "bad l"); m.rf_last_access = true; m.return_order += 'l'; break;
            case 'q': if (!arg.empty()) return err(Errc::invalid_protocol, "bad q"); m.quiet = true; break;
            case 'u': if (!arg.empty()) return err(Errc::invalid_protocol, "bad u"); m.no_update = true; break;
            case 'b': if (!arg.empty()) return err(Errc::invalid_protocol, "bad b"); m.key_binary = true; break;
            case 'F': if (!to_int(arg, m.set_flags)) return err(Errc::invalid_protocol, "bad F"); m.has_set_flags = true; break;
            case 'T':
                if (!to_int(arg, m.ttl) ||
                    m.ttl > std::numeric_limits<std::uint32_t>::max())
                    return err(Errc::invalid_protocol, "bad T");
                m.has_ttl = true;
                break;
            case 'C': if (!to_int(arg, m.cas)) return err(Errc::invalid_protocol, "bad C"); m.has_cas = true; break;
            case 'M': if (arg.size() != 1) return err(Errc::invalid_protocol, "bad M"); m.mode = arg[0]; break;
            case 'O': m.opaque = arg; m.return_order += 'O'; break;
            case 'N':
                if (!to_int(arg, m.vivify_ttl) || m.vivify_ttl < 0 ||
                    m.vivify_ttl > std::numeric_limits<std::uint32_t>::max())
                    return err(Errc::invalid_protocol, "bad N");
                m.has_vivify = true;
                m.return_order += 'N';
                break;
            case 'R': if (!to_int(arg, m.recache_ttl)) return err(Errc::invalid_protocol, "bad R"); m.has_recache = true; m.return_order += 'R'; break;
            case 'I': if (!arg.empty()) return err(Errc::invalid_protocol, "bad I"); m.invalidate = true; break;
            case 'x': if (!arg.empty()) return err(Errc::invalid_protocol, "bad x"); m.remove_value = true; break;
            case 'J': if (!to_int(arg, m.initial)) return err(Errc::invalid_protocol, "bad J"); m.has_initial = true; break;
            case 'D': if (!to_int(arg, m.delta)) return err(Errc::invalid_protocol, "bad D"); break;
            case 'P':
            case 'L': break; // proxy-routing hints: origin servers intentionally ignore them
            default: return err(Errc::invalid_protocol, "unsupported meta flag");
        }
    }
    if (m.opaque.size() > 32) return err(Errc::invalid_protocol, "opaque token too long");
    if (m.key_binary) {
        auto decoded = decode_base64(m.key);
        if (!decoded) return std::unexpected(decoded.error());
        m.decoded_key = std::move(*decoded);
    } else if (!valid_key(m.key)) {
        return err(Errc::invalid_protocol, "invalid meta key");
    }
    if (m.verb == MetaVerb::ma && m.has_initial && !m.has_vivify)
        return err(Errc::invalid_protocol, "J requires N");
    return m;
}

std::uint32_t exptime_to_expiry(std::int64_t exptime, std::uint32_t now) {
    constexpr std::uint32_t kRelativeMax = 60u * 60u * 24u * 30u; // 30 days (memcache convention)
    if (exptime < 0) return 1;                    // classic protocol: immediately expired
    if (exptime == 0) return 0;                 // never expires
    const auto positive = static_cast<std::uint32_t>(exptime);
    if (positive > kRelativeMax) return positive; // already an absolute Unix timestamp
    return now + positive;                        // relative to now
}

std::string_view command_parse_error_reply(const Error& error) noexcept {
    return error.code == Errc::unsupported ? kError : kClientErrorBadCommand;
}

std::string_view storage_failure_reply(Errc code) noexcept {
    switch (code) {
        case Errc::too_large: return kServerErrorTooLarge;
        case Errc::out_of_space: return kServerErrorOutOfSpace;
        case Errc::read_only: return kServerErrorReadOnly;
        case Errc::out_of_memory: return kServerErrorOutOfMemory;
        default: return kServerErrorStorage;
    }
}

std::string_view commit_failure_reply(Errc code, bool meta) noexcept {
    if (code == Errc::cas_mismatch) return meta ? kMetaExists : kExists;
    if (code == Errc::condition_not_met)
        return meta ? kMetaNotStored : kNotStored;
    return storage_failure_reply(code);
}

void append_value_header(std::string& out, std::string_view key, std::uint32_t flags,
                         std::uint64_t bytes) {
    out.reserve(out.size() + 8 + key.size() + 24);
    out += "VALUE ";
    out += key;
    out += ' ';
    append_u32(out, flags);
    out += ' ';
    append_u64(out, bytes);
    out += "\r\n";
}

void append_value_header_cas(std::string& out, std::string_view key, std::uint32_t flags,
                             std::uint64_t bytes, std::uint64_t cas) {
    out.reserve(out.size() + 8 + key.size() + 40);
    out += "VALUE ";
    out += key;
    out += ' ';
    append_u32(out, flags);
    out += ' ';
    append_u64(out, bytes);
    out += ' ';
    append_u64(out, cas);
    out += "\r\n";
}

std::string value_header(std::string_view key, std::uint32_t flags, std::uint64_t bytes) {
    std::string s;
    append_value_header(s, key, flags, bytes);
    return s;
}

std::string value_header_cas(std::string_view key, std::uint32_t flags, std::uint64_t bytes,
                             std::uint64_t cas) {
    std::string s;
    append_value_header_cas(s, key, flags, bytes, cas);
    return s;
}

} // namespace goblin::memcache
