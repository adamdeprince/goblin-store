#include "goblin/store/client.hpp"
#include "goblin/store/rdma_wire.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace goblin::client {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

void validate_key(std::string_view key) {
    if (key.empty() || key.size() > 250) {
        throw std::invalid_argument("memcache key must contain 1 to 250 bytes");
    }
    for (const unsigned char ch : key) {
        if (ch <= 0x20 || ch == 0x7f) {
            throw std::invalid_argument("memcache key contains whitespace or a control byte");
        }
    }
}

template <class Integer>
Integer parse_integer(std::string_view text, std::string_view field) {
    Integer value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        throw ProtocolError("invalid " + std::string(field) + " in memcache response");
    }
    return value;
}

template <class Integer>
void append_integer(std::string& out, Integer value) {
    char bytes[32];
    const auto [end, ec] = std::to_chars(bytes, bytes + sizeof(bytes), value);
    if (ec != std::errc{}) throw std::runtime_error("formatting integer failed");
    out.append(bytes, static_cast<std::size_t>(end - bytes));
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < line.size()) {
        const std::size_t end = line.find(' ', pos);
        out.push_back(line.substr(pos, end == std::string_view::npos ? line.size() - pos
                                                                    : end - pos));
        if (end == std::string_view::npos) break;
        pos = end + 1;
        if (pos == line.size() || line[pos] == ' ') {
            out.emplace_back();
            break;
        }
    }
    return out;
}

[[noreturn]] void throw_server_error(std::string_view line) {
    throw ServerError(std::string(line));
}

bool is_server_error(std::string_view line) {
    return line == "ERROR" || line.starts_with("CLIENT_ERROR ") ||
           line.starts_with("SERVER_ERROR ");
}

} // namespace

struct Client::Impl {
    std::unique_ptr<Transport> transport;
    std::chrono::milliseconds operation_timeout;
    std::uint64_t max_value_bytes;
    std::recursive_mutex mutex;
    bool transaction_active = false;
    std::thread::id transaction_owner{};
    std::string input;
    std::size_t input_offset = 0;

    Impl(std::unique_ptr<Transport> t, std::chrono::milliseconds timeout,
         std::uint64_t max_value)
        : transport(std::move(t)), operation_timeout(timeout), max_value_bytes(max_value) {
        if (!transport) throw std::invalid_argument("client transport must not be null");
        if (operation_timeout.count() < 0)
            throw std::invalid_argument("operation timeout must not be negative");
        if (transport->max_fragment_size() == 0)
            throw std::invalid_argument("transport fragment size must not be zero");
        if (transport->max_bulk_fragment_size() == 0)
            throw std::invalid_argument("transport bulk fragment size must not be zero");
    }

    ~Impl() { transport->close(); }

    struct Transaction {
        Impl& impl;
        std::unique_lock<std::recursive_mutex> lock;
        bool armed = false;

        explicit Transaction(Impl& value) : impl(value), lock(value.mutex) {
            if (impl.transaction_active && impl.transaction_owner == std::this_thread::get_id())
                throw std::logic_error("Goblin Store client operations are not reentrant");
            impl.transaction_active = true;
            impl.transaction_owner = std::this_thread::get_id();
            armed = true;
        }
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        ~Transaction() {
            if (armed) {
                impl.transaction_active = false;
                impl.transaction_owner = {};
            }
        }
    };

    [[nodiscard]] Deadline deadline() const noexcept {
        return Clock::now() + operation_timeout;
    }

    [[noreturn]] void fail_connection(std::string_view phase) {
        std::string message(phase);
        message += ": RDMA connection failed";
        if (!transport->error().empty()) {
            message += ": ";
            message += transport->error();
        }
        transport->close();
        throw ConnectionError(message);
    }

    [[noreturn]] void fail_timeout(std::string_view phase) {
        transport->close();
        throw TimeoutError(std::string(phase) + ": timed out");
    }

    [[noreturn]] void fail_protocol(std::string message) {
        transport->close();
        throw ProtocolError(std::move(message));
    }

