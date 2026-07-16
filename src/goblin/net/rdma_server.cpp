#include "goblin/net/rdma_server.hpp"

#include "goblin/core/stats.hpp"
#include "goblin/protocol/memcache/rdma_session.hpp"
#include "goblin/store/rdma_wire.hpp"

#ifndef GOBLIN_HAVE_RDMA
#define GOBLIN_HAVE_RDMA 0
#endif

#if GOBLIN_HAVE_RDMA

#if !defined(__linux__)
#error "GOBLIN_HAVE_RDMA is only supported on Linux"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace goblin::net {
namespace {

namespace wire = store::rdma_wire;

constexpr std::uint64_t kControlSequenceLimit = std::uint64_t{1} << 56;
constexpr std::uint64_t kWrTagMask = std::uint64_t{3} << 62;
constexpr std::uint64_t kWrSequenceMask = ~kWrTagMask;
constexpr std::uint64_t kWrWrite = std::uint64_t{1} << 62;
constexpr std::uint64_t kWrCreditRead = std::uint64_t{2} << 62;
constexpr std::uint64_t kWrBulkWrite = std::uint64_t{3} << 62;
constexpr std::uint64_t kSignalEvery = 32;
constexpr std::uint32_t kSendQueueDepth = 512;
constexpr std::uint32_t kCqDepth = 1024;

static_assert(wire::kControlRecordBytes + sizeof(std::uint64_t) <= 220);
static_assert(wire::kControlCommitOffset % alignof(std::uint64_t) == 0);

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__loongarch__)
    asm volatile("dbar 0" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

std::size_t align_to(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

std::string errno_message(std::string_view operation, int error = errno) {
    return std::string(operation) + ": " + std::strerror(error);
}

bool nonblocking(int fd, std::string& error) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = errno_message("make RDMA-CM event channel nonblocking");
        return false;
    }
    return true;
}

bool compute_control_geometry(Size requested, std::uint32_t& slots, std::size_t& mapped,
                              std::string& error) {
    const std::uint64_t count = requested / wire::kControlSlotStride;
    if (count < 2 || count > std::numeric_limits<std::uint32_t>::max()) {
        error = "RDMA ring must hold between 2 and 2^32-1 slots";
        return false;
    }
    const std::uint64_t raw = wire::kControlRegionHeaderBytes +
                              count * wire::kControlSlotStride;
    const long page_result = ::sysconf(_SC_PAGESIZE);
    if (page_result <= 0) {
        error = errno_message("read native page size");
        return false;
    }
    const auto page = static_cast<std::size_t>(page_result);
    if (raw > std::numeric_limits<std::uint32_t>::max() ||
        raw > std::numeric_limits<std::size_t>::max() - (page - 1)) {
        error = "RDMA ring allocation exceeds the v3 descriptor limit";
        return false;
    }
    mapped = align_to(static_cast<std::size_t>(raw), page);
    if (mapped > std::numeric_limits<std::uint32_t>::max()) {
        error = "page-aligned RDMA ring exceeds the v3 descriptor limit";
        return false;
    }
    slots = static_cast<std::uint32_t>(count);
    return true;
}

struct Mapping {
    std::byte* data = nullptr;
    std::size_t size = 0;

    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept : data(other.data), size(other.size) {
        other.data = nullptr;
        other.size = 0;
    }
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            reset();
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
    ~Mapping() { reset(); }

    bool allocate(std::size_t bytes, std::string& error, std::string_view name) {
        void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) {
            error = errno_message(std::string("mmap ") + std::string(name));
            return false;
        }
        data = static_cast<std::byte*>(mapped);
        size = bytes;
        std::memset(data, 0, size); // first-touch on the server's selected NUMA CPU set
        return true;
    }
    void reset() noexcept {
        if (data) (void)::munmap(data, size);
        data = nullptr;
        size = 0;
    }
};

class Connection final : public memcache::RdmaSessionChannel {
public:
    static std::shared_ptr<Connection> create(
        rdma_cm_id* id, wire::Descriptor remote, const RdmaConfig& config, WriteMode write_mode,
        storage::TierManager& tm, storage::Index& index, core::StatsRegistry& registry,
        const std::atomic<bool>& shutdown, const std::atomic<bool>& listener_stop,
        std::string& error) {
        auto result = std::shared_ptr<Connection>(new Connection(
            id, remote, config, write_mode, tm, index, registry, shutdown, listener_stop));
        if (!result->initialize(error)) {
            // Reject while the accepted id is still alive; destruction below then tears down any
            // partially-created QP/MRs and the migrated CM id exactly once.
            (void)rdma_reject(result->id_, nullptr, 0);
            return nullptr;
        }
        return result;
    }

