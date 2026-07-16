// Protocol-agnostic per-core async event loop over one io_uring ring (ADR-0001/0002). Owns its
// connections and dispatches accept/recv/send/read completions by tagged user_data, freeing a
// connection only once its in-flight op count hits zero (so a late completion can't touch freed
// memory). It drives the shared object-serving machinery — head-first GET streaming through
// TierManager (zero-copy pinned head + per-piece async disk reads into one borrowed I/O buffer),
// read/write backpressure (park + drain, ADR-0011), and the slow-client stall sweep — and calls
// virtual hooks for the four protocol-specific seams. memcache and HTTP each subclass it.
#pragma once

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h> // iovec, msghdr, MSG_MORE
#include <unordered_map>

namespace goblin::net {

class StreamLoop {
public:
    StreamLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm, storage::Index& index,
               core::IoBufferPool& iobufs, unsigned io_timeout_ms = 0,
               core::StatsRegistry* reg = nullptr, WriteMode write_mode = WriteMode::evict);
    virtual ~StreamLoop();

    void run();           // arm accept, then loop until stop()
    void run_once();      // one submit -> wait(timeout) -> reap -> dispatch (steppable, tests)
    void stop() noexcept; // ask run() to exit (thread-safe)
    std::size_t live_conns() const noexcept { return conns_.size(); }
    void set_read_ahead(bool on) noexcept { read_ahead_ = on; } // double-buffered GET pipeline (A/B knob)
    void set_shutdown(const std::atomic<bool>* flag, unsigned grace_ms) noexcept {
        shutdown_ = flag; // observed in run(): once set, stop accepting, drain in-flight, then return
        shutdown_grace_ms_ = grace_ms;
    }

protected:
    enum class St { idle, set_body, set_wait, get_wait, get_header, get_send_head, get_stream, get_trailer };

    struct Conn {
        int fd = -1;
        unsigned inflight = 0;   // outstanding SQEs referencing this Conn
        bool closing = false;    // freed once inflight hits 0
        bool quit_after = false; // close once the pending response is sent (memcache quit / HTTP no-keep-alive)
        St state = St::idle;
        std::chrono::steady_clock::time_point last_progress{}; // last completion that moved bytes (stall timeout)
        std::array<std::byte, 16 * 1024> rbuf;
        std::string in;           // accumulated unparsed request bytes
        std::size_t in_off = 0;   // consume cursor into `in` (avoids front-erase memmove until compact)
        std::size_t phr_prev_len = 0; // picohttpparser resume length (HTTP partial headers)
        std::string out;          // response header / status line / trailer pending send
        std::size_t out_sent = 0; // partial-send progress into out
        // Coalesced header+head sendmsg (TTFB): iov lives here until the CQE, not on the stack.
        iovec send_iov[2]{};
        msghdr send_msg{};
        bool coalesced_send = false;     // in-flight CQE spans out + head pin
        Size coalesced_head_len = 0;     // head bytes included in the writev
        std::string sni;          // TLS SNI, set after the handshake (empty = plaintext); HTTPS enforces Host==sni

        // GET streaming state:
        std::string get_key;        // final (post-derivation) key, kept to re-open_snapshot when parked
        crypto::Digest get_digest{}; // hashed key for park/retry (avoid re-SHA-256 on drain)
        std::optional<ByteRange> req_range; // requested sub-range (HTTP Range), resolved in frame_get_hit
        std::string inm;            // HTTP If-None-Match (conditional GET), used in frame_get_hit; empty=absent
        bool get_with_cas = false;  // memcache `gets`: emit the CAS in the VALUE header
        bool meta = false;            // meta `mg` with value: VA framing (frame_get_hit/miss/on_value_sent)
        std::uint8_t meta_rflags = 0; // meta return-flags bitmask (f=1 s=2 t=4 c=8 k=16)
        bool meta_quiet = false;      // meta q: suppress EN miss (mg) / HD success (ms)
        std::string meta_opaque;      // meta O<token> to echo (copied; `in` is erased before the stream)
        std::string meta_key;         // meta `ms` key, for k-echo on the HD reply (copied)
        std::uint32_t meta_expiry = 0; // meta `ms` absolute expiry (T converted at parse; 0 = never)
        std::optional<storage::TierManager::ReadStream> rs; // open object files for this GET
        Size get_size = 0;          // last byte to stream (exclusive)
        Size get_pos = 0;           // head/header-phase cursor; tail start handed to send_pos/plan_pos
        storage::TierManager::HeadPin head_pin; // pinned RAM head for the zero-copy send (ADR-0018)
        std::size_t head_sent = 0;              // partial-send progress into the head

