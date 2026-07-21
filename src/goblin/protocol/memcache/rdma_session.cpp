#include "goblin/protocol/memcache/rdma_session.hpp"
#include "goblin/protocol/memcache/stats_format.hpp"

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

RdmaSession::RdmaSession(storage::TierManager& tm, storage::Index&,
                         core::Reactor* reactor, core::Stats& stats,
                         core::StatsRegistry* registry, RdmaSessionChannel& channel,
                         WriteMode write_mode)
    : tm_(tm), reactor_(reactor), stats_(stats), registry_(registry),
      channel_(channel), write_mode_(write_mode) {}

RdmaSession::~RdmaSession() {
    reset_get();
    release_set_source();
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
        if (request_started_ != std::chrono::steady_clock::time_point{}) {
            stats_.ttfb.observe(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - request_started_).count()));
            request_started_ = {};
        }
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
    stats_.bytes_received.fetch_add(bytes.size(), rlx);
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
    stats_.bytes_received.fetch_add(bytes.size(), rlx);
    bool stored = false;
    if (store_ && !set_failed_) {
        // This synchronous copy/write is the lifetime boundary: only after it returns may the
        // transport pop READY and publish a cumulative RELEASE for the remote RX slot.
        if (auto status = store_->write(bytes); !status) {
            set_failed_ = true;
            set_reply_.assign(storage_failure_reply(status.error().code));
            store_.reset();
        } else {
            stored = true;
        }
    }
    set_remaining_ -= bytes.size();
    if (stored) stats_.bytes_stored.fetch_add(bytes.size(), rlx);
    ++counters_.bulk_records_released_after_write;
    if (set_remaining_ == 0) {
        if (set_mode_ == 'P') (void)copy_mutation_source();
        state_ = State::set_trailer;
    }
    return ConsumeResult::consumed;
}

void RdmaSession::release_set_source() {
    if (set_source_ && set_source_->pin.valid) {
        tm_.unpin_head(set_source_->pin);
        set_source_->pin.valid = false;
    }
    set_source_.reset();
}

bool RdmaSession::copy_mutation_source() {
    if (set_source_copied_ || !set_source_) return true;
    if (!store_ || set_failed_) {
        release_set_source();
        return false;
    }
    const auto copied = tm_.copy_snapshot(*store_, *set_source_);
    set_source_copied_ = true;
    release_set_source();
    if (copied) return true;
    set_failed_ = true;
    set_reply_.assign(storage_failure_reply(copied.error().code));
    store_.reset();
    return false;
}

bool RdmaSession::retry_store() {
    if (state_ != State::set_wait) return false;
    auto handle = tm_.begin_store(set_digest_, set_size_, write_mode_, set_condition_);
    if (!handle) {
        if (handle.error().code == Errc::would_block) return false;
        set_reject_ = true;
        set_failed_ = true;
        set_reply_.assign(storage_failure_reply(handle.error().code));
        state_ = set_remaining_ == 0 ? State::set_trailer : State::set_body;
        return true;
    }
    store_.emplace(std::move(*handle));
    if (set_mode_ == 'A') (void)copy_mutation_source();
    state_ = set_remaining_ == 0 ? State::set_trailer : State::set_body;
    return true;
}

void RdmaSession::finish_store_trailer() {
    if (set_mode_ == 'P') (void)copy_mutation_source();
    std::string_view reply;
    if (set_reject_) {
        reply = set_reply_;
    } else if (!store_ || set_failed_) {
        reply = set_reply_;
    } else {
        const auto committed = store_->commit(set_flags_, set_expiry_, set_cas_);
        if (!committed)
            reply = (set_mode_ == 'A' || set_mode_ == 'P') &&
                            committed.error().code == Errc::cas_mismatch
                        ? kNotStored : commit_failure_reply(committed.error().code, false);
        else
            reply = kStored;
    }
    store_.reset();
    release_set_source();
    if (reply == kStored) {
        stats_.sets.fetch_add(1, rlx);
    } else {
        stats_.set_rejected.fetch_add(1, rlx);
    }
    const bool condition = reply == kNotStored || reply == kExists || reply == kNotFound;
    stats_.note_command(set_command_,
        reply == kStored ? core::CommandResult::success
        : set_command_ == core::CommandKind::cas && reply == kNotFound
            ? core::CommandResult::miss
        : condition ? core::CommandResult::condition : core::CommandResult::error);
    if (!set_noreply_) queue(reply);
    state_ = State::idle;
}

