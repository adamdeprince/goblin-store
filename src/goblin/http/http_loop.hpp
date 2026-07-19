// HTTP/1.1 object serving on the shared streaming loop (ADR-0005/0015). Implements the four
// net::StreamLoop seams for HTTP: parse a request, derive the key (path or vhost mode), and serve a
// GET through the shared head-first stream (or a HEAD as headers only). Ranges and PUT land later;
// for now GET/HEAD on the whole object, everything else 405.
#pragma once

#include "goblin/http/key_derivation.hpp"
#include "goblin/http/mirror.hpp"
#include "goblin/net/stream_loop.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace goblin::http {

class HttpLoop : public net::StreamLoop {
public:
    HttpLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm, storage::Index& index,
             core::IoBufferPool& iobufs, KeyOptions keyopt, unsigned io_timeout_ms = 0,
             core::StatsRegistry* reg = nullptr, MirrorService* mirror = nullptr)
        : net::StreamLoop(reactor, listen_fd, tm, index, iobufs, io_timeout_ms, reg,
                          WriteMode::evict, mirror != nullptr),
          keyopt_(keyopt), mirror_(mirror) {}
    HttpLoop(core::Reactor& reactor, net::ConnectionInbox& inbox, storage::TierManager& tm,
             storage::Index& index, core::IoBufferPool& iobufs, KeyOptions keyopt,
             unsigned io_timeout_ms = 0, core::StatsRegistry* reg = nullptr,
             MirrorService* mirror = nullptr)
        : net::StreamLoop(reactor, inbox, tm, index, iobufs, io_timeout_ms, reg,
                          WriteMode::evict, mirror != nullptr),
          keyopt_(keyopt), mirror_(mirror) {}
    HttpLoop(net::StreamIo& stream_io, int listen_fd, storage::TierManager& tm,
             storage::Index& index, core::IoBufferPool& iobufs, KeyOptions keyopt,
             unsigned io_timeout_ms = 0, core::StatsRegistry* reg = nullptr,
             MirrorService* mirror = nullptr)
        : net::StreamLoop(stream_io, listen_fd, tm, index, iobufs, io_timeout_ms, reg,
                          WriteMode::evict, mirror != nullptr),
          keyopt_(keyopt), mirror_(mirror) {}
    HttpLoop(net::StreamIo& stream_io, net::ConnectionInbox& inbox,
             storage::TierManager& tm, storage::Index& index,
             core::IoBufferPool& iobufs, KeyOptions keyopt,
             unsigned io_timeout_ms = 0, core::StatsRegistry* reg = nullptr,
             MirrorService* mirror = nullptr)
        : net::StreamLoop(stream_io, inbox, tm, index, iobufs, io_timeout_ms, reg,
                          WriteMode::evict, mirror != nullptr),
          keyopt_(keyopt), mirror_(mirror) {}

protected:
    void process(Conn*) override;
    void frame_get_hit(Conn*, std::string_view key, const storage::ObjectMeta&) override;
    void frame_get_miss(Conn*) override;
    void on_value_sent(Conn*) override;
    void append_value_trailer(Conn*) override;
    bool accept_get_snapshot(Conn*, std::string_view,
                             const storage::ObjectMeta&) override;
    bool on_custom_send(Conn*, int result) override;
    void on_external(Conn*, int result) override;
    void on_destroy(Conn*) override;

private:
    KeyOptions keyopt_; // path vs vhost key derivation (ADR-0015)
    MirrorService* mirror_ = nullptr; // process-wide origin fetch pool; null = ordinary object mode
    struct MirrorContext {
        MirrorRequest request;
        std::shared_ptr<MirrorFetch> fetch;
        std::shared_ptr<const MirrorChunk> sending;
        std::uint64_t sending_sequence = 0;
        std::size_t sending_offset = 0;
        Size body_sent = 0;
        bool header_sent = false;
        bool close_delimited = false;
        bool allow_stale_once = false;
    };
    std::unordered_map<Conn*, MirrorContext> mirrors_;

    void start_mirror_fetch(Conn*);
    void advance_mirror(Conn*);
    void arm_mirror_notification(Conn*);
    void start_mirror_chunk_send(Conn*);
    void finish_mirror(Conn*);
    void abandon_mirror(Conn*);
    void append_mirror_head(Conn*, const OriginResponseHead&);
    void append_cached_head(Conn*, const storage::ObjectMeta&,
                            const storage::HttpCacheMetadata&);
};

} // namespace goblin::http