    ~Connection() override { reset(); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool accept(std::string& error) {
        const auto descriptor = wire::encode_descriptor(local_descriptor());
        rdma_conn_param parameters{};
        parameters.private_data = descriptor.data();
        parameters.private_data_len = static_cast<std::uint8_t>(descriptor.size());
        parameters.responder_resources = 1;
        parameters.initiator_depth = 1;
        parameters.retry_count = 7;
        parameters.rnr_retry_count = 7;
        if (rdma_accept(id_, &parameters) != 0) {
            error = errno_message("accept RDMA-CM connection");
            return false;
        }
        return true;
    }

    void run() {
        // Accept is asynchronous. Ack ESTABLISHED (or the failure/disconnect event) before creating
        // per-connection services, so even an io_uring initialization failure can tear CM down
        // without leaving an outstanding event that makes rdma_destroy_id wait.
        while (!established_ && !disconnected_ && !failed_ && !stop_requested()) {
            pollfd descriptor{.fd = channel_->fd, .events = POLLIN, .revents = 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready > 0) (void)drain_cm_events();
            else if (ready < 0 && errno != EINTR) fail(errno_message("poll RDMA-CM connection"));
        }
        if (!established_ || disconnected_ || failed_ || stop_requested()) {
            disconnect();
            return;
        }
        auto reactor = core::Reactor::create(512);
        if (!reactor) {
            fail("create RDMA connection io_uring: " + reactor.error().detail);
            disconnect();
            return;
        }
        registry_.add(&stats_);
        stats_.conns.fetch_add(1, std::memory_order_relaxed);
        stats_.curr_conns.fetch_add(1, std::memory_order_relaxed);
        session_.emplace(tm_, index_, &*reactor, stats_, &registry_, *this, write_mode_);

        while (!stop_requested() && !failed_ && !disconnected_) {
            bool progress = false;
            progress |= drain_cm_events();
            progress |= reap_completions();
            progress |= flush_bulk_release();
            progress |= process_incoming();
            progress |= flush_bulk_release();
            if (session_ && !failed_) progress |= session_->drive();
            if (session_ && session_->failed()) fail(std::string(session_->error()));
            if (session_ && session_->close_requested()) {
                disconnect();
                break;
            }
            if (!progress) cpu_relax();
        }
        if (stop_requested()) disconnect();
        session_.reset();
        stats_.curr_conns.fetch_sub(1, std::memory_order_relaxed);
        registry_.remove(&stats_);
    }

    std::size_t inline_capacity() const noexcept override { return wire::kInlineBytes; }

    bool try_send_inline(std::string_view payload) noexcept override {
        (void)reap_completions();
        (void)drain_leading_bulk_releases();
        (void)flush_bulk_release();
        if (!established_ || failed_ || disconnected_ || bulk_release_pending_) return false;
        std::array<std::byte, wire::kControlRecordBytes> frame{};
        const std::size_t length = wire::encode_inline(payload, frame);
        return length != 0 && post_control_record(
            std::string_view(reinterpret_cast<const char*>(frame.data()), length));
    }

    std::optional<TxWindow> acquire_tx_window() noexcept override {
        (void)reap_completions();
        (void)drain_leading_bulk_releases();
        if (!established_ || failed_ || disconnected_) return std::nullopt;
        for (unsigned i = 0; i < tx_slots_.size(); ++i) {
            auto& slot = tx_slots_[i];
            if (slot.reserved ||
                (slot.completion_wqe != 0 && completed_wqes_ < slot.completion_wqe))
                continue;
            slot.reserved = true;
            return TxWindow{i, MutBytes(bulk_.data +
                static_cast<std::size_t>(i) * local_bulk_window_bytes_,
                negotiated_bulk_window_bytes_)};
        }
        return std::nullopt;
    }

    bool try_send_tx_window(unsigned id, std::size_t length) noexcept override {
        (void)reap_completions();
        (void)drain_leading_bulk_releases();
        (void)flush_bulk_release();
        if (id >= tx_slots_.size() || !tx_slots_[id].reserved || bulk_release_pending_)
            return false;
        return post_bulk(id, length);
    }

    void release_tx_window(unsigned id) noexcept override {
        if (id < tx_slots_.size()) tx_slots_[id].reserved = false;
    }

private:
    struct TxSlot {
        bool reserved = false;
        std::uint64_t completion_wqe = 0;
    };

    Connection(rdma_cm_id* id, wire::Descriptor remote, const RdmaConfig& config,
               WriteMode write_mode, storage::TierManager& tm, storage::Index& index,
               core::StatsRegistry& registry, const std::atomic<bool>& shutdown,
               const std::atomic<bool>& listener_stop)
        : id_(id), remote_(remote), config_(config), write_mode_(write_mode), tm_(tm),
          index_(index), registry_(registry), shutdown_(shutdown), listener_stop_(listener_stop) {}

    rdma_event_channel* channel_ = nullptr;
    rdma_cm_id* id_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_cq* cq_ = nullptr;
    ibv_mr* control_mr_ = nullptr;
    ibv_mr* bulk_mr_ = nullptr;
    Mapping control_;
    Mapping bulk_;
    wire::Descriptor remote_{};
    RdmaConfig config_;
    WriteMode write_mode_;
    storage::TierManager& tm_;
    storage::Index& index_;
    core::StatsRegistry& registry_;
    const std::atomic<bool>& shutdown_;
    const std::atomic<bool>& listener_stop_;
    core::Stats stats_{};
    std::optional<memcache::RdmaSession> session_;

    std::uint32_t local_slot_count_ = 0;
    std::uint32_t local_bulk_window_bytes_ = 0;
    std::uint32_t local_bulk_window_count_ = 0;
    std::size_t negotiated_bulk_window_bytes_ = 0;
    std::vector<TxSlot> tx_slots_;

    bool established_ = false;
    bool disconnected_ = false;
    bool disconnect_event_seen_ = false;
    bool failed_ = false;
    std::string error_;
    std::uint64_t receive_sequence_ = 1;
    std::uint64_t send_sequence_ = 1;
    std::uint64_t cached_remote_consumed_ = 0;
    std::uint64_t posted_wqes_ = 0;
    std::uint64_t completed_wqes_ = 0;
    std::uint32_t send_queue_depth_ = 0;
    std::uint32_t signal_every_ = 1;
    std::uint64_t credit_read_wqe_ = 0;
    bool credit_read_pending_ = false;
    std::uint64_t bulk_send_sequence_ = 1;
    std::uint64_t bulk_remote_released_ = 0;
    std::uint64_t bulk_receive_sequence_ = 1;
    std::uint64_t bulk_release_sequence_ = 0;
    bool bulk_release_pending_ = false;

    void fail(std::string message) noexcept {
        if (!failed_) {
            error_ = std::move(message);
            std::println(stderr, "RDMA connection: {}", error_);
        }
        failed_ = true;
    }

    bool stop_requested() const noexcept {
        return shutdown_.load(std::memory_order_relaxed) ||
               listener_stop_.load(std::memory_order_relaxed);
    }

    void disconnect() noexcept {
        if (!id_ || !established_ || disconnected_) return;
        if (rdma_disconnect(id_) == 0 && channel_) {
            // CM teardown is asynchronous. Ack DISCONNECTED before destroying the id; otherwise
            // rdma_destroy_id may wait indefinitely for an event still owned by this channel.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!disconnect_event_seen_ && std::chrono::steady_clock::now() < deadline) {
                pollfd descriptor{.fd = channel_->fd, .events = POLLIN, .revents = 0};
                const int ready = ::poll(&descriptor, 1, 10);
                if (ready > 0) (void)drain_cm_events();
                else if (ready < 0 && errno != EINTR) break;
            }
        }
        disconnected_ = true;
    }

