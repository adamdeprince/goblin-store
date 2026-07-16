#pragma once

#include "goblin/store/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace goblin::client {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class ConnectionError : public Error { using Error::Error; };
class TimeoutError : public Error { using Error::Error; };
class ProtocolError : public Error { using Error::Error; };
class ServerError : public Error { using Error::Error; };
class ValueTooLargeError : public Error { using Error::Error; };

struct Options {
    // Numeric IPv4 or IPv6 address. Hostname lookup is deliberately excluded
    // so connect_timeout bounds the complete RDMA-CM setup.
    std::string address;
    std::uint16_t port = 11211;
    std::uint64_t ring_bytes = 64 * 1024;
    // Per-connection registered receive-window geometry. Commands, metadata,
    // and trailers use the inline ring; value bodies use these bulk windows.
    // The size must be a power of two of at least 4096 bytes. Each connection
    // registers both receive and outbound-staging halves, consuming
    // 2 * bulk_window_bytes * bulk_window_count bytes in addition to the ring.
    std::uint32_t bulk_window_bytes = 256 * 1024;
    std::uint16_t bulk_window_count = 4;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds operation_timeout{30000};
    // Zero means limited only by std::string::max_size(). get_to() does not
    // materialize the value and therefore does not use this limit.
    std::uint64_t max_value_bytes = 0;
};

struct ItemInfo {
    std::uint32_t flags = 0;
    std::uint64_t size = 0;
    std::optional<std::uint64_t> cas;
};

struct Item : ItemInfo {
    std::string value;
};

enum class StoreResult { stored, not_stored, exists, not_found };
enum class DeleteResult { deleted, not_found };

// Native InfiniBand memcache client. The transport is the Goblin/Packrat
// one-sided RC ring; memcache remains its ordinary binary-safe text stream.
// Calls on one Client are safe from multiple threads but execute in order on
// one QP. Use one Client per thread when parallelism is desired.
class Client {
public:
    using Sink = std::function<void(std::string_view)>;

    [[nodiscard]] static Client connect(const Options& options);

    // Injection seam for deterministic tests and alternate byte transports.
    // The same incremental parser and framing are used in either case.
    Client(std::unique_ptr<Transport> transport,
           std::chrono::milliseconds operation_timeout = std::chrono::milliseconds{30000},
           std::uint64_t max_value_bytes = 0);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    ~Client();

    [[nodiscard]] std::optional<Item> get(std::string_view key);
    [[nodiscard]] std::optional<Item> gets(std::string_view key);

    // Streams the body without materializing it. The sink is invoked with
    // ordered, non-empty pieces while the complete transaction is serialized.
    // It must not start another transaction on this Client; attempted
    // transactional re-entry throws logic_error.
    [[nodiscard]] std::optional<ItemInfo> get_to(std::string_view key, const Sink& sink,
                                                 bool with_cas = false);

    [[nodiscard]] StoreResult set(std::string_view key, std::string_view value,
                                  std::uint32_t flags = 0, std::uint32_t exptime = 0);
    [[nodiscard]] StoreResult add(std::string_view key, std::string_view value,
                                  std::uint32_t flags = 0, std::uint32_t exptime = 0);
    [[nodiscard]] StoreResult replace(std::string_view key, std::string_view value,
                                      std::uint32_t flags = 0, std::uint32_t exptime = 0);
    [[nodiscard]] StoreResult compare_exchange(std::string_view key, std::string_view value,
                                               std::uint64_t cas, std::uint32_t flags = 0,
                                               std::uint32_t exptime = 0);
    [[nodiscard]] DeleteResult erase(std::string_view key);

    [[nodiscard]] std::string version();
    [[nodiscard]] std::unordered_map<std::string, std::string> stats();

    void close() noexcept;

private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool rdma_available() noexcept;
[[nodiscard]] std::unique_ptr<Transport> connect_rdma(const Options& options);

} // namespace goblin::client
