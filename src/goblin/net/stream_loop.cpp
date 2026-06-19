#include "goblin/net/stream_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace goblin::net {

namespace {
constexpr auto rlx = std::memory_order_relaxed;
}

StreamLoop::StreamLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm,
                       storage::Index& index, core::IoBufferPool& iobufs, unsigned io_timeout_ms,
                       core::StatsRegistry* reg)
    : r_(reactor), tm_(tm), index_(index), iobufs_(iobufs), reg_(reg), lfd_(listen_fd),
      io_timeout_ms_(io_timeout_ms), last_sweep_(std::chrono::steady_clock::now()) {
    if (reg_) reg_->add(&stats_);
}

StreamLoop::~StreamLoop() {
    if (reg_) reg_->remove(&stats_);
}

void StreamLoop::stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

void StreamLoop::run() {
    arm_accept();
    while (!stop_.load(std::memory_order_relaxed)) {
        if (shutdown_ && shutdown_->load(std::memory_order_relaxed)) { drain(); break; }
        run_once();
    }
    for (auto& [c, up] : conns_) ::close(c->fd); // hard-close anything left after the drain / on stop()
    conns_.clear();
}

// Graceful shutdown: stop accepting, let in-flight transfers (GET streams / SET ingests) finish, and
// retire connections idle between requests. Bounded by shutdown_grace_ms_; run() hard-closes whatever
// outlasts it. Idle conns get shutdown(SHUT_RDWR) -- a HALF-close, not an RST: SHUT_WR drains the
// kernel send buffer to the client (FIN only after it empties, so a response still in flight isn't
// truncated), and SHUT_RD completes the conn's pending recv with EOF so on_recv()/retire() reclaim it.
void StreamLoop::drain() {
    draining_ = true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(shutdown_grace_ms_);
    while (!conns_.empty() && std::chrono::steady_clock::now() < deadline) {
        for (auto& [c, up] : conns_)
            if (!c->closing && c->state == St::idle && c->out.empty()) // active conns finish on their own
                ::shutdown(c->fd, SHUT_RDWR);
        run_once(); // dispatch completions; active conns advance, half-closed idle ones retire
    }
}

void StreamLoop::run_once() {
    r_.submit_and_wait_timeout(200);
    const auto now = std::chrono::steady_clock::now();
    std::array<core::Completion, 256> cqes{};
    const unsigned n = r_.reap(cqes);
    for (unsigned i = 0; i < n; ++i) {
        const std::uint64_t ud = cqes[i].user_data;
        const int res = cqes[i].res;
        if (ud == kAccept) {
            on_accept(res);
            continue;
        }
        auto* c = reinterpret_cast<Conn*>(ud & ~std::uint64_t{7});
        const std::uint64_t op = ud & 7;
        if (c->inflight) --c->inflight;
        if (res > 0) c->last_progress = now; // bytes moved -> this conn is making progress
        if (op == kRecv)
            on_recv(c, res);
        else if (op == kSend)
            on_send(c, res);
        else if (op == kRead)
            on_read(c, res);
        else if (op == kPoll)
            on_poll(c, res);
        retire(c);
    }
    // After dispatch (incl. any write commits / GET completions that freed buffers this tick), wake
    // parked ops. Writes also wake on idle timeout ticks (a buffer freed on another loop); the GET
    // read pool is per-loop, so its frees are always local.
    if (!set_waiters_.empty()) drain_set_waiters();
    if (!get_waiters_.empty()) drain_get_waiters();
    // Periodically drop connections whose in-flight transfer has stalled (slow client) and reclaim
    // their buffers. Sweep frequency is bounded so it stays cheap with many connections.
    if (io_timeout_ms_) {
        const auto interval = std::chrono::milliseconds(std::min(io_timeout_ms_, 250u));
        if (now - last_sweep_ >= interval) {
            last_sweep_ = now;
            sweep_stalled(now);
        }
    }
}

void StreamLoop::arm_accept() { r_.submit_accept(lfd_, kAccept); }