    void reset() noexcept {
        session_.reset();
        if (id_ && id_->qp) rdma_destroy_qp(id_);
        if (bulk_mr_) (void)ibv_dereg_mr(bulk_mr_);
        if (control_mr_) (void)ibv_dereg_mr(control_mr_);
        if (cq_) (void)ibv_destroy_cq(cq_);
        if (pd_) (void)ibv_dealloc_pd(pd_);
        if (id_) (void)rdma_destroy_id(id_);
        if (channel_) rdma_destroy_event_channel(channel_);
        bulk_mr_ = nullptr;
        control_mr_ = nullptr;
        cq_ = nullptr;
        pd_ = nullptr;
        id_ = nullptr;
        channel_ = nullptr;
    }

    wire::Descriptor local_descriptor() const noexcept {
        return wire::Descriptor{
            .ring_address = reinterpret_cast<std::uintptr_t>(control_.data),
            .nonce = reinterpret_cast<std::uintptr_t>(this) ^
                     static_cast<std::uint64_t>(::getpid()),
            .bulk_address = reinterpret_cast<std::uintptr_t>(bulk_.data),
            .ring_rkey = control_mr_->rkey,
            .ring_region_bytes = static_cast<std::uint32_t>(control_.size),
            .ring_slot_count = local_slot_count_,
            .bulk_rkey = bulk_mr_->rkey,
            .bulk_region_bytes = static_cast<std::uint32_t>(bulk_.size),
            .bulk_window_bytes = local_bulk_window_bytes_,
            .bulk_window_count = local_bulk_window_count_,
        };
    }

