#include "goblin/protocol/memcache/event_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <string_view>
#include <unistd.h>

namespace goblin::memcache {

EventLoop::EventLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm,
                     storage::Index& index, core::IoBufferPool& iobufs)
    : r_(reactor), lfd_(listen_fd), tm_(tm), index_(index), iobufs_(iobufs) {}

void EventLoop::stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

void EventLoop::run() {
    arm_accept();
    while (!stop_.load(std::memory_order_relaxed)) run_once();
    for (auto& [c, up] : conns_) ::close(c->fd);
    conns_.clear();
}

void EventLoop::run_once() {
    r_.submit_and_wait_timeout(200);
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
        if (op == kRecv)
            on_recv(c, res);
        else if (op == kSend)
            on_send(c, res);
        else if (op == kRead)
            on_read(c, res);
        retire(c);
    }
}

void EventLoop::arm_accept() { r_.submit_accept(lfd_, kAccept); }

void EventLoop::on_accept(int res) {
    arm_accept();
    if (res < 0) return;
    auto up = std::make_unique<Conn>();
    up->fd = res;
    Conn* c = up.get();
    conns_.emplace(c, std::move(up));
    start_recv(c);
}

void EventLoop::start_recv(Conn* c) {
    if (c->closing) return;
    if (r_.submit_recv(c->fd, MutBytes(c->rbuf.data(), c->rbuf.size()), tag(c, kRecv)))
        ++c->inflight;
    else
        close_conn(c);
}