    void check_wait(Deadline until, std::string_view phase) {
        if (transport->failed() || transport->disconnected()) fail_connection(phase);
        if (Clock::now() >= until) fail_timeout(phase);
        transport->wait();
    }

    void check_progress(Deadline until, std::string_view phase) {
        if (transport->failed() || transport->disconnected()) fail_connection(phase);
        if (Clock::now() >= until) fail_timeout(phase);
    }

    void write_all(std::string_view bytes, Deadline until, std::string_view phase) {
        const std::size_t maximum = transport->max_fragment_size();
        while (!bytes.empty()) {
            check_progress(until, phase);
            const std::size_t length = std::min(maximum, bytes.size());
            if (transport->try_send(bytes.substr(0, length))) {
                bytes.remove_prefix(length);
            } else {
                check_wait(until, phase);
            }
        }
    }

    void write_bulk_all(std::string_view bytes, Deadline until, std::string_view phase) {
        const std::size_t maximum = transport->max_bulk_fragment_size();
        while (!bytes.empty()) {
            check_progress(until, phase);
            const std::size_t length = std::min(maximum, bytes.size());
            if (transport->try_send_bulk(bytes.substr(0, length))) {
                bytes.remove_prefix(length);
            } else {
                check_wait(until, phase);
            }
        }
    }

    void compact_input() {
        if (input_offset == input.size()) {
            input.clear();
            input_offset = 0;
        } else if (input_offset >= 4096) {
            input.erase(0, input_offset);
            input_offset = 0;
        }
    }

    void fill(Deadline until, std::string_view phase) {
        for (;;) {
            check_progress(until, phase);
            if (auto fragment = transport->peek()) {
                if (fragment->empty()) fail_protocol("RDMA transport returned an empty fragment");
                try {
                    input.append(fragment->data(), fragment->size());
                } catch (...) {
                    transport->close();
                    throw;
                }
                transport->pop();
                return;
            }
            check_wait(until, phase);
        }
    }

    [[nodiscard]] std::string read_line(Deadline until) {
        constexpr std::size_t kMaxLine = 8192;
        for (;;) {
            check_progress(until, "receive response line");
            const std::size_t end = input.find("\r\n", input_offset);
            if (end != std::string::npos) {
                if (end - input_offset > kMaxLine)
                    fail_protocol("memcache response line exceeds 8192 bytes");
                std::string line(input.data() + input_offset, end - input_offset);
                input_offset = end + 2;
                compact_input();
                return line;
            }
            const std::size_t buffered = input.size() - input_offset;
            if (buffered > kMaxLine &&
                !(buffered == kMaxLine + 1 && input.back() == '\r'))
                fail_protocol("memcache response line exceeds 8192 bytes");
            fill(until, "receive response");
        }
    }

