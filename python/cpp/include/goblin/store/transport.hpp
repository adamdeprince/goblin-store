#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace goblin::client {

// Ordered, bidirectional byte fragments. Fragment boundaries have no protocol
// meaning: the memcache layer treats received inline and bulk fragments as one
// continuous stream. Outbound command/status framing uses try_send(); object
// bodies use try_send_bulk() so an RDMA implementation can move them through
// registered windows instead of the inline control ring. A transport is
// single-owner; Client serializes complete request/reply pairs.
class Transport {
public:
    virtual ~Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    [[nodiscard]] virtual std::size_t max_fragment_size() const noexcept = 0;
    [[nodiscard]] virtual bool try_send(std::string_view fragment) noexcept = 0;
    [[nodiscard]] virtual std::size_t max_bulk_fragment_size() const noexcept = 0;
    [[nodiscard]] virtual bool try_send_bulk(std::string_view fragment) noexcept = 0;

    // The returned view remains valid until pop() and must not be retained.
    // Implementations transparently merge inline and registered-window data in
    // wire order; the memcache parser does not depend on their physical source.
    [[nodiscard]] virtual std::optional<std::string_view> peek() noexcept = 0;
    virtual void pop() noexcept = 0;

    [[nodiscard]] virtual bool failed() const noexcept = 0;
    [[nodiscard]] virtual bool disconnected() const noexcept = 0;
    [[nodiscard]] virtual std::string_view error() const noexcept = 0;
    // Make nonblocking transport progress (CQ/CM polling), then issue one CPU
    // relax hint. Client owns deadline enforcement around this call.
    virtual void wait() noexcept = 0;
    virtual void close() noexcept = 0;

protected:
    Transport() = default;
};

} // namespace goblin::client
