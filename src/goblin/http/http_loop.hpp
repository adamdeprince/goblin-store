// HTTP/1.1 object serving on the shared streaming loop (ADR-0005/0015). Implements the four
// net::StreamLoop seams for HTTP: parse a request, derive the key (path or vhost mode), and serve a
// GET through the shared head-first stream (or a HEAD as headers only). Ranges and PUT land later;
// for now GET/HEAD on the whole object, everything else 405.
#pragma once

#include "goblin/http/key_derivation.hpp"
#include "goblin/net/stream_loop.hpp"

#include <string>

namespace goblin::http {

class HttpLoop : public net::StreamLoop {
public:
    HttpLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm, storage::Index& index,
             core::IoBufferPool& iobufs, KeyOptions keyopt, unsigned io_timeout_ms = 0,
             core::StatsRegistry* reg = nullptr)
        : net::StreamLoop(reactor, listen_fd, tm, index, iobufs, io_timeout_ms, reg), keyopt_(keyopt) {}

protected:
    void process(Conn*) override;
    void frame_get_hit(Conn*, const std::string& key, const storage::ObjectMeta&) override;
    void frame_get_miss(Conn*) override;
    void on_value_sent(Conn*) override;

private:
    KeyOptions keyopt_; // path vs vhost key derivation (ADR-0015)
};

} // namespace goblin::http