    bool initialize(std::string& error) {
        if (!id_ || !id_->verbs) {
            error = "RDMA-CM request did not select a verbs device";
            return false;
        }
        channel_ = rdma_create_event_channel();
        if (!channel_) {
            error = errno_message("create per-connection RDMA-CM channel");
            return false;
        }
        if (!nonblocking(channel_->fd, error)) return false;
        if (rdma_migrate_id(id_, channel_) != 0) {
            error = errno_message("migrate accepted RDMA-CM id");
            return false;
        }

        std::size_t control_bytes = 0;
        if (!compute_control_geometry(config_.ring_bytes, local_slot_count_, control_bytes, error))
            return false;
        local_bulk_window_bytes_ = static_cast<std::uint32_t>(config_.bulk_window_bytes);
        local_bulk_window_count_ = static_cast<std::uint32_t>(config_.bulk_window_count);
        const std::uint64_t bulk_bytes = std::uint64_t{2} * local_bulk_window_bytes_ *
                                         local_bulk_window_count_;
        if (!wire::valid_bulk_geometry(local_bulk_window_bytes_, local_bulk_window_count_) ||
            bulk_bytes > std::numeric_limits<std::size_t>::max()) {
            error = "invalid local RDMA bulk geometry";
            return false;
        }
        negotiated_bulk_window_bytes_ = std::min<std::size_t>(
            local_bulk_window_bytes_, remote_.bulk_window_bytes);
        if (negotiated_bulk_window_bytes_ < kDeviceBlock) {
            error = "RDMA peers negotiated a bulk window smaller than 4 KiB";
            return false;
        }
        if (!control_.allocate(control_bytes, error, "RDMA control ring") ||
            !bulk_.allocate(static_cast<std::size_t>(bulk_bytes), error, "RDMA bulk region"))
            return false;

        pd_ = ibv_alloc_pd(id_->verbs);
        if (!pd_) { error = errno_message("allocate RDMA protection domain"); return false; }
        cq_ = ibv_create_cq(id_->verbs, kCqDepth, nullptr, nullptr, 0);
        if (!cq_) { error = errno_message("create RDMA completion queue"); return false; }
        control_mr_ = ibv_reg_mr(pd_, control_.data, control_.size,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!control_mr_) { error = errno_message("register RDMA control ring"); return false; }
        bulk_mr_ = ibv_reg_mr(pd_, bulk_.data, bulk_.size,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!bulk_mr_) { error = errno_message("register RDMA bulk region"); return false; }

        ibv_qp_init_attr qp{};
        qp.send_cq = cq_;
        qp.recv_cq = cq_;
        qp.cap.max_send_wr = kSendQueueDepth;
        qp.cap.max_recv_wr = 1;
        qp.cap.max_send_sge = 1;
        qp.cap.max_recv_sge = 1;
        qp.cap.max_inline_data = static_cast<std::uint32_t>(
            wire::kControlRecordBytes + sizeof(std::uint64_t));
        qp.qp_type = IBV_QPT_RC;
        if (rdma_create_qp(id_, pd_, &qp) != 0) {
            error = errno_message("create RDMA RC queue pair");
            return false;
        }
        if (qp.cap.max_inline_data < wire::kControlRecordBytes + sizeof(std::uint64_t)) {
            error = "RDMA device cannot inline the 200-byte Goblin control write";
            return false;
        }
        // Providers may round max_send_wr above the requested 512 while this connection's CQ is
        // deliberately sized for the requested geometry. Never let logical reservation trust that
        // larger value and create more completions than the CQ can hold.
        send_queue_depth_ = std::min<std::uint32_t>(qp.cap.max_send_wr, kSendQueueDepth);
        if (send_queue_depth_ < 3) {
            error = "RDMA device supplied an unusable send queue";
            return false;
        }
        signal_every_ = std::min<std::uint32_t>(kSignalEvery, send_queue_depth_ / 2);
        try {
            tx_slots_.resize(local_bulk_window_count_);
        } catch (const std::bad_alloc&) {
            error = "allocate RDMA source-window lifetime state";
            return false;
        }
        return true;
    }