void EventLoop::start_send(Conn* c) {
    if (c->closing) return;
    const auto* p = reinterpret_cast<const std::byte*>(c->out.data()) + c->out_sent;
    if (r_.submit_send(c->fd, ByteView(p, c->out.size() - c->out_sent), tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void EventLoop::on_recv(Conn* c, int res) {
    if (res <= 0) {
        close_conn(c);
        return;
    }
    c->in.append(reinterpret_cast<const char*>(c->rbuf.data()), static_cast<std::size_t>(res));
    process(c);
}

void EventLoop::process(Conn* c) {
    constexpr std::size_t kMaxLine = 8 * 1024; // bound the command line (ADR-0016)
    while (true) {
        if (c->state == St::set_body) {
            // Stream the data block to disk as it arrives (sync pwrite per chunk; async between).
            while (c->set_remaining > 0 && !c->in.empty()) {
                const std::size_t take =
                    std::min<std::size_t>(c->in.size(), static_cast<std::size_t>(c->set_remaining));
                if (c->sh && !c->set_failed) {
                    const ByteView chunk(reinterpret_cast<const std::byte*>(c->in.data()), take);
                    if (auto st = c->sh->write(chunk); !st) c->set_failed = true;
                }
                c->in.erase(0, take);
                c->set_remaining -= take;
            }
            if (c->set_remaining > 0) break; // need more body
            if (c->in.size() < 2) break;     // need the trailing CRLF
            const bool crlf = c->in[0] == '\r' && c->in[1] == '\n';
            c->in.erase(0, 2);
            std::string_view reply;
            if (!crlf)
                reply = kBadDataChunk;
            else if (c->set_reject || c->set_failed || !c->sh)
                reply = kNotStored;
            else if (auto st = c->sh->commit(c->set_flags); !st)
                reply = kNotStored;
            else
                reply = kStored;
            c->sh.reset();
            if (!c->set_noreply) c->out += reply;
            c->state = St::idle;
            continue;
        }
        if (c->state != St::idle) return; // GET streaming — driven by sends/reads, not here

        const auto eol = c->in.find("\r\n");
        if (eol == std::string::npos) break;
        const auto cmd = parse_command(std::string_view(c->in.data(), eol));
        if (!cmd) {
            c->in.erase(0, eol + 2);
            c->out += kError;
            continue;
        }
        // Copy fields out before erasing — cmd's views point into `in`.
        const Verb verb = cmd->verb;
        const bool noreply = cmd->noreply;
        const std::uint32_t flags = cmd->flags;
        const std::uint64_t nbytes = cmd->bytes;
        const std::string key(cmd->key);
        c->in.erase(0, eol + 2);

        if (verb == Verb::version) {
            c->out += kVersion;
        } else if (verb == Verb::quit) {
            c->quit_after = true;
            break;
        } else if (verb == Verb::del) {
            const bool erased = tm_.remove(crypto::hash_key(key));
            if (!noreply) c->out += (erased ? kDeleted : kNotFound);
        } else if (verb == Verb::get || verb == Verb::gets) {
            const auto digest = crypto::hash_key(key);
            // One consistent snapshot (ADR-0018): metadata + pinned head + open files, captured
            // under one lock so a concurrent copy-on-write replace can't split head from body.
            auto snap = tm_.open_snapshot(digest);
            if (!snap) {
                c->out += kEnd; // miss (or files vanished)
                continue;
            }
            const Size head_len = snap->pin.valid ? snap->pin.len : 0;
            if (snap->meta.size > head_len) { // a disk tail remains -> need an I/O buffer
                auto buf = iobufs_.acquire();
                if (!buf) {
                    if (snap->pin.valid) tm_.unpin_head(snap->pin);
                    close_conn(c); // out of I/O buffers (size the pool; backpressure is a TODO)
                    return;
                }
                c->rs.emplace(std::move(*snap->rs));
                c->iobuf = *buf;
                c->have_iobuf = true;
            }
            c->head_pin = snap->pin;
            c->head_sent = 0;
            c->get_size = snap->meta.size;
            c->get_pos = 0;
            c->out += value_header(key, snap->meta.flags, snap->meta.size);
            c->state = St::get_header;
            break; // stream the value before parsing any further command
        } else if (verb == Verb::set || verb == Verb::add || verb == Verb::replace) {
            const auto digest = crypto::hash_key(key);
            bool reject = (verb == Verb::add && index_.contains(digest)) ||
                          (verb == Verb::replace && !index_.contains(digest));
            c->sh.reset();
            if (!reject) {
                auto h = tm_.begin_store(digest, nbytes);
                if (h)
                    c->sh.emplace(std::move(*h));
                else
                    reject = true; // couldn't open the writer -> NOT_STORED
            }
            c->set_remaining = nbytes;
            c->set_flags = flags;
            c->set_noreply = noreply;
            c->set_reject = reject;
            c->set_failed = false;
            c->state = St::set_body;
            continue; // consume the body in the set_body branch
        } else {
            c->out += kError;
        }
    }
    if (c->state == St::idle && c->in.size() > kMaxLine) {
        close_conn(c);
        return;
    }
    if (!c->out.empty())
        start_send(c);
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

void EventLoop::pump_get(Conn* c) {
    if (c->get_pos >= c->get_size) { // value done -> trailer
        c->out += "\r\n";
        c->out += kEnd;
        c->state = St::get_trailer;
        start_send(c);
        return;
    }
    const Size want = std::min<Size>(c->iobuf.size(), c->get_size - c->get_pos);
    const auto plan = c->rs->plan(c->get_pos, c->iobuf.subspan(0, want));
    c->piece_len = plan.total;
    if (c->piece_len != want) { // couldn't map the whole piece
        abort_get(c);
        return;
    }
    if (plan.segs.empty()) { // all-head piece, already in iobuf
        c->piece_sent = 0;
        c->state = St::get_send_piece;
        start_send_piece(c);
        return;
    }
    c->pending_reads = 0;
    c->read_error = false;
    c->state = St::get_reading;
    for (const auto& s : plan.segs) {
        // O_DIRECT (ADR-0011): the read length must be a device-block multiple. Round up into the
        // (page-aligned, io_chunk-sized) I/O buffer and serve only piece_len bytes; offsets are
        // already 4 KiB-aligned (pieces start on 4 KiB boundaries, segments at stripe boundaries).
        const Size rlen = std::min<Size>(align_up(s.len, kDeviceBlock), c->iobuf.size() - s.out_off);
        if (!r_.submit_read(s.fd, s.file_off, c->iobuf.subspan(s.out_off, rlen), tag(c, kRead))) {
            abort_get(c);
            return;
        }
        ++c->inflight;
        ++c->pending_reads;
    }
}

void EventLoop::on_read(Conn* c, int res) {
    if (res <= 0) c->read_error = true;
    if (c->pending_reads) --c->pending_reads;
    if (c->pending_reads != 0) return;
    if (c->closing) return; // already aborting; just drain the remaining reads
    if (c->read_error) {
        abort_get(c);
        return;
    }
    c->piece_sent = 0;
    c->state = St::get_send_piece;
    start_send_piece(c);
}

void EventLoop::start_send_piece(Conn* c) {
    if (c->closing) return;
    const ByteView piece(c->iobuf.data() + c->piece_sent, c->piece_len - c->piece_sent);
    if (r_.submit_send(c->fd, piece, tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void EventLoop::start_send_head(Conn* c) {
    if (c->closing) return;
    const ByteView head = tm_.pinned_bytes(c->head_pin); // zero-copy: straight from the head pool
    const ByteView rem(head.data() + c->head_sent, c->head_pin.len - c->head_sent);
    if (r_.submit_send(c->fd, rem, tag(c, kSend)))
        ++c->inflight;
    else
        close_conn(c);
}

void EventLoop::on_send(Conn* c, int res) {
    if (res <= 0) {
        close_conn(c);
        return;
    }
    if (c->state == St::get_send_piece) {
        c->piece_sent += static_cast<std::size_t>(res);
        if (c->piece_sent < c->piece_len) {
            start_send_piece(c); // short send -> remainder of the piece
            return;
        }
        c->get_pos += c->piece_len;
        pump_get(c); // next piece, or trailer
        return;
    }
    if (c->state == St::get_send_head) {
        c->head_sent += static_cast<std::size_t>(res);
        if (c->head_sent < c->head_pin.len) {
            start_send_head(c); // short send -> remainder of the head
            return;
        }
        const Size hl = c->head_pin.len;
        unpin_if_held(c); // head fully sent -> release the pin
        c->get_pos = hl;  // stream the disk tail (if any) after the head
        pump_get(c);
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
        if (c->head_pin.valid) { // send the resident head zero-copy straight from the pool
            c->head_sent = 0;
            c->state = St::get_send_head;
            start_send_head(c);
        } else {
            pump_get(c); // no resident head -> stream the value from disk
        }
    } else if (c->state == St::get_trailer)
        finish_get(c);
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

void EventLoop::release_iobuf(Conn* c) {
    if (c->have_iobuf) {
        iobufs_.release(c->iobuf);
        c->have_iobuf = false;
    }
}

void EventLoop::unpin_if_held(Conn* c) {
    if (c->head_pin.valid) {
        tm_.unpin_head(c->head_pin);
        c->head_pin.valid = false;
    }
}

void EventLoop::finish_get(Conn* c) {
    release_iobuf(c);
    unpin_if_held(c);
    c->rs.reset();
    c->state = St::idle;
    process(c); // resume any pipelined commands, or read more
}

void EventLoop::abort_get(Conn* c) {
    release_iobuf(c);
    unpin_if_held(c);
    c->rs.reset();
    close_conn(c);
}

void EventLoop::close_conn(Conn* c) {
    if (c->closing) return;
    c->closing = true;
    ::close(c->fd);
}

void EventLoop::retire(Conn* c) {
    if (c->closing && c->inflight == 0) {
        release_iobuf(c);  // in case we closed mid-GET
        unpin_if_held(c);  // release the head pin if a send was interrupted
        conns_.erase(c);
    }
}

} // namespace goblin::memcache