        // Disk-tail streaming with double-buffered read-ahead (ADR-0011): up to 2 lanes. Lane 0's
        // buffer is mandatory; lane 1 is acquired opportunistically to pipeline -- read piece N+1 while
        // piece N sends. One lane => the serial single-buffer path. Sends stay strictly ordered; only
        // the disk reads run ahead. At most one read batch and one send are in flight at a time, so
        // completions route by the read_lane / send_lane indices (no extra tag bits needed).
        struct Lane {
            MutBytes buf;
            bool have = false;       // holds a read I/O-pool buffer
            Size len = 0;            // bytes in this lane's piece
            std::size_t sent = 0;    // partial-send progress within the piece
            unsigned reads = 0;      // disk reads still outstanding for this piece
            bool err = false;        // a read failed
            bool ready = false;      // piece fully read (or all-head): awaiting its turn to send
        };
        std::array<Lane, 2> lane;
        int n_lanes = 0;            // read buffers held (1 = serial, 2 = pipelined)
        int read_lane = -1;         // lane with disk reads in flight, or -1
        int send_lane = -1;         // lane with a send in flight, or -1
        int fill_i = 0;             // next lane to plan+read into (round-robin over n_lanes)
        int send_i = 0;             // next lane to send from (round-robin; pieces sent in plan order)
        Size plan_pos = 0;          // next object byte to plan+read
        Size send_pos = 0;          // next object byte to send (advances as pieces fully send)

        // Write-ingest state (memcache SET; HTTP PUT later):
        std::optional<storage::TierManager::StoreHandle> sh; // open writer for this body
        crypto::Digest set_digest{}; // key digest; kept to retry begin_store while parked (set_wait)
        Size set_remaining = 0;     // body bytes still to receive (== full nbytes while parked)
        std::uint32_t set_flags = 0;
        std::uint32_t set_exptime = 0; // raw memcache exptime; -> absolute expiry at commit time
        std::uint64_t set_cas = 0;     // cas store: required current CAS (0 = not a cas store)
        std::string_view set_reply;    // reply to send on set_reject: NOT_STORED / NOT_FOUND / EXISTS
        bool set_noreply = false;
        bool set_reject = false; // admission failed -> drain the body, then reject
        bool set_failed = false; // a disk write failed -> don't commit
    };

    // ---- the four protocol seams ----
    virtual void process(Conn*) = 0;                  // parse `in`, act, queue replies / start streams
    virtual void frame_get_hit(Conn*, std::string_view key, const storage::ObjectMeta&) = 0; // header + range
    virtual void frame_get_miss(Conn*) = 0;           // queue the miss response (END / 404), state -> idle
    virtual void on_value_sent(Conn*) = 0;            // value fully streamed: trailer (memcache) or finish (HTTP)
    // Append only the after-value framing into `out` (memcache CRLF+END; HTTP nothing). Used by the
    // small-object inline path that already copied the body into `out` for a single send.
    virtual void append_value_trailer(Conn*) = 0;

    // ---- optional seams (default to plaintext); HTTPS overrides them to drive the TLS handshake/read ----
    virtual void on_connection(Conn* c) { start_recv(c); } // first action after accept (TLS: handshake)
    virtual void on_poll(Conn* c, int /*revents*/) { close_conn(c); } // a readiness poll fired (TLS only)
    virtual void on_destroy(Conn*) {}                      // conn retiring -> free any per-conn TLS state

    // ---- shared machinery the subclass drives ----
    // Hash `key` once; on read-pool exhaustion parks with get_key+get_digest and returns false.
    // Drain retries via the digest overload so the key is not re-hashed.
    bool begin_get(Conn*, std::string_view key, bool record_access = true, std::uint32_t now = 0);
    bool begin_get(Conn*, std::string_view key, const crypto::Digest& digest, bool record_access,
                   std::uint32_t now = 0);
    virtual void start_recv(Conn*);                 // post a recv (HTTPS overrides: poll-driven SSL_read)
    void start_send(Conn*);                         // (re)send the pending `out` (may coalesce with head)
    void close_conn(Conn*);
    void finish_get(Conn*); // GET fully served -> release buffers, resume parsing