void StreamLoop::on_accept(int res) {
    if (!draining_) arm_accept();            // once draining, stop accepting new connections
    if (res < 0) return;
    if (draining_) { ::close(res); return; } // reject a connection that slipped in during the drain
    const int one = 1;
    ::setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // no Nagle: small replies must not
                                                                  // wait on the peer's delayed ACK (~40ms)
    auto up = std::make_unique<Conn>();
    up->fd = res;
    up->last_progress = std::chrono::steady_clock::now();
    Conn* c = up.get();
    conns_.emplace(c, std::move(up));
    stats_.conns.fetch_add(1, rlx);
    stats_.curr_conns.fetch_add(1, rlx);
    on_connection(c); // plaintext: start_recv; HTTPS: begin the TLS handshake
}

void StreamLoop::start_recv(Conn* c) {
    if (c->closing) return;
    if (r_.submit_recv(c->fd, MutBytes(c->rbuf.data(), c->rbuf.size()), tag(c, kRecv)))
        ++c->inflight;
    else
        close_conn(c);
}

void StreamLoop::start_send(Conn* c) {
    if (c->closing) return;
    const auto* p = reinterpret_cast<const std::byte*>(c->out.data()) + c->out_sent;
    if (r_.submit_send(c->fd, ByteView(p, c->out.size() - c->out_sent), tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void StreamLoop::on_recv(Conn* c, int res) {
    if (res <= 0) {
        close_conn(c);
        return;
    }
    c->in.append(reinterpret_cast<const char*>(c->rbuf.data()), static_cast<std::size_t>(res));
    process(c);
}

// Open a GET for `key`: take a consistent snapshot (ADR-0018: metadata + pinned head + open files
// under one lock, so a concurrent copy-on-write replace can't split head from body) and, if a disk
// tail remains, borrow a read I/O buffer. Returns false when the read pool is exhausted -> the conn
// parks in get_wait with the snapshot released (re-taken on retry; ADR-0011 backpressure -- queue,
// never shed). On a hit, frame_get_hit() queues the header and state becomes get_header; on a miss,
// frame_get_miss() queues the miss reply and state returns to idle.
bool StreamLoop::begin_get(Conn* c, const std::string& key) {
    auto snap = tm_.open_snapshot(crypto::hash_key(key));
    if (!snap) {
        stats_.get_misses.fetch_add(1, rlx);
        frame_get_miss(c);
        return true;
    }
    const Size head_len = snap->pin.valid ? snap->pin.len : 0;
    if (snap->meta.size > head_len) { // a disk tail remains -> need a read I/O buffer
        auto buf = iobufs_.acquire();
        if (!buf) { // pool exhausted -> park: drop the snapshot now, re-take it when a buffer frees
            if (snap->pin.valid) tm_.unpin_head(snap->pin);
            stats_.get_backpressure.fetch_add(1, rlx);
            c->get_key = key;
            c->state = St::get_wait;
            get_waiters_.push_back(c);
            return false;
        }
        c->rs.emplace(std::move(*snap->rs));
        c->lane[0].buf = *buf;
        c->lane[0].have = true;
        c->n_lanes = 1;
        if (read_ahead_) {
            if (auto buf2 = iobufs_.acquire()) { // opportunistic 2nd buffer -> read-ahead (best-effort)
                c->lane[1].buf = *buf2;
                c->lane[1].have = true;
                c->n_lanes = 2;
            }
        }
        c->read_lane = c->send_lane = -1;
        c->fill_i = c->send_i = 0;
        for (auto& L : c->lane) { L.len = L.sent = L.reads = 0; L.ready = L.err = false; }
    }
    c->head_pin = snap->pin;
    c->head_sent = 0;
    stats_.get_hits.fetch_add(1, rlx);
    frame_get_hit(c, key, snap->meta); // protocol header + get_pos/get_size
    c->state = St::get_header;
    return true;
}

// Retry writes parked on staging exhaustion. Runs every run_once tick: a write that commits on THIS
// loop frees a buffer picked up the same tick; a buffer freed on ANOTHER loop is caught on the next
// ~200 ms wait. FIFO; stops at the first conn that still can't get a buffer (pool then empty).
void StreamLoop::drain_set_waiters() {
    while (!set_waiters_.empty()) {
        Conn* c = set_waiters_.front();
        if (c->closing) { // parked conns have no in-flight op, but stay defensive
            set_waiters_.pop_front();
            continue;
        }
        auto h = tm_.begin_store(c->set_digest, c->set_remaining); // set_remaining == full nbytes here
        if (!h) {
            if (h.error().code == Errc::would_block) return; // still no buffer -> retry next tick
            c->set_reject = true;                            // real open failure -> reject
        } else {
            c->sh.emplace(std::move(*h));
        }
        set_waiters_.pop_front();
        c->state = St::set_body;
        process(c); // drive the body already buffered in `in`, then resume recv (or reply if rejected)
    }
}

// Retry GETs parked on read I/O-pool exhaustion. The read pool is per-loop, so a buffer freed by a
// finishing GET on this loop is always observed here. FIFO; begin_get re-parks the first conn that
// still can't get a buffer, returning false and stopping the drain until the next tick.
void StreamLoop::drain_get_waiters() {
    while (!get_waiters_.empty()) {
        Conn* c = get_waiters_.front();
        if (c->closing) {
            get_waiters_.pop_front();
            continue;
        }
        get_waiters_.pop_front();
        if (!begin_get(c, c->get_key)) return;         // re-parked -> pool still empty, retry next tick
        if (c->state == St::get_header) start_send(c); // hit -> stream the value (header first)
        else process(c);                                // miss while parked (removed) -> flush + resume
    }
}

// Head/header fully sent: begin streaming the disk tail [get_pos, get_size) through the read lanes.
void StreamLoop::enter_stream(Conn* c) {
    c->send_pos = c->plan_pos = c->get_pos;
    c->state = St::get_stream;
    pump_stream(c);
}

// Plan the next tail piece into lane `i` and issue its disk reads (or mark it ready immediately if it
// maps to already-resident head bytes the plan copied in). Advances plan_pos + fill_i. false -> abort.
bool StreamLoop::plan_lane_read(Conn* c, int i) {
    Conn::Lane& L = c->lane[i];
    const Size want = std::min<Size>(L.buf.size(), c->get_size - c->plan_pos);
    const auto plan = c->rs->plan(c->plan_pos, L.buf.subspan(0, want));
    if (plan.total != want) return false; // couldn't map the whole piece
    L.len = plan.total;
    L.sent = 0;
    L.reads = 0;
    L.err = false;
    L.ready = false;
    c->plan_pos += L.len;
    c->fill_i = (c->fill_i + 1) % c->n_lanes;
    if (plan.segs.empty()) { // all-head piece already copied into the buffer -> no disk reads
        L.ready = true;
        return true;
    }
    for (const auto& s : plan.segs) {
        // O_DIRECT (ADR-0011): read length must be a device-block multiple. Round up into the
        // (page-aligned, io_chunk-sized) buffer and serve only L.len bytes; offsets are already
        // 4 KiB-aligned (pieces start on 4 KiB boundaries, segments at stripe boundaries).
        const Size rlen = std::min<Size>(align_up(s.len, kDeviceBlock), L.buf.size() - s.out_off);
        if (!r_.submit_read(s.fd, s.file_off, L.buf.subspan(s.out_off, rlen), tag(c, kRead)))
            return false;
        ++c->inflight;
        ++L.reads;
    }
    c->read_lane = i;
    return true;
}

// Drive the tail pipeline: start the next in-order piece send if its lane is ready, and start the
// next read-ahead into a free lane. At most one send and one read are in flight, so the read of piece
// N+1 overlaps the send of piece N (ADR-0006 shock absorber). Finishes the value once all bytes send.
void StreamLoop::pump_stream(Conn* c) {
    for (;;) {
        bool progress = false;
        // Send the next piece in order, once its lane has finished reading and nothing else is sending.
        if (c->send_lane < 0 && c->send_pos < c->get_size && c->lane[c->send_i].ready) {
            c->send_lane = c->send_i;
            c->lane[c->send_i].sent = 0;
            start_lane_send(c);
            if (c->closing) return;
            progress = true;
        }
        // Read-ahead: fill the next free lane (never the one currently sending) while tail remains.
        if (c->read_lane < 0 && c->plan_pos < c->get_size) {
            const int f = c->fill_i;
            if (c->lane[f].have && !c->lane[f].ready && f != c->send_lane) {
                if (!plan_lane_read(c, f)) { abort_get(c); return; }
                progress = true;
            }
        }
        if (c->send_lane < 0 && c->read_lane < 0 && c->send_pos >= c->get_size) {
            on_value_sent(c); // tail fully sent
            return;
        }
        if (!progress) return; // nothing more to start this tick; wait for an in-flight completion
    }
}

void StreamLoop::on_read(Conn* c, int res) {
    if (c->read_lane < 0) return; // no read batch tracked (defensive)
    Conn::Lane& L = c->lane[c->read_lane];
    if (res <= 0) L.err = true;
    if (L.reads) --L.reads;
    if (L.reads != 0) return; // more segments of this piece still outstanding
    const int done = c->read_lane;
    c->read_lane = -1;
    if (c->closing) return; // already aborting; just drain
    if (c->lane[done].err) {
        abort_get(c);
        return;
    }
    c->lane[done].ready = true;
    pump_stream(c); // piece ready -> may start its send and the next read-ahead
}

void StreamLoop::start_lane_send(Conn* c) {
    if (c->closing) return;
    Conn::Lane& L = c->lane[c->send_lane];
    const ByteView piece(L.buf.data() + L.sent, L.len - L.sent);
    if (r_.submit_send(c->fd, piece, tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void StreamLoop::start_send_head(Conn* c) {
    if (c->closing) return;
    const Size head_hi = std::min<Size>(c->head_pin.len, c->get_size); // head bytes within the range
    const ByteView head = tm_.pinned_bytes(c->head_pin); // zero-copy: straight from the head pool
    const ByteView rem(head.data() + c->head_sent, head_hi - c->head_sent);
    if (r_.submit_send(c->fd, rem, tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void StreamLoop::on_send(Conn* c, int res) {
    if (res < 0) {
        if (res == -EAGAIN || res == -EWOULDBLOCK) { // non-blocking fd (kTLS/HTTPS): buffer full -> retry
            if (c->state == St::get_stream) start_lane_send(c);
            else if (c->state == St::get_send_head) start_send_head(c);
            else start_send(c);
            return;
        }
        close_conn(c);
        return;
    }
    if (res == 0) { // peer closed
        close_conn(c);
        return;
    }
    stats_.bytes_served.fetch_add(static_cast<std::uint64_t>(res), rlx); // header/head/piece/trailer
    if (c->state == St::get_stream) {
        Conn::Lane& L = c->lane[c->send_lane];
        L.sent += static_cast<std::size_t>(res);
        if (L.sent < L.len) {
            start_lane_send(c); // short send -> remainder of this piece
            return;
        }
        c->send_pos += L.len; // piece fully sent -> this lane is free again
        L.ready = false;
        L.len = L.sent = L.reads = 0;
        L.err = false;
        c->send_lane = -1;
        c->send_i = (c->send_i + 1) % c->n_lanes;
        pump_stream(c); // start the next ordered send + read-ahead, or finish the value
        return;
    }
    if (c->state == St::get_send_head) {
        c->head_sent += static_cast<std::size_t>(res); // head_sent is an absolute offset into the head
        const Size head_hi = std::min<Size>(c->head_pin.len, c->get_size);
        if (c->head_sent < head_hi) {
            start_send_head(c); // short send -> remainder of the head slice
            return;
        }
        unpin_if_held(c);     // head slice fully sent -> release the pin
        c->get_pos = head_hi; // tail (if any) continues from the end of the head slice
        enter_stream(c);
        return;
    }
    // out-based send: idle response, GET header, or GET trailer
    c->out_sent += static_cast<std::size_t>(res);
    if (c->out_sent < c->out.size()) {
        start_send(c);
        return;
    }
    c->out.clear();
    c->out_sent = 0;
    if (c->state == St::get_header) {
        // Send the resident head zero-copy for the part of the range it covers, [get_pos, head_hi).
        const Size head_hi = c->head_pin.valid ? std::min<Size>(c->head_pin.len, c->get_size) : 0;
        if (c->get_pos < head_hi) {
            c->head_sent = c->get_pos; // absolute offset within the head to start from
            c->state = St::get_send_head;
            start_send_head(c);
        } else {
            unpin_if_held(c); // range is disk-only (or empty) -> drop the unused head pin
            enter_stream(c);
        }
    } else if (c->state == St::get_trailer)
        finish_get(c);
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

void StreamLoop::release_lanes(Conn* c) {
    for (auto& L : c->lane) {
        if (L.have) {
            iobufs_.release(L.buf);
            L.have = false;
        }
        L.len = L.sent = L.reads = 0;
        L.ready = L.err = false;
    }
    c->n_lanes = 0;
    c->read_lane = c->send_lane = -1;
    c->fill_i = c->send_i = 0;
}

void StreamLoop::unpin_if_held(Conn* c) {
    if (c->head_pin.valid) {
        tm_.unpin_head(c->head_pin);
        c->head_pin.valid = false;
    }
}

void StreamLoop::finish_get(Conn* c) {
    release_lanes(c);
    unpin_if_held(c);
    c->rs.reset();
    c->state = St::idle;
    process(c); // resume any pipelined requests, or read more
}

void StreamLoop::abort_get(Conn* c) {
    release_lanes(c);
    unpin_if_held(c);
    c->rs.reset();
    close_conn(c);
}

void StreamLoop::close_conn(Conn* c) {
    if (c->closing) return;
    c->closing = true;
    ::close(c->fd);
}

void StreamLoop::retire(Conn* c) {
    if (c->closing && c->inflight == 0) {
        release_lanes(c);  // in case we closed mid-GET (frees any held read buffers)
        unpin_if_held(c);  // release the head pin if a send was interrupted
        on_destroy(c);     // free any per-conn TLS state before the Conn goes away
        stats_.curr_conns.fetch_sub(1, rlx);
        conns_.erase(c);
    }
}

// Abortive close: SO_LINGER{1,0} makes ::close send a RST, which discards unsent data and promptly
// completes any pending io_uring send/recv on the socket with an error -- so the in-flight op drains
// and retire() reclaims the buffer. A graceful FIN could leave a send parked indefinitely on a
// dead-but-open peer (exactly the slow-client case we're reclaiming from).
void StreamLoop::abort_conn(Conn* c) {
    if (c->closing) return;
    stats_.slow_drops.fetch_add(1, rlx);
    const linger lo{1, 0};
    ::setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lo, sizeof lo);
    close_conn(c);
}

// Drop connections whose in-flight transfer has gone idle past io_timeout_ms_ while holding a scarce
// buffer: a slow reader pinning a read buffer / head, or a slow writer pinning a staging buffer
// (ADR-0011). We sweep only resource-holders -- an idle keepalive conn between requests holds nothing
// and is left alone. Reclaiming an active slow reader's buffer also unblocks any parked GETs behind it.
void StreamLoop::sweep_stalled(std::chrono::steady_clock::time_point now) {
    const auto deadline = std::chrono::milliseconds(io_timeout_ms_);
    for (auto& [c, up] : conns_) {
        if (c->closing) continue;
        const bool holds_buffer = c->n_lanes > 0 || c->head_pin.valid || c->sh.has_value();
        if (holds_buffer && now - c->last_progress >= deadline) abort_conn(c);
    }
}

} // namespace goblin::net
