// memcache text protocol on the shared streaming loop (ADR-0002/0005). All the connection,
// GET-streaming, backpressure and slow-client machinery lives in net::StreamLoop; this layer is just
// the four protocol seams: command parse/dispatch + SET ingest (process), the VALUE header and the
// END trailer for a GET, and the END for a miss.
#pragma once

#include "goblin/net/stream_loop.hpp"

#include <string>
#include <string_view>

namespace goblin::memcache {

class Authenticator;

class EventLoop : public net::StreamLoop {
public:
    using net::StreamLoop::StreamLoop; // inherit the (reactor, lfd, tm, index, iobufs, timeout) ctor
    void set_authenticator(const Authenticator* value) noexcept { authenticator_ = value; }

protected:
    void process(Conn*) override;
    void frame_get_hit(Conn*, std::string_view key, const storage::ObjectMeta&) override;
    void frame_get_miss(Conn*) override;
    void on_value_sent(Conn*) override;
    void append_value_trailer(Conn*) override;

private:
    // Continue an active classic multi-get until it parks, starts streaming a hit, or reaches the
    // final key. Returns false only when read-buffer backpressure parked the current key.
    bool continue_get_batch(Conn*, std::uint32_t now);
    std::string format_stats() const; // STAT lines for the memcache `stats` command (aggregated)
    std::string format_settings() const;
    const Authenticator* authenticator_ = nullptr;
};

} // namespace goblin::memcache