    void read_exact(std::uint64_t count, Deadline until, const Sink& sink) {
        while (count != 0) {
            check_progress(until, "receive value body");
            if (input_offset == input.size()) fill(until, "receive value body");
            const std::size_t available = input.size() - input_offset;
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(count, static_cast<std::uint64_t>(available)));
            try {
                sink(std::string_view(input).substr(input_offset, take));
            } catch (...) {
                transport->close();
                throw;
            }
            input_offset += take;
            count -= take;
            compact_input();
        }
    }

    void expect_bytes(std::string_view expected, Deadline until) {
        std::size_t matched = 0;
        read_exact(expected.size(), until, [&](std::string_view bytes) {
            if (bytes != expected.substr(matched, bytes.size()))
                fail_protocol("malformed memcache value trailer");
            matched += bytes.size();
        });
    }

    [[nodiscard]] std::optional<ItemInfo> read_get_header(std::string_view requested_key,
                                                          bool with_cas, Deadline until) {
        const std::string line = read_line(until);
        if (line == "END") return std::nullopt;
        if (is_server_error(line)) throw_server_error(line);

        const auto token = words(line);
        const std::size_t expected = with_cas ? 5 : 4;
        if (token.size() != expected || token[0] != "VALUE")
            fail_protocol("unexpected memcache GET response: " + line);
        if (token[1] != requested_key)
            fail_protocol("memcache GET response key does not match the request");

        ItemInfo info;
        try {
            info.flags = parse_integer<std::uint32_t>(token[2], "flags");
            info.size = parse_integer<std::uint64_t>(token[3], "value size");
            if (with_cas) info.cas = parse_integer<std::uint64_t>(token[4], "CAS");
        } catch (const ProtocolError&) {
            transport->close();
            throw;
        }
        return info;
    }

    [[nodiscard]] std::optional<ItemInfo> get_to_locked(std::string_view key, const Sink& sink,
                                                        bool with_cas,
                                                        std::uint64_t materialize_limit) {
        validate_key(key);
        if (!sink) throw std::invalid_argument("GET sink must not be empty");
        const Deadline until = deadline();
        std::string command = with_cas ? "gets " : "get ";
        command.append(key);
        command += "\r\n";
        write_all(command, until, "send GET request");

        auto info = read_get_header(key, with_cas, until);
        if (!info) return std::nullopt;
        if (materialize_limit != 0 && info->size > materialize_limit) {
            transport->close();
            throw ValueTooLargeError("memcache value is larger than the configured materialization limit");
        }
        read_exact(info->size, until, sink);
        expect_bytes("\r\n", until);
        const std::string end = read_line(until);
        if (end != "END") fail_protocol("memcache GET response is missing END");
        return info;
    }

    [[nodiscard]] StoreResult store_locked(std::string_view verb, std::string_view key,
                                           std::string_view value, std::uint32_t flags,
                                           std::uint32_t exptime,
                                           std::optional<std::uint64_t> cas) {
        validate_key(key);
        const Deadline until = deadline();
        std::string header;
        header.reserve(verb.size() + key.size() + 96);
        header.append(verb);
        header += ' ';
        header.append(key);
        header += ' ';
        append_integer(header, flags);
        header += ' ';
        append_integer(header, exptime);
        header += ' ';
        append_integer(header, value.size());
        if (cas) {
            header += ' ';
            append_integer(header, *cas);
        }
        header += "\r\n";

        write_all(header, until, "send storage header");
        // Value bytes never enter the inline control ring. An empty value has
        // no data fragment; its declared zero length plus the inline trailer
        // fully represents it on the ordered transport.
        write_bulk_all(value, until, "send storage body");
        write_all("\r\n", until, "send storage trailer");

        const std::string line = read_line(until);
        if (line == "STORED") return StoreResult::stored;
        if (line == "NOT_STORED") return StoreResult::not_stored;
        if (line == "EXISTS") return StoreResult::exists;
        if (line == "NOT_FOUND") return StoreResult::not_found;
        if (is_server_error(line)) throw_server_error(line);
        fail_protocol("unexpected memcache storage response: " + line);
    }
};

Client Client::connect(const Options& options) {
    if (options.address.empty()) throw std::invalid_argument("RDMA address must not be empty");
    if (options.port == 0) throw std::invalid_argument("RDMA port must not be zero");
    if (!goblin::store::rdma_wire::valid_bulk_geometry(options.bulk_window_bytes,
                                                        options.bulk_window_count)) {
        throw std::invalid_argument(
            "RDMA bulk windows must be power-of-two sizes of at least 4096 bytes, "
            "with 1 to 65535 windows and less than 4 GiB of combined receive "
            "and staging memory");
    }
    if (options.connect_timeout.count() < 0 || options.operation_timeout.count() < 0)
        throw std::invalid_argument("timeouts must not be negative");
    return Client(connect_rdma(options), options.operation_timeout, options.max_value_bytes);
}

Client::Client(std::unique_ptr<Transport> transport,
               std::chrono::milliseconds operation_timeout,
               std::uint64_t max_value_bytes)
    : impl_(std::make_unique<Impl>(std::move(transport), operation_timeout, max_value_bytes)) {}

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() = default;

