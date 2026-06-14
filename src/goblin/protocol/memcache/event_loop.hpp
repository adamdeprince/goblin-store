// Per-core async event loop over one io_uring ring (ADR-0002). Owns its connections; dispatches
// accept/recv/send/read completions by tagged user_data; frees a connection only once its in-flight
// op count reaches zero (so a late completion can never touch freed memory). GET streams head-first
// through TierManager::ReadStream: header -> per-piece plan() -> async disk segment reads -> piece
// send -> trailer, borrowing one I/O-pool buffer for the stream. SET ingest lands in Step 4.
#pragma once

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
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
#include <unordered_map>
#include <vector>

namespace goblin::memcache {

class EventLoop {
public:
    EventLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm, storage::Index& index,
              core::IoBufferPool& iobufs, unsigned io_timeout_ms = 0);

    void run();           // arm accept, then loop until stop()
    void run_once();      // one submit -> wait(timeout) -> reap -> dispatch (steppable, tests)
    void stop() noexcept; // ask run() to exit (thread-safe)
    std::size_t live_conns() const noexcept { return conns_.size(); }

private:
    enum class St { idle, set_body, set_wait, get_wait, get_header, get_send_head, get_reading, get_send_piece, get_trailer };

    struct Conn {
        int fd = -1;
        unsigned inflight = 0;   // outstanding SQEs referencing this Conn
        bool closing = false;    // freed once inflight hits 0
        bool quit_after = false; // close once the pending response is sent
        St state = St::idle;
        std::chrono::steady_clock::time_point last_progress{}; // last completion that moved bytes (stall timeout)
        std::array<std::byte, 16 * 1024> rbuf;
        std::string in;           // accumulated unparsed request bytes
        std::string out;          // status line / VALUE header / trailer pending send
        std::size_t out_sent = 0; // partial-send progress into out

        // GET streaming state:
        std::string get_key;        // key, kept to re-open_snapshot when parked (get_wait)
        std::optional<storage::TierManager::ReadStream> rs; // open object files for this GET
        MutBytes iobuf;             // borrowed I/O-pool buffer (value pieces land here)
        bool have_iobuf = false;
        Size get_size = 0;          // total value bytes
        Size get_pos = 0;           // next byte to stream
        Size piece_len = 0;         // bytes in the current piece
        std::size_t piece_sent = 0; // partial-send progress into the current piece
        unsigned pending_reads = 0; // disk segment reads outstanding for the current piece
        bool read_error = false;
        storage::TierManager::HeadPin head_pin; // pinned RAM head for the zero-copy send (ADR-0018)
        std::size_t head_sent = 0;              // partial-send progress into the head

        // SET ingest state:
        std::optional<storage::TierManager::StoreHandle> sh; // open writer for this SET
        crypto::Digest set_digest{}; // key digest; kept to retry begin_store while parked (set_wait)
        Size set_remaining = 0;     // body bytes still to receive (== full nbytes while parked)
        std::uint32_t set_flags = 0;
        bool set_noreply = false;
        bool set_reject = false; // admission failed (add exists / replace missing) -> drain + NOT_STORED
        bool set_failed = false; // a disk write failed -> don't commit
    };

    static constexpr std::uint64_t kAccept = 0; // user_data for the listener accept (no Conn)
    enum Op : std::uint64_t { kRecv = 1, kSend = 2, kRead = 3 };
    static std::uint64_t tag(Conn* c, Op op) { return reinterpret_cast<std::uint64_t>(c) | op; }

    void arm_accept();
    void on_accept(int res);
    void start_recv(Conn*);
    void start_send(Conn*);
    void on_recv(Conn*, int res);
    void on_send(Conn*, int res);
    void on_read(Conn*, int res);
    void start_send_head(Conn*); // send the pinned head region zero-copy (partial-aware)
    void unpin_if_held(Conn*);   // release the head pin if this conn holds one
    void process(Conn*);          // parse commands out of `in`, act, queue replies / start streams
    bool begin_get(Conn*, const std::string& key); // open the GET; false if parked on read-pool exhaustion
    void drain_set_waiters();     // retry parked SETs (set_wait) once a write-staging buffer may be free
    void drain_get_waiters();     // retry parked GETs (get_wait) once a read I/O buffer may be free
    void sweep_stalled(std::chrono::steady_clock::time_point now); // drop buffer-holding conns gone idle
    void abort_conn(Conn*);       // abortive close (RST) so a stalled conn's pending op completes -> buffer freed
    void pump_get(Conn*);         // produce + send the next value piece, or the trailer
    void start_send_piece(Conn*); // (re)send the current piece from iobuf (partial-aware)
    void finish_get(Conn*);       // GET fully sent -> release buffers, resume parsing
    void abort_get(Conn*);        // error mid-GET -> release buffers, close
    void release_iobuf(Conn*);
    void close_conn(Conn*);
    void retire(Conn*);

    core::Reactor& r_;
    int lfd_;
    storage::TierManager& tm_;
    storage::Index& index_;
    core::IoBufferPool& iobufs_;
    unsigned io_timeout_ms_; // drop a stalled in-flight transfer after this many ms (0 = off, ADR-0011)
    std::chrono::steady_clock::time_point last_sweep_{}; // last stall sweep, to bound sweep frequency
    std::atomic<bool> stop_{false};
    std::unordered_map<Conn*, std::unique_ptr<Conn>> conns_;
    std::deque<Conn*> set_waiters_; // SETs parked on write-staging exhaustion (ADR-0011 backpressure)
    std::deque<Conn*> get_waiters_; // GETs parked on read I/O-pool exhaustion (per-loop; queue, never shed)
};

} // namespace goblin::memcache