    bool drain_cm_events() noexcept {
        bool progress = false;
        for (;;) {
            rdma_cm_event* event = nullptr;
            if (rdma_get_cm_event(channel_, &event) != 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) fail(errno_message("read RDMA-CM event"));
                return progress;
            }
            progress = true;
            const rdma_cm_event_type type = event->event;
            const int status = event->status;
            if (rdma_ack_cm_event(event) != 0) {
                fail(errno_message("acknowledge RDMA-CM event"));
                return progress;
            }
            switch (type) {
                case RDMA_CM_EVENT_ESTABLISHED:
                    if (status != 0) fail("RDMA-CM establishment failed");
                    else established_ = true;
                    break;
                case RDMA_CM_EVENT_DISCONNECTED:
                case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                    disconnect_event_seen_ = true;
                    disconnected_ = true;
                    break;
                case RDMA_CM_EVENT_DEVICE_REMOVAL:
                    disconnected_ = true;
                    fail("RDMA device was removed");
                    break;
                case RDMA_CM_EVENT_REJECTED:
                case RDMA_CM_EVENT_CONNECT_ERROR:
                case RDMA_CM_EVENT_UNREACHABLE:
                case RDMA_CM_EVENT_ADDR_ERROR:
                case RDMA_CM_EVENT_ROUTE_ERROR:
                    disconnected_ = true;
                    fail("RDMA-CM connection failed: " + std::string(rdma_event_str(type)) +
                         (status == 0 ? "" : " (status " + std::to_string(status) + ')'));
                    break;
                default:
                    break;
            }
        }
    }

    bool reap_completions() noexcept {
        if (!cq_ || failed_) return false;
        bool progress = false;
        std::array<ibv_wc, 16> completions{};
        for (;;) {
            const int count = ibv_poll_cq(cq_, static_cast<int>(completions.size()),
                                         completions.data());
            if (count < 0) { fail("poll RDMA completion queue failed"); return progress; }
            if (count == 0) return progress;
            progress = true;
            for (int i = 0; i < count; ++i) {
                const auto& completion = completions[static_cast<std::size_t>(i)];
                if (completion.status != IBV_WC_SUCCESS) {
                    credit_read_pending_ = false;
                    fail("RDMA work request failed: " +
                         std::string(ibv_wc_status_str(completion.status)));
                    return progress;
                }
                const std::uint64_t sequence = completion.wr_id & kWrSequenceMask;
                completed_wqes_ = std::max(completed_wqes_, sequence);
                if ((completion.wr_id & kWrTagMask) == kWrCreditRead) {
                    if (!credit_read_pending_ || sequence != credit_read_wqe_) {
                        fail("RDMA credit-read completion did not match its request");
                        return progress;
                    }
                    const auto* source = reinterpret_cast<const std::uint64_t*>(
                        control_.data + wire::kCreditReadbackOffset);
                    const std::uint64_t consumed = wire::sequence_from_wire(
                        __atomic_load_n(source, __ATOMIC_ACQUIRE));
                    credit_read_pending_ = false;
                    if (consumed < cached_remote_consumed_ || consumed >= send_sequence_) {
                        fail("RDMA peer returned an invalid consumed control sequence");
                        return progress;
                    }
                    cached_remote_consumed_ = consumed;
                }
            }
        }
    }

    bool reserve_wqes(std::uint32_t count) noexcept {
        (void)reap_completions();
        const std::uint64_t outstanding = posted_wqes_ - completed_wqes_;
        return count <= send_queue_depth_ - 1 &&
               outstanding + count <= send_queue_depth_ - 1;
    }

    bool start_remote_consumed_read() noexcept {
        if (!established_ || failed_ || disconnected_ || credit_read_pending_ || !reserve_wqes(1))
            return false;
        auto* destination = reinterpret_cast<std::uint64_t*>(
            control_.data + wire::kCreditReadbackOffset);
        ibv_sge sge{};
        sge.addr = reinterpret_cast<std::uintptr_t>(destination);
        sge.length = sizeof(*destination);
        sge.lkey = control_mr_->lkey;
        ibv_send_wr request{};
        request.wr_id = kWrCreditRead | (++posted_wqes_);
        request.sg_list = &sge;
        request.num_sge = 1;
        request.opcode = IBV_WR_RDMA_READ;
        request.send_flags = IBV_SEND_SIGNALED;
        request.wr.rdma.remote_addr = remote_.ring_address + wire::kConsumedOffset;
        request.wr.rdma.rkey = remote_.ring_rkey;
        ibv_send_wr* bad = nullptr;
        const int result = ibv_post_send(id_->qp, &request, &bad);
        if (result != 0) {
            --posted_wqes_;
            fail(errno_message("post RDMA credit read", result));
            return false;
        }
        credit_read_wqe_ = request.wr_id & kWrSequenceMask;
        credit_read_pending_ = true;
        return true;
    }

    bool control_has_credit() noexcept {
        const std::uint64_t used = (send_sequence_ - 1) - cached_remote_consumed_;
        if (used < remote_.ring_slot_count) return true;
        if (!credit_read_pending_) (void)start_remote_consumed_read();
        return false;
    }

    bool post_control_record(std::string_view payload) noexcept {
        if (payload.empty() || payload.size() > wire::kControlRecordBytes ||
            send_sequence_ >= kControlSequenceLimit || !control_has_credit() || !reserve_wqes(1)) {
            if (send_sequence_ >= kControlSequenceLimit) fail("RDMA control sequence exhausted");
            return false;
        }
        std::array<std::byte, wire::kControlRecordBytes + sizeof(std::uint64_t)> inline_data{};
        std::memcpy(inline_data.data(), payload.data(), payload.size());
        const std::uint64_t commit = wire::control_commit_to_wire(send_sequence_, payload.size());
        std::memcpy(inline_data.data() + payload.size(), &commit, sizeof(commit));
        const std::uint64_t slot = (send_sequence_ - 1) % remote_.ring_slot_count;
        const std::uint64_t remote_commit = remote_.ring_address +
            wire::kControlRegionHeaderBytes + slot * wire::kControlSlotStride +
            wire::kControlCommitOffset;
        ibv_sge sge{};
        sge.addr = reinterpret_cast<std::uintptr_t>(inline_data.data());
        sge.length = static_cast<std::uint32_t>(payload.size() + sizeof(commit));
        const std::uint64_t wqe = ++posted_wqes_;
        ibv_send_wr request{};
        request.wr_id = kWrWrite | wqe;
        request.sg_list = &sge;
        request.num_sge = 1;
        request.opcode = IBV_WR_RDMA_WRITE;
        request.send_flags = IBV_SEND_INLINE |
            ((wqe % signal_every_) == 0 ? IBV_SEND_SIGNALED : 0);
        request.wr.rdma.remote_addr = remote_commit - payload.size();
        request.wr.rdma.rkey = remote_.ring_rkey;
        ibv_send_wr* bad = nullptr;
        const int result = ibv_post_send(id_->qp, &request, &bad);
        if (result != 0) {
            --posted_wqes_;
            fail(errno_message("post inline RDMA write", result));
            return false;
        }
        ++send_sequence_;
        return true;
    }

    bool post_bulk(unsigned source_slot, std::size_t length) noexcept {
        if (!established_ || failed_ || disconnected_ || length == 0 ||
            length > negotiated_bulk_window_bytes_ ||
            bulk_send_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            send_sequence_ >= kControlSequenceLimit)
            return false;
        const std::uint64_t capacity = std::min<std::uint64_t>(
            local_bulk_window_count_, remote_.bulk_window_count);
        if ((bulk_send_sequence_ - 1) - bulk_remote_released_ >= capacity ||
            !control_has_credit() || !reserve_wqes(2))
            return false;

        auto& local = tx_slots_[source_slot];
        if (local.completion_wqe != 0 && completed_wqes_ < local.completion_wqe) return false;
        std::byte* source = bulk_.data +
            static_cast<std::size_t>(source_slot) * local_bulk_window_bytes_;
        const auto ready = wire::encode_bulk_ready(static_cast<std::uint32_t>(length),
                                                    bulk_send_sequence_);
        std::array<std::byte, wire::kControlRecordBytes + sizeof(std::uint64_t)> ready_inline{};
        std::memcpy(ready_inline.data(), ready.data(), ready.size());
        const std::uint64_t commit = wire::control_commit_to_wire(send_sequence_, ready.size());
        std::memcpy(ready_inline.data() + ready.size(), &commit, sizeof(commit));

        const std::uint64_t remote_slot =
            (bulk_send_sequence_ - 1) % remote_.bulk_window_count;
        const std::uint64_t remote_rx_half =
            static_cast<std::uint64_t>(remote_.bulk_window_bytes) * remote_.bulk_window_count;
        const std::uint64_t remote_bulk = remote_.bulk_address + remote_rx_half +
            remote_slot * remote_.bulk_window_bytes;
        const std::uint64_t control_slot = (send_sequence_ - 1) % remote_.ring_slot_count;
        const std::uint64_t remote_control_commit = remote_.ring_address +
            wire::kControlRegionHeaderBytes + control_slot * wire::kControlSlotStride +
            wire::kControlCommitOffset;

        ibv_sge bulk_sge{};
        bulk_sge.addr = reinterpret_cast<std::uintptr_t>(source);
        bulk_sge.length = static_cast<std::uint32_t>(length);
        bulk_sge.lkey = bulk_mr_->lkey;
        ibv_sge ready_sge{};
        ready_sge.addr = reinterpret_cast<std::uintptr_t>(ready_inline.data());
        ready_sge.length = static_cast<std::uint32_t>(ready.size() + sizeof(commit));

        const std::uint64_t data_wqe = posted_wqes_ + 1;
        const std::uint64_t ready_wqe = posted_wqes_ + 2;
        ibv_send_wr ready_request{};
        ready_request.wr_id = kWrWrite | ready_wqe;
        ready_request.sg_list = &ready_sge;
        ready_request.num_sge = 1;
        ready_request.opcode = IBV_WR_RDMA_WRITE;
        ready_request.send_flags = IBV_SEND_INLINE;
        ready_request.wr.rdma.remote_addr = remote_control_commit - ready.size();
        ready_request.wr.rdma.rkey = remote_.ring_rkey;
        ibv_send_wr data_request{};
        data_request.wr_id = kWrBulkWrite | data_wqe;
        data_request.next = &ready_request;
        data_request.sg_list = &bulk_sge;
        data_request.num_sge = 1;
        data_request.opcode = IBV_WR_RDMA_WRITE;
        data_request.send_flags = IBV_SEND_SIGNALED;
        data_request.wr.rdma.remote_addr = remote_bulk;
        data_request.wr.rdma.rkey = remote_.bulk_rkey;
        ibv_send_wr* bad = nullptr;
        const int result = ibv_post_send(id_->qp, &data_request, &bad);
        if (result != 0) {
            fail(errno_message("post RDMA bulk write and READY", result));
            return false;
        }
        posted_wqes_ += 2;
        local.completion_wqe = data_wqe;
        local.reserved = false;
        ++bulk_send_sequence_;
        ++send_sequence_;
        return true;
    }

    std::optional<std::string_view> peek_control_record() noexcept {
        const std::uint64_t slot = (receive_sequence_ - 1) % local_slot_count_;
        auto* commit_address = reinterpret_cast<std::uint64_t*>(control_.data +
            wire::kControlRegionHeaderBytes + slot * wire::kControlSlotStride +
            wire::kControlCommitOffset);
        const std::uint64_t commit = wire::control_commit_from_wire(
            __atomic_load_n(commit_address, __ATOMIC_ACQUIRE));
        const std::uint64_t sequence = commit >> 8;
        if (sequence < receive_sequence_) return std::nullopt;
        if (sequence != receive_sequence_) {
            fail("RDMA control sequence jumped from " + std::to_string(receive_sequence_) +
                 " to " + std::to_string(sequence));
            return std::nullopt;
        }
        const std::size_t length = static_cast<std::size_t>(commit & 0xffU);
        if (length == 0 || length > wire::kControlRecordBytes) {
            fail("RDMA control ring published an invalid record length");
            return std::nullopt;
        }
        return std::string_view(reinterpret_cast<const char*>(commit_address) - length, length);
    }

    void consume_control_record() noexcept {
        const std::uint64_t consumed = receive_sequence_++;
        auto* word = reinterpret_cast<std::uint64_t*>(control_.data + wire::kConsumedOffset);
        __atomic_store_n(word, wire::sequence_to_wire(consumed), __ATOMIC_RELEASE);
    }

    bool accept_bulk_release(std::uint64_t sequence) noexcept {
        if (sequence < bulk_remote_released_ || sequence >= bulk_send_sequence_) {
            fail("RDMA peer released an invalid bulk sequence");
            return false;
        }
        bulk_remote_released_ = sequence;
        return true;
    }

    bool drain_leading_bulk_releases() noexcept {
        bool progress = false;
        while (!failed_ && !disconnected_) {
            const auto record = peek_control_record();
            if (!record) return progress;
            wire::FrameView frame;
            if (!wire::decode_frame(*record, frame)) {
                fail("RDMA control ring published an invalid v3 frame");
                return progress;
            }
            if (frame.type != wire::FrameType::bulk_release) return progress;
            if (!accept_bulk_release(frame.sequence)) return progress;
            consume_control_record();
            progress = true;
        }
        return progress;
    }

    bool flush_bulk_release() noexcept {
        if (!bulk_release_pending_ || !established_ || failed_ || disconnected_) return false;
        const auto frame = wire::encode_bulk_release(bulk_release_sequence_);
        if (!post_control_record(std::string_view(
                reinterpret_cast<const char*>(frame.data()), frame.size())))
            return false;
        bulk_release_pending_ = false;
        return true;
    }

    bool process_incoming() noexcept {
        bool progress = drain_leading_bulk_releases();
        if (!session_ || failed_ || disconnected_ || !established_) return progress;
        for (unsigned records = 0; records < 64; ++records) {
            const auto record = peek_control_record();
            if (!record) return progress;
            wire::FrameView frame;
            if (!wire::decode_frame(*record, frame)) {
                fail("RDMA control ring published an invalid v3 frame");
                return progress;
            }
            if (frame.type == wire::FrameType::bulk_release) {
                if (!accept_bulk_release(frame.sequence)) return progress;
                consume_control_record();
                progress = true;
                continue;
            }

            memcache::RdmaSession::ConsumeResult result;
            if (frame.type == wire::FrameType::inline_bytes) {
                result = session_->accept_inline(frame.inline_payload);
            } else {
                if (frame.sequence != bulk_receive_sequence_ ||
                    frame.length > negotiated_bulk_window_bytes_) {
                    fail("RDMA BULK_READY has an invalid sequence or length");
                    return progress;
                }
                const std::size_t slot = static_cast<std::size_t>(
                    (bulk_receive_sequence_ - 1) % local_bulk_window_count_);
                const std::size_t rx_half = static_cast<std::size_t>(local_bulk_window_bytes_) *
                                            local_bulk_window_count_;
                const std::size_t offset = rx_half + slot * local_bulk_window_bytes_;
                std::atomic_thread_fence(std::memory_order_acquire);
                result = session_->accept_bulk(ByteView(bulk_.data + offset, frame.length));
            }
            if (result == memcache::RdmaSession::ConsumeResult::blocked) return progress;
            if (result == memcache::RdmaSession::ConsumeResult::fatal) {
                fail(std::string(session_->error()));
                return progress;
            }
            consume_control_record();
            progress = true;
            if (session_->close_requested()) return progress;
            if (frame.type == wire::FrameType::bulk_ready) {
                bulk_release_sequence_ = bulk_receive_sequence_++;
                bulk_release_pending_ = true;
                (void)flush_bulk_release();
            }
        }
        return progress;
    }
};