std::optional<Item> Client::get(std::string_view key) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    Item item;
    const std::uint64_t limit = impl_->max_value_bytes;
    auto info = impl_->get_to_locked(key, [&](std::string_view bytes) { item.value.append(bytes); },
                                     false, limit);
    if (!info) return std::nullopt;
    static_cast<ItemInfo&>(item) = *info;
    return item;
}

std::optional<Item> Client::gets(std::string_view key) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    Item item;
    const std::uint64_t limit = impl_->max_value_bytes;
    auto info = impl_->get_to_locked(key, [&](std::string_view bytes) { item.value.append(bytes); },
                                     true, limit);
    if (!info) return std::nullopt;
    static_cast<ItemInfo&>(item) = *info;
    return item;
}

std::optional<ItemInfo> Client::get_to(std::string_view key, const Sink& sink, bool with_cas) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    return impl_->get_to_locked(key, sink, with_cas, 0);
}

StoreResult Client::set(std::string_view key, std::string_view value, std::uint32_t flags,
                        std::uint32_t exptime) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    return impl_->store_locked("set", key, value, flags, exptime, std::nullopt);
}

StoreResult Client::add(std::string_view key, std::string_view value, std::uint32_t flags,
                        std::uint32_t exptime) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    return impl_->store_locked("add", key, value, flags, exptime, std::nullopt);
}

StoreResult Client::replace(std::string_view key, std::string_view value, std::uint32_t flags,
                            std::uint32_t exptime) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    return impl_->store_locked("replace", key, value, flags, exptime, std::nullopt);
}

StoreResult Client::compare_exchange(std::string_view key, std::string_view value,
                                     std::uint64_t cas, std::uint32_t flags,
                                     std::uint32_t exptime) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    return impl_->store_locked("cas", key, value, flags, exptime, cas);
}

DeleteResult Client::erase(std::string_view key) {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    validate_key(key);
    const Deadline until = impl_->deadline();
    std::string command = "delete ";
    command.append(key);
    command += "\r\n";
    impl_->write_all(command, until, "send delete request");
    const std::string line = impl_->read_line(until);
    if (line == "DELETED") return DeleteResult::deleted;
    if (line == "NOT_FOUND") return DeleteResult::not_found;
    if (is_server_error(line)) throw_server_error(line);
    impl_->fail_protocol("unexpected memcache delete response: " + line);
}

std::string Client::version() {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    const Deadline until = impl_->deadline();
    impl_->write_all("version\r\n", until, "send version request");
    const std::string line = impl_->read_line(until);
    if (is_server_error(line)) throw_server_error(line);
    constexpr std::string_view prefix = "VERSION ";
    if (!line.starts_with(prefix)) impl_->fail_protocol("unexpected memcache version response: " + line);
    return line.substr(prefix.size());
}

std::unordered_map<std::string, std::string> Client::stats() {
    if (!impl_) throw ConnectionError("client has been moved from");
    Impl::Transaction transaction(*impl_);
    const Deadline until = impl_->deadline();
    impl_->write_all("stats\r\n", until, "send stats request");
    std::unordered_map<std::string, std::string> result;
    for (;;) {
        const std::string line = impl_->read_line(until);
        if (line == "END") return result;
        if (is_server_error(line)) throw_server_error(line);
        constexpr std::string_view prefix = "STAT ";
        if (!line.starts_with(prefix)) impl_->fail_protocol("unexpected memcache stats response: " + line);
        const std::string_view body(line.data() + prefix.size(), line.size() - prefix.size());
        const std::size_t split = body.find(' ');
        if (split == std::string_view::npos || split == 0 || split + 1 == body.size())
            impl_->fail_protocol("malformed memcache STAT response: " + line);
        result.emplace(body.substr(0, split), body.substr(split + 1));
    }
}

void Client::close() noexcept {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->transport->close();
}

} // namespace goblin::client
