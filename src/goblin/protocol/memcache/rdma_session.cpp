#include "goblin/protocol/memcache/rdma_session.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <limits>
#include <unistd.h>

namespace goblin::memcache {
namespace {

constexpr std::size_t kMaxLine = 8 * 1024;
constexpr auto rlx = std::memory_order_relaxed;

} // namespace

RdmaSession::RdmaSession(storage::TierManager& tm, storage::Index& index,
                         core::Reactor* reactor, core::Stats& stats,
                         core::StatsRegistry* registry, RdmaSessionChannel& channel,
                         WriteMode write_mode)
    : tm_(tm), index_(index), reactor_(reactor), stats_(stats), registry_(registry),
      channel_(channel), write_mode_(write_mode) {}

RdmaSession::~RdmaSession() {
    reset_get();
    store_.reset();
}

void RdmaSession::fail(std::string message) {
    if (!failed_) error_ = std::move(message);
    failed_ = true;
}

void RdmaSession::queue(std::string_view bytes) {
    if (bytes.empty()) return;
    if (output_offset_ == output_.size()) {
        output_.clear();
        output_offset_ = 0;
    }
    output_.append(bytes);
}

bool RdmaSession::flush_inline() {
    bool progress = false;
    const std::size_t capacity = channel_.inline_capacity();
    if (capacity == 0) {
        fail("RDMA channel reports zero inline capacity");
        return false;
    }
    while (output_offset_ < output_.size()) {
        const std::size_t length = std::min(capacity, output_.size() - output_offset_);
        const std::string_view piece(output_.data() + output_offset_, length);
        if (!channel_.try_send_inline(piece)) break;
        output_offset_ += length;
        stats_.bytes_served.fetch_add(length, rlx);
        progress = true;
    }
    if (output_offset_ == output_.size()) {
        output_.clear();
        output_offset_ = 0;
    }
    return progress;
}

RdmaSession::ConsumeResult RdmaSession::accept_inline(std::string_view bytes) {
    if (failed_ || close_requested_ || bytes.empty()) return ConsumeResult::fatal;
    // A GET response and an ordinary one-line response are serialized before the next request.
    // SET is the exception: its trailer is an inline record following one or more READY records.
    if ((state_ == State::get_header || state_ == State::get_body) ||
        (!output_.empty() && state_ != State::set_trailer))
        return ConsumeResult::blocked;
    if (state_ == State::set_body && set_remaining_ != 0) {
        fail("SET value bytes arrived inline; v3 requires BULK_READY body records");
        return ConsumeResult::fatal;
    }
    try {
        input_.append(bytes);
    } catch (const std::bad_alloc&) {
        fail("allocate RDMA memcache input buffer");
        return ConsumeResult::fatal;
    }
    (void)process_input();
    return failed_ ? ConsumeResult::fatal : ConsumeResult::consumed;
}

RdmaSession::ConsumeResult RdmaSession::accept_bulk(ByteView bytes) {
    if (failed_ || close_requested_ || bytes.empty()) return ConsumeResult::fatal;
    if (state_ == State::set_wait) return ConsumeResult::blocked;
    if (state_ != State::set_body || set_remaining_ == 0) {
        fail("bulk value record arrived outside a SET body");
        return ConsumeResult::fatal;
    }
    if (bytes.size() > set_remaining_) {
        fail("bulk SET record exceeds the declared value length");
        return ConsumeResult::fatal;
    }

    ++counters_.bulk_records_received;
    bool stored = false;
    if (store_ && !set_failed_) {
        // This synchronous copy/write is the lifetime boundary: only after it returns may the
        // transport pop READY and publish a cumulative RELEASE for the remote RX slot.
        if (auto status = store_->write(bytes); !status) {
            set_failed_ = true;
            store_.reset();
        } else {
            stored = true;
        }
    }
    set_remaining_ -= bytes.size();
    if (stored) stats_.bytes_stored.fetch_add(bytes.size(), rlx);
    ++counters_.bulk_records_released_after_write;
    if (set_remaining_ == 0) state_ = State::set_trailer;
    return ConsumeResult::consumed;
}

bool RdmaSession::retry_store() {
    if (state_ != State::set_wait) return false;
    auto handle = tm_.begin_store(set_digest_, set_size_, write_mode_);
    if (!handle) {
        if (handle.error().code == Errc::would_block) return false;
        set_reject_ = true;
        set_failed_ = true;
        state_ = set_remaining_ == 0 ? State::set_trailer : State::set_body;
        return true;
    }
    store_.emplace(std::move(*handle));
    state_ = set_remaining_ == 0 ? State::set_trailer : State::set_body;
    return true;
}

void RdmaSession::finish_store_trailer() {
    std::string_view reply;
    if (set_reject_) {
        reply = set_reply_;
    } else if (!store_ || set_failed_) {
        reply = kNotStored;
    } else {
        const auto committed = store_->commit(
            set_flags_, exptime_to_expiry(set_exptime_, storage::now_unix()), set_cas_);
        if (!committed)
            reply = committed.error().code == Errc::cas_mismatch ? kExists : kNotStored;
        else
            reply = kStored;
    }
    store_.reset();
    if (reply == kStored) {
        stats_.sets.fetch_add(1, rlx);
    } else {
        stats_.set_rejected.fetch_add(1, rlx);
    }
    if (!set_noreply_) queue(reply);
    state_ = State::idle;
}

bool RdmaSession::process_input() {
    bool progress = false;
    for (;;) {
        if (failed_ || close_requested_) return progress;
        if (state_ == State::set_wait || state_ == State::set_body ||
            state_ == State::get_header || state_ == State::get_body)
            return progress;
        if (!output_.empty()) return progress;

        if (state_ == State::set_trailer) {
            if (input_.size() < 2) return progress;
            if (input_[0] != '\r' || input_[1] != '\n') {
                fail("SET body is not followed by an inline CRLF trailer");
                return progress;
            }
            input_.erase(0, 2);
            finish_store_trailer();
            progress = true;
            continue;
        }

        const std::size_t eol = input_.find("\r\n");
        if (eol == std::string::npos) {
            if (input_.size() > kMaxLine) fail("memcache command line exceeds 8192 bytes");
            return progress;
        }
        if (eol > kMaxLine) {
            fail("memcache command line exceeds 8192 bytes");
            return progress;
        }
        const auto parsed = parse_command(std::string_view(input_.data(), eol));
        if (!parsed) {
            input_.erase(0, eol + 2);
            queue(kError);
            progress = true;
            continue;
        }

        // Command::key views input_, so copy/use everything before erasing the line.
        const Verb verb = parsed->verb;
        const std::string key(parsed->key);
        const std::uint32_t flags = parsed->flags;
        const std::uint32_t exptime = parsed->exptime;
        const Size bytes = parsed->bytes;
        const std::uint64_t cas = parsed->cas;
        const bool noreply = parsed->noreply;
        input_.erase(0, eol + 2);
        progress = true;

        if (verb == Verb::quit) {
            close_requested_ = true;
            return progress;
        }
        if (verb == Verb::version) {
            queue(kVersion);
            continue;
        }
        if (verb == Verb::stats) {
            queue(format_stats());
            continue;
        }
        const auto digest = crypto::hash_key(key);
        if (verb == Verb::del) {
            const bool erased = tm_.remove(digest);
            if (!noreply) queue(erased ? kDeleted : kNotFound);
            continue;
        }
        if (verb == Verb::get || verb == Verb::gets) {
            snapshot_ = tm_.open_snapshot(digest, /*record_access=*/true, storage::now_unix());
            if (!snapshot_) {
                stats_.get_misses.fetch_add(1, rlx);
                queue(kEnd);
                continue;
            }
            tm_.touch_policies(digest, snapshot_->meta.size, snapshot_->pin.valid);
            stats_.get_hits.fetch_add(1, rlx);
            if (verb == Verb::gets)
                queue(value_header_cas(key, snapshot_->meta.flags, snapshot_->meta.size,
                                       snapshot_->meta.etag));
            else
                queue(value_header(key, snapshot_->meta.flags, snapshot_->meta.size));
            get_pos_ = 0;
            get_head_len_ = snapshot_->pin.valid ? snapshot_->pin.len : 0;
            first_head_sent_ = false;
            state_ = State::get_header;
            return progress;
        }
        if (verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
            verb == Verb::cas) {
            bool reject = false;
            std::string_view reject_reply = kNotStored;
            std::uint64_t cas_check = 0;
            if (verb != Verb::set) {
                const auto existing = index_.lookup(digest);
                const bool present = existing &&
                    !storage::is_expired(*existing, storage::now_unix());
                if (verb == Verb::add && present) reject = true;
                else if (verb == Verb::replace && !present) reject = true;
                else if (verb == Verb::cas) {
                    if (!present) { reject = true; reject_reply = kNotFound; }
                    else if (existing->etag != cas) { reject = true; reject_reply = kExists; }
                    else cas_check = cas;
                }
            }
            set_digest_ = digest;
            set_size_ = bytes;
            set_remaining_ = bytes;
            set_flags_ = flags;
            set_exptime_ = exptime;
            set_cas_ = cas_check;
            set_noreply_ = noreply;
            set_reject_ = reject;
            set_failed_ = false;
            set_reply_.assign(reject_reply);
            store_.reset();
            if (!reject) {
                auto handle = tm_.begin_store(digest, bytes, write_mode_);
                if (handle) {
                    store_.emplace(std::move(*handle));
                } else if (handle.error().code == Errc::would_block) {
                    stats_.set_backpressure.fetch_add(1, rlx);
                    state_ = State::set_wait;
                    return progress;
                } else {
                    set_reject_ = true;
                    set_failed_ = true;
                }
            }
            state_ = bytes == 0 ? State::set_trailer : State::set_body;
            return progress;
        }
        queue(kError);
    }
}

bool RdmaSession::prefetch_first_tail() {
    if (!snapshot_ || !snapshot_->rs || prefetched_tail_ ||
        get_head_len_ >= snapshot_->meta.size || !reactor_)
        return false;
    auto window = channel_.acquire_tx_window();
    if (!window) return false;
    const Size logical = std::min<Size>(window->bytes.size(),
                                       snapshot_->meta.size - get_head_len_);
    auto read = snapshot_->rs->read(*reactor_, get_head_len_, window->bytes);
    if (!read || *read != logical) {
        channel_.release_tx_window(window->id);
        fail(read ? "short RDMA tail prefetch" : read.error().detail);
        return false;
    }
    prefetched_tail_.emplace(PreparedWindow{*window, static_cast<std::size_t>(logical),
                                             get_head_len_});
    ++counters_.tail_prefetches_after_first_head;
    return true;
}

bool RdmaSession::prepare_next_get_piece() {
    if (!snapshot_ || prepared_ || get_pos_ >= snapshot_->meta.size) return false;
    if (prefetched_tail_ && prefetched_tail_->object_offset == get_pos_) {
        prepared_.emplace(std::move(*prefetched_tail_));
        prefetched_tail_.reset();
        return true;
    }
    auto window = channel_.acquire_tx_window();
    if (!window) return false;
    const Size available = std::min<Size>(window->bytes.size(), snapshot_->meta.size - get_pos_);
    Size length = 0;
    if (get_pos_ < get_head_len_) {
        length = std::min<Size>(available, get_head_len_ - get_pos_);
        const ByteView head = tm_.pinned_bytes(snapshot_->pin);
        std::memcpy(window->bytes.data(), head.data() + get_pos_, static_cast<std::size_t>(length));
    } else {
        if (!snapshot_->rs || !reactor_) {
            channel_.release_tx_window(window->id);
            fail("RDMA disk tail requires io_uring support");
            return false;
        }
        auto read = snapshot_->rs->read(*reactor_, get_pos_, window->bytes);
        if (!read || *read != available) {
            channel_.release_tx_window(window->id);
            fail(read ? "short RDMA tail read" : read.error().detail);
            return false;
        }
        length = available;
    }
    prepared_.emplace(PreparedWindow{*window, static_cast<std::size_t>(length), get_pos_});
    return true;
}

void RdmaSession::reset_get() {
    if (prepared_) {
        channel_.release_tx_window(prepared_->window.id);
        prepared_.reset();
    }
    if (prefetched_tail_) {
        channel_.release_tx_window(prefetched_tail_->window.id);
        prefetched_tail_.reset();
    }
    if (snapshot_ && snapshot_->pin.valid) {
        tm_.unpin_head(snapshot_->pin);
        snapshot_->pin.valid = false;
    }
    snapshot_.reset();
    get_pos_ = 0;
    get_head_len_ = 0;
    first_head_sent_ = false;
}

void RdmaSession::finish_get() {
    reset_get();
    queue("\r\nEND\r\n");
    state_ = State::idle;
}

bool RdmaSession::drive_get() {
    if (!snapshot_) {
        fail("RDMA GET state lost its storage snapshot");
        return false;
    }
    if (get_pos_ == snapshot_->meta.size) {
        finish_get();
        return true;
    }
    if (!prepared_ && !prepare_next_get_piece()) return false;
    if (failed_ || !prepared_) return false;
    const bool was_head = prepared_->object_offset < get_head_len_;
    const std::size_t length = prepared_->length;
    if (!channel_.try_send_tx_window(prepared_->window.id, length)) return false;
    get_pos_ += length;
    stats_.bytes_served.fetch_add(length, rlx);
    ++counters_.bulk_records_sent;
    prepared_.reset(); // transport now owns the source slot until its signalled data CQE

    // The first head data WQE is already on the RC send queue. Before waiting for the head to
    // drain—or copying the rest of a multi-window head—start the disk read into another registered
    // window. The NIC and io_uring therefore run concurrently from the first body bytes.
    if (was_head && !first_head_sent_) {
        first_head_sent_ = true;
        (void)prefetch_first_tail();
    }
    if (get_pos_ >= get_head_len_ && snapshot_->pin.valid) {
        tm_.unpin_head(snapshot_->pin);
        snapshot_->pin.valid = false;
    }
    if (get_pos_ == snapshot_->meta.size) finish_get();
    return true;
}

bool RdmaSession::drive() {
    if (failed_ || close_requested_) return false;
    bool any = false;
    for (unsigned turns = 0; turns < 64 && !failed_ && !close_requested_; ++turns) {
        bool progress = false;
        if (state_ == State::set_wait) progress |= retry_store();
        progress |= flush_inline();
        if (!output_.empty()) {
            any |= progress;
            if (!progress) break;
            continue;
        }
        if (state_ == State::get_header) {
            state_ = State::get_body;
            progress = true;
        }
        if (state_ == State::get_body) progress |= drive_get();
        if (state_ == State::idle || state_ == State::set_trailer)
            progress |= process_input();
        any |= progress;
        if (!progress) break;
    }
    return any;
}

std::string RdmaSession::format_stats() const {
    const core::StatsSnapshot stats = registry_ ? registry_->aggregate() : stats_.snapshot();
    const std::uint64_t uptime = registry_ ? registry_->uptime_secs() : 0;
    std::string out;
    auto line = [&out](std::string_view name, std::uint64_t value) {
        out += std::format("STAT {} {}\r\n", name, value);
    };
    out += std::format("STAT pid {}\r\n", ::getpid());
    line("uptime", uptime);
    out += "STAT version goblin-store 0.0.2\r\n";
    line("curr_connections", stats.curr_conns);
    line("total_connections", stats.conns);
    line("cmd_get", stats.get_hits + stats.get_misses);
    line("cmd_set", stats.sets + stats.set_rejected);
    line("get_hits", stats.get_hits);
    line("get_misses", stats.get_misses);
    line("sets_stored", stats.sets);
    line("sets_rejected", stats.set_rejected);
    line("bytes_served", stats.bytes_served);
    line("bytes_stored", stats.bytes_stored);
    line("rdma_bulk_rx", counters_.bulk_records_received);
    line("rdma_bulk_tx", counters_.bulk_records_sent);
    const auto promotion = tm_.numa_promotion_stats();
    line("numa_promotions", promotion.count);
    line("numa_promotion_bytes", promotion.bytes_moved);
    out += kEnd;
    return out;
}

} // namespace goblin::memcache
