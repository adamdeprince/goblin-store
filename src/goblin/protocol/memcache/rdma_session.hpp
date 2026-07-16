// Transport-independent classic memcache session for the native RDMA endpoint.
//
// Control records carry command lines, response framing, and SET trailers only. Value bytes are
// accepted and emitted exclusively through registered bulk windows. Keeping this state machine
// independent of libibverbs gives the lifetime and framing rules a hardware-free test seam.
#pragma once

#include "goblin/common/config.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/core/stats.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goblin::memcache {

class RdmaSessionChannel {
public:
    struct TxWindow {
        unsigned id = 0;
        MutBytes bytes{};
    };

    virtual ~RdmaSessionChannel() = default;
    virtual std::size_t inline_capacity() const noexcept = 0;
    virtual bool try_send_inline(std::string_view bytes) noexcept = 0;
    virtual std::optional<TxWindow> acquire_tx_window() noexcept = 0;
    // Success transfers the reservation to the transport until its signalled data WQE completes.
    virtual bool try_send_tx_window(unsigned id, std::size_t length) noexcept = 0;
    virtual void release_tx_window(unsigned id) noexcept = 0;
};

class RdmaSession {
public:
    enum class ConsumeResult { consumed, blocked, fatal };

    struct LifetimeCounters {
        std::uint64_t bulk_records_received = 0;
        std::uint64_t bulk_records_released_after_write = 0;
        std::uint64_t bulk_records_sent = 0;
        std::uint64_t tail_prefetches_after_first_head = 0;
    };

    RdmaSession(storage::TierManager& tm, storage::Index& index, core::Reactor* reactor,
                core::Stats& stats, core::StatsRegistry* registry, RdmaSessionChannel& channel,
                WriteMode write_mode = WriteMode::evict);
    ~RdmaSession();

    RdmaSession(const RdmaSession&) = delete;
    RdmaSession& operator=(const RdmaSession&) = delete;

    // The transport must retain a READY/RX slot while this returns blocked. For a consumed bulk
    // record, StoreHandle::write has already returned before this method returns, so emitting the
    // cumulative RELEASE afterwards cannot race the storage copy.
    ConsumeResult accept_inline(std::string_view bytes);
    ConsumeResult accept_bulk(ByteView bytes);

    // Drive retries, response framing, head copies, tail prefetch, and bulk sends until the channel
    // backpressures. Returns true when it made observable progress.
    bool drive();

    bool failed() const noexcept { return failed_; }
    bool close_requested() const noexcept { return close_requested_; }
    std::string_view error() const noexcept { return error_; }
    const LifetimeCounters& lifetime_counters() const noexcept { return counters_; }

private:
    enum class State { idle, set_wait, set_body, set_trailer, get_header, get_body };

    struct PreparedWindow {
        RdmaSessionChannel::TxWindow window;
        std::size_t length = 0;
        Size object_offset = 0;
    };

    bool flush_inline();
    bool process_input();
    bool retry_store();
    bool drive_get();
    bool prepare_next_get_piece();
    bool prefetch_first_tail();
    void finish_store_trailer();
    void finish_get();
    void reset_get();
    void queue(std::string_view bytes);
    void fail(std::string message);
    std::string format_stats() const;

    storage::TierManager& tm_;
    storage::Index& index_;
    core::Reactor* reactor_;
    core::Stats& stats_;
    core::StatsRegistry* registry_;
    RdmaSessionChannel& channel_;
    WriteMode write_mode_;

    State state_ = State::idle;
    std::string input_;
    std::string output_;
    std::size_t output_offset_ = 0;
    bool failed_ = false;
    bool close_requested_ = false;
    std::string error_;

    crypto::Digest set_digest_{};
    Size set_size_ = 0;
    Size set_remaining_ = 0;
    std::uint32_t set_flags_ = 0;
    std::uint32_t set_exptime_ = 0;
    std::uint64_t set_cas_ = 0;
    bool set_noreply_ = false;
    bool set_reject_ = false;
    bool set_failed_ = false;
    std::string set_reply_;
    std::optional<storage::TierManager::StoreHandle> store_;

    std::optional<storage::TierManager::Snapshot> snapshot_;
    Size get_pos_ = 0;
    Size get_head_len_ = 0;
    bool first_head_sent_ = false;
    std::optional<PreparedWindow> prepared_;
    std::optional<PreparedWindow> prefetched_tail_;

    LifetimeCounters counters_{};
};

} // namespace goblin::memcache