bool RdmaSession::continue_get_batch() {
    while (get_batch_next_ < get_batch_keys_.size()) {
        const std::string& key = get_batch_keys_[get_batch_next_++];
        const auto digest = crypto::hash_key(key);
        if (get_batch_expiry_)
            tm_.touch_ttl(digest, *get_batch_expiry_, storage::now_unix());
        snapshot_ = tm_.open_snapshot(digest, /*record_access=*/true, storage::now_unix());
        if (!snapshot_) {
            stats_.get_misses.fetch_add(1, rlx);
            continue; // classic retrieval responses omit misses
        }
        tm_.touch_policies(digest, snapshot_->meta.size, snapshot_->pin.valid);
        stats_.get_hits.fetch_add(1, rlx);
        if (get_batch_with_cas_)
            queue(value_header_cas(key, snapshot_->meta.flags, snapshot_->meta.size,
                                   snapshot_->meta.etag));
        else
            queue(value_header(key, snapshot_->meta.flags, snapshot_->meta.size));
        get_pos_ = 0;
        get_head_len_ = snapshot_->pin.valid ? snapshot_->pin.len : 0;
        first_head_sent_ = false;
        state_ = State::get_header;
        return true;
    }
    queue(kEnd);
    get_batch_keys_.clear();
    get_batch_next_ = 0;
    get_batch_active_ = false;
    get_batch_expiry_.reset();
    state_ = State::idle;
    return true;
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
            queue(command_parse_error_reply(parsed.error()));
            progress = true;
            continue;
        }

        // Command::key views input_, so copy/use everything before erasing the line.
        const Verb verb = parsed->verb;
        const std::string key(parsed->key);
        std::vector<std::string> extra_keys;
        extra_keys.reserve(parsed->extra_keys.size());
        for (const std::string_view extra : parsed->extra_keys) extra_keys.emplace_back(extra);
        const std::uint32_t flags = parsed->flags;
        const std::int64_t exptime = parsed->exptime;
        const Size bytes = parsed->bytes;
        const std::uint64_t cas = parsed->cas;
        const std::uint64_t delta = parsed->delta;
        const std::uint32_t delay = parsed->delay;
        const bool noreply = parsed->noreply;
        if (verb == Verb::get || verb == Verb::gets || verb == Verb::gat || verb == Verb::gats)
            request_started_ = std::chrono::steady_clock::now();
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
            if (parsed->stats_verb == StatsVerb::reset) {
                if (registry_) registry_->reset();
                else stats_.reset();
                queue(kReset);
            } else if (parsed->stats_verb == StatsVerb::settings) {
                queue(format_settings_response(registry_ ? registry_->settings()
                                                         : core::StatsSettings{}));
            } else {
                queue(format_stats());
            }
            continue;
        }
        if (verb == Verb::touch) {
            const bool touched = tm_.touch_ttl(crypto::hash_key(key),
                exptime_to_expiry(exptime, storage::now_unix()));
            stats_.note_command(core::CommandKind::touch, touched ? core::CommandResult::success
                                                                 : core::CommandResult::miss);
            if (!noreply) queue(touched ? kTouched : kNotFound);
            continue;
        }
        if (verb == Verb::incr || verb == Verb::decr) {
            auto result = tm_.arithmetic(crypto::hash_key(key), delta, verb == Verb::decr,
                                         std::nullopt, std::nullopt, 0, write_mode_);
            stats_.note_command(verb == Verb::decr ? core::CommandKind::decr
                                                   : core::CommandKind::incr,
                result ? core::CommandResult::success
                : result.error().code == Errc::not_found ? core::CommandResult::miss
                                                         : core::CommandResult::error);
            if (!noreply) {
                if (result) {
                    queue(std::to_string(result->value));
                    queue("\r\n");
                } else if (result.error().code == Errc::not_found) queue(kNotFound);
                else if (result.error().code == Errc::invalid_argument) queue(kClientErrorNonNumeric);
                else queue(storage_failure_reply(result.error().code));
            }
            continue;
        }
        if (verb == Verb::flush_all) {
            tm_.flush_all(delay);
            stats_.note_command(core::CommandKind::flush, core::CommandResult::success);
            if (!noreply) queue(kOk);
            continue;
        }
        if (verb == Verb::get || verb == Verb::gets ||
            verb == Verb::gat || verb == Verb::gats) {
            const bool with_cas = verb == Verb::gets || verb == Verb::gats;
            const std::optional<std::uint32_t> touch_expiry =
                (verb == Verb::gat || verb == Verb::gats)
                    ? std::optional(exptime_to_expiry(exptime, storage::now_unix())) : std::nullopt;
            if (!extra_keys.empty()) {
                get_batch_keys_.clear();
                get_batch_keys_.reserve(1 + extra_keys.size());
                get_batch_keys_.push_back(key);
                for (std::string& extra : extra_keys)
                    get_batch_keys_.push_back(std::move(extra));
                get_batch_next_ = 0;
                get_batch_active_ = true;
                get_batch_with_cas_ = with_cas;
                get_batch_expiry_ = touch_expiry;
                (void)continue_get_batch();
                return progress;
            }
            const auto digest = crypto::hash_key(key);
            if (touch_expiry)
                tm_.touch_ttl(digest, *touch_expiry, storage::now_unix());
            snapshot_ = tm_.open_snapshot(digest, /*record_access=*/true, storage::now_unix());
            if (!snapshot_) {
                stats_.get_misses.fetch_add(1, rlx);
                queue(kEnd);
                continue;
            }
            tm_.touch_policies(digest, snapshot_->meta.size, snapshot_->pin.valid);
            stats_.get_hits.fetch_add(1, rlx);
            if (with_cas)
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
        const auto digest = crypto::hash_key(key);
        if (verb == Verb::del) {
            const bool erased = tm_.remove(digest);
            stats_.note_command(core::CommandKind::delete_, erased ? core::CommandResult::success
                                                                  : core::CommandResult::miss);
            if (!noreply) queue(erased ? kDeleted : kNotFound);
            continue;
        }
        if (verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
            verb == Verb::append || verb == Verb::prepend || verb == Verb::cas) {
            bool reject = false;
            Errc admission_error = Errc::ok;
            std::string_view reject_reply = kNotStored;
            std::uint64_t cas_check = 0;
            storage::StoreCondition condition = storage::StoreCondition::unconditional;
            release_set_source();
            const bool mutation = verb == Verb::append || verb == Verb::prepend;
            set_command_ = [&] {
                switch (verb) {
                    case Verb::add: return core::CommandKind::add;
                    case Verb::replace: return core::CommandKind::replace;
                    case Verb::append: return core::CommandKind::append;
                    case Verb::prepend: return core::CommandKind::prepend;
                    case Verb::cas: return core::CommandKind::cas;
                    default: return core::CommandKind::set;
                }
            }();
            if (mutation)
                set_source_ = tm_.open_snapshot(digest, /*record_access=*/false,
                                                storage::now_unix());
            if (verb == Verb::add) condition = storage::StoreCondition::add;
            else if (verb == Verb::replace || mutation)
                condition = storage::StoreCondition::replace;
            const auto existing = set_source_ ? std::optional(set_source_->meta) :
                                                tm_.lookup_live(digest);
            if (verb != Verb::set) {
                const bool present = existing.has_value();
                if (verb == Verb::add && present) reject = true;
                else if (verb == Verb::replace && !present) reject = true;
                else if (mutation && !present) reject = true;
                else if (verb == Verb::cas) {
                    if (!present) { reject = true; reject_reply = kNotFound; }
                    else if (existing->etag != cas) { reject = true; reject_reply = kExists; }
                    else cas_check = cas;
                }
            }
            Size store_size = bytes;
            std::uint32_t store_flags = flags;
            std::uint32_t store_expiry = exptime_to_expiry(exptime, storage::now_unix());
            if (!reject && mutation) {
                if (existing->size > std::numeric_limits<Size>::max() - bytes) {
                    reject = true;
                    admission_error = Errc::too_large;
                }
                else store_size = existing->size + bytes;
                store_flags = existing->flags;
                store_expiry = existing->expiry;
                cas_check = existing->etag;
            }
            set_digest_ = digest;
            set_size_ = store_size;
            set_remaining_ = bytes;
            set_flags_ = store_flags;
            set_expiry_ = store_expiry;
            set_cas_ = cas_check;
            set_mode_ = verb == Verb::append ? 'A' : verb == Verb::prepend ? 'P' : 'S';
            set_source_copied_ = false;
            set_condition_ = condition;
            set_noreply_ = noreply;
            set_reject_ = reject;
            set_failed_ = admission_error != Errc::ok;
            set_reply_.assign(admission_error == Errc::ok
                ? reject_reply : storage_failure_reply(admission_error));
            store_.reset();
            if (!reject) {
                auto handle = tm_.begin_store(digest, store_size, write_mode_, condition);
                if (handle) {
                    store_.emplace(std::move(*handle));
                    if (set_mode_ == 'A') (void)copy_mutation_source();
                } else if (handle.error().code == Errc::would_block) {
                    stats_.set_backpressure.fetch_add(1, rlx);
                    state_ = State::set_wait;
                    return progress;
                } else {
                    set_reject_ = true;
                    set_failed_ = true;
                    set_reply_.assign(storage_failure_reply(handle.error().code));
                }
            }
            if (reject) release_set_source();
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
    state_ = State::idle;
    queue("\r\n");
    if (get_batch_active_)
        (void)continue_get_batch();
    else
        queue(kEnd);
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
    const core::StatsSnapshot stats =
        registry_ ? registry_->aggregate(core::StatsSelection::memcache) : stats_.snapshot();
    const std::uint64_t uptime = registry_ ? registry_->uptime_secs() : 0;
    auto out = format_stats_response(tm_, stats, uptime);
    const auto end = out.rfind(kEnd);
    if (end != std::string::npos) {
        out.insert(end, std::format("STAT rdma_bulk_rx {}\r\nSTAT rdma_bulk_tx {}\r\n",
                                   counters_.bulk_records_received,
                                   counters_.bulk_records_sent));
    }
    return out;
}

} // namespace goblin::memcache