struct Listener {
    rdma_event_channel* channel = nullptr;
    rdma_cm_id* id = nullptr;

    ~Listener() {
        if (id) (void)rdma_destroy_id(id);
        if (channel) rdma_destroy_event_channel(channel);
    }
};

Result<std::unique_ptr<Listener>> open_listener(const RdmaConfig& config) {
    auto listener = std::make_unique<Listener>();
    listener->channel = rdma_create_event_channel();
    if (!listener->channel)
        return err(Errc::io_error, errno_message("create RDMA-CM listener channel"));
    std::string nonblock_error;
    if (!nonblocking(listener->channel->fd, nonblock_error))
        return err(Errc::io_error, std::move(nonblock_error));
    if (rdma_create_id(listener->channel, &listener->id, nullptr, RDMA_PS_TCP) != 0)
        return err(Errc::io_error, errno_message("create RDMA-CM listener id"));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(config.port);
    const int lookup = ::getaddrinfo(config.address.c_str(), port.c_str(), &hints, &addresses);
    if (lookup != 0)
        return err(Errc::invalid_argument,
                   "parse numeric RDMA listen address: " + std::string(gai_strerror(lookup)));
    const int bind_result = rdma_bind_addr(listener->id, addresses->ai_addr);
    ::freeaddrinfo(addresses);
    if (bind_result != 0)
        return err(Errc::io_error, errno_message("bind RDMA-CM listener"));
    if (rdma_listen(listener->id, static_cast<int>(config.backlog)) != 0)
        return err(Errc::io_error, errno_message("listen on RDMA-CM endpoint"));
    return listener;
}

} // namespace