    // Input-buffer helpers: `in_off` is a consume cursor so pipelined leftovers aren't memmoved
    // on every request. Compact when waste grows large or the buffer is fully consumed.
    static std::string_view in_view(const Conn* c) noexcept {
        return std::string_view(c->in).substr(c->in_off);
    }
    static void consume_in(Conn* c, std::size_t n) noexcept {
        c->in_off += n;
        if (c->in_off >= c->in.size()) {
            c->in.clear();
            c->in_off = 0;
        } else if (c->in_off >= 4096) { // bound wasted prefix so capacity doesn't grow unbounded
            c->in.erase(0, c->in_off);
            c->in_off = 0;
        }
    }

    enum Op : unsigned { kRecv = 1, kSend = 2, kRead = 3, kPoll = 4 }; // user_data low 3 bits
    static std::uint64_t tag(Conn* c, unsigned op) { return reinterpret_cast<std::uint64_t>(c) | op; }

    core::Reactor& r_;
    storage::TierManager& tm_;
    storage::Index& index_;
    core::IoBufferPool& iobufs_;
    core::Stats stats_;                 // this worker's counters (single-writer; aggregated via reg_)
    core::StatsRegistry* reg_ = nullptr; // registry to aggregate on `stats` (memcache only); may be null
    WriteMode write_mode_ = WriteMode::evict; // protocol-specific disk-full admission policy
    bool read_ahead_ = true;             // acquire a 2nd read lane for pipelining in begin_get (A/B knob)
    std::deque<Conn*> set_waiters_; // writes parked on staging exhaustion (ADR-0011 backpressure)
    std::deque<Conn*> get_waiters_; // GETs parked on read I/O-pool exhaustion (per-loop; queue, never shed)

private:
    static constexpr std::uint64_t kAccept = 0; // user_data for the listener accept (no Conn)

    void arm_accept();
    void on_accept(int res);
    void on_recv(Conn*, int res);
    void on_send(Conn*, int res);
    void on_read(Conn*, int res);
    void start_send_head(Conn*);  // send the pinned head region zero-copy (partial-aware)
    void start_lane_send(Conn*);  // (re)send the current lane's piece (partial-aware)
    void prime_tail_read(Conn*);  // initial response queued -> start disk I/O behind the head
    void enter_stream(Conn*);     // head done -> allow the prefetched disk tail to send
    void pump_stream(Conn*);      // drive ordered sends plus read-ahead, or finish the value
    bool plan_lane_read(Conn*, int lane); // plan + issue a piece's reads into a lane; false -> abort
    void abort_get(Conn*);        // error mid-GET -> release buffers, close
    void drain_set_waiters();     // retry parked writes once a staging buffer may be free
    void drain_get_waiters();     // retry parked GETs once a read I/O buffer may be free
    void sweep_stalled(std::chrono::steady_clock::time_point now); // drop buffer-holding conns gone idle
    void abort_conn(Conn*);       // abortive close (RST) so a stalled conn's pending op completes
    void drain();                 // graceful shutdown: finish in-flight transfers, drop idle conns
    void unpin_if_held(Conn*);
    void release_lanes(Conn*); // release all held read buffers + reset lane state
    void retire(Conn*);

    int lfd_;
    unsigned io_timeout_ms_; // drop a stalled in-flight transfer after this many ms (0 = off, ADR-0011)
    std::chrono::steady_clock::time_point last_sweep_{}; // last stall sweep, to bound sweep frequency
    std::atomic<bool> stop_{false};
    const std::atomic<bool>* shutdown_ = nullptr; // external SIGTERM flag; null = run until stop()
    unsigned shutdown_grace_ms_ = 5000;           // drain deadline for in-flight transfers
    bool draining_ = false;                        // shutdown observed -> stop accepting, finish in-flight
    std::unordered_map<Conn*, std::unique_ptr<Conn>> conns_;
};

} // namespace goblin::net
