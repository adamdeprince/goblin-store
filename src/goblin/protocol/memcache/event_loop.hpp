// memcache text protocol on the shared streaming loop (ADR-0002/0005). All the connection,
// GET-streaming, backpressure and slow-client machinery lives in net::StreamLoop; this layer is just
// the four protocol seams: command parse/dispatch + SET ingest (process), the VALUE header and the
// END trailer for a GET, and the END for a miss.
#pragma once

#include "goblin/net/stream_loop.hpp"

#include <string>

namespace goblin::memcache {

class EventLoop : public net::StreamLoop {
public:
    using net::StreamLoop::StreamLoop; // inherit the (reactor, lfd, tm, index, iobufs, timeout) ctor

protected:
    void process(Conn*) override;
    void frame_get_hit(Conn*, const std::string& key, const storage::ObjectMeta&) override;
    void frame_get_miss(Conn*) override;
    void on_value_sent(Conn*) override;
};

} // namespace goblin::memcache