bool rdma_server_available() noexcept { return true; }

Status serve_rdma(const RdmaConfig& config, WriteMode write_mode, storage::TierManager& tm,
                  storage::Index& index, core::StatsRegistry& stats,
                  const std::atomic<bool>& shutdown, std::function<void(Status)> startup) {
    if (!core::Reactor::available()) {
        Status failure = err(Errc::unsupported,
            "native RDMA disk-tail streaming requires a build with liburing");
        if (startup) startup(failure);
        return failure;
    }
    auto listener_result = open_listener(config);
    if (!listener_result) {
        Status failure = std::unexpected(listener_result.error());
        if (startup) startup(failure);
        return failure;
    }
    auto listener = std::move(*listener_result);
    if (startup) startup(Status{});
    std::atomic<bool> listener_stop{false};
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers;

    Status result{};
    while (!shutdown.load(std::memory_order_relaxed)) {
        // Join finished connections continuously; retaining joinable pthreads until process
        // shutdown would leak a stack and kernel task record on every reconnect.
        for (auto it = workers.begin(); it != workers.end();) {
            if (!it->done->load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            it->thread.join();
            it = workers.erase(it);
        }
        pollfd descriptor{.fd = listener->channel->fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            result = err(Errc::io_error, errno_message("poll RDMA-CM listener"));
            break;
        }
        if (ready == 0) continue;

        for (;;) {
            rdma_cm_event* event = nullptr;
            if (rdma_get_cm_event(listener->channel, &event) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                result = err(Errc::io_error, errno_message("read RDMA-CM listener event"));
                break;
            }
            const rdma_cm_event_type type = event->event;
            const int status = event->status;
            rdma_cm_id* accepted_id = event->id;
            wire::Descriptor remote;
            bool descriptor_ok = false;
            if (type == RDMA_CM_EVENT_CONNECT_REQUEST && status == 0 &&
                event->param.conn.private_data &&
                event->param.conn.private_data_len >= wire::kDescriptorBytes) {
                descriptor_ok = wire::decode_descriptor(
                    std::span(static_cast<const std::byte*>(event->param.conn.private_data),
                              event->param.conn.private_data_len), remote);
            }
            if (rdma_ack_cm_event(event) != 0) {
                result = err(Errc::io_error,
                             errno_message("acknowledge RDMA-CM listener event"));
                break;
            }
            if (type != RDMA_CM_EVENT_CONNECT_REQUEST || status != 0) continue;
            if (!descriptor_ok) {
                (void)rdma_reject(accepted_id, nullptr, 0);
                (void)rdma_destroy_id(accepted_id);
                continue;
            }

            std::string connection_error;
            auto connection = Connection::create(accepted_id, remote, config, write_mode, tm,
                                                 index, stats, shutdown, listener_stop,
                                                 connection_error);
            if (!connection) {
                // Connection::create rejected and destroyed its migrated CM id.
                std::println(stderr, "RDMA connection rejected: {}", connection_error);
                continue;
            }
            if (!connection->accept(connection_error)) {
                (void)rdma_reject(accepted_id, nullptr, 0);
                std::println(stderr, "RDMA connection rejected: {}", connection_error);
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            workers.push_back(Worker{
                std::thread([connection = std::move(connection), done] {
                    connection->run();
                    done->store(true, std::memory_order_release);
                }),
                std::move(done)});
        }
        if (!result) break;
    }
    // A listener failure is independent of main's process-wide shutdown flag. Signal every
    // connection explicitly before joining so cleanup is bounded and no joinable thread reaches
    // vector destruction.
    listener_stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.thread.join();
    return result;
}

} // namespace goblin::net

#else

namespace goblin::net {

bool rdma_server_available() noexcept { return false; }

Status serve_rdma(const RdmaConfig&, WriteMode, storage::TierManager&, storage::Index&,
                  core::StatsRegistry&, const std::atomic<bool>&,
                  std::function<void(Status)> startup) {
    Status failure = err(Errc::unsupported,
                         "native RDMA endpoint unavailable; install libibverbs-dev and librdmacm-dev");
    if (startup) startup(failure);
    return failure;
}

} // namespace goblin::net

#endif
