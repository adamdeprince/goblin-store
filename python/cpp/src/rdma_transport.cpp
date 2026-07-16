#include "goblin/store/client.hpp"
#include "goblin/store/rdma_wire.hpp"

#if !defined(__linux__)
#error "The native Goblin Store RDMA transport is Linux-only"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace goblin::client {
namespace {

namespace wire = goblin::store::rdma_wire;

constexpr std::uint64_t kSequenceLimit = std::uint64_t{1} << 56;
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

using Clock = std::chrono::steady_clock;

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

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

std::string errno_message(std::string_view operation, int error = errno) {
    return std::string(operation) + ": " + std::strerror(error);
}

bool compute_region_geometry(std::uint64_t requested, std::uint32_t& slots,
                             std::size_t& mapped, std::string& error) {
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
    if (raw > std::numeric_limits<std::size_t>::max() - (page - 1)) {
        error = "RDMA ring allocation is too large";
        return false;
    }
    mapped = align_up(static_cast<std::size_t>(raw), page);
    if (mapped > std::numeric_limits<std::uint32_t>::max()) {
        error = "RDMA ring allocation exceeds the wire descriptor limit";
        return false;
    }
    slots = static_cast<std::uint32_t>(count);
    return true;
}

bool decode_descriptor(const void* private_data, std::size_t length,
                       wire::Descriptor& descriptor, std::string& error) {
    if (private_data == nullptr || length < wire::kDescriptorBytes) {
        error = "RDMA peer supplied a " + std::to_string(length) +
                "-byte descriptor; Goblin bulk ABI v3 requires 56";
        return false;
    }
    if (!wire::decode_descriptor(
            std::span(static_cast<const std::byte*>(private_data), length), descriptor)) {
        error = "RDMA peer supplied an incompatible Goblin bulk ABI v3 descriptor";
        return false;
    }
    return true;
}

int hca_numa_node(ibv_context* context) noexcept {
    if (context == nullptr || context->device == nullptr) return -1;
    char path[256];
    const int length = std::snprintf(path, sizeof(path),
        "/sys/class/infiniband/%s/device/numa_node", ibv_get_device_name(context->device));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)) return -1;
    std::FILE* file = std::fopen(path, "re");
    if (!file) return -1;
    int node = -1;
    if (std::fscanf(file, "%d", &node) != 1) node = -1;
    std::fclose(file);
    return node;
}

bool bind_range(void* address, std::size_t length, int node) noexcept {
    constexpr int kMpolBind = 2;
    constexpr unsigned long kMaxNodes = sizeof(unsigned long) * CHAR_BIT;
    if (node < 0 || static_cast<unsigned long>(node) >= kMaxNodes) return false;
    const unsigned long mask = 1UL << static_cast<unsigned>(node);
    return ::syscall(SYS_mbind, address, length, kMpolBind, &mask, kMaxNodes, 0) == 0;
}

bool all_on_node(void* address, std::size_t length, int node) noexcept {
    const long page_result = ::sysconf(_SC_PAGESIZE);
    if (page_result <= 0) return false;
    const auto page = static_cast<std::size_t>(page_result);
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = cursor + length;
    constexpr std::size_t kBatch = 512;
    void* pages[kBatch];
    int status[kBatch];
    while (cursor < end) {
        std::size_t count = 0;
        for (; count < kBatch && cursor < end; ++count, cursor += page) {
            pages[count] = reinterpret_cast<void*>(cursor);
            status[count] = -1;
        }
        if (::syscall(SYS_move_pages, 0, count, pages, nullptr, status, 0) != 0) return false;
        for (std::size_t i = 0; i < count; ++i)
            if (status[i] != node) return false;
    }
    return true;
}

struct CmEvent {
    rdma_cm_event_type type = RDMA_CM_EVENT_ADDR_ERROR;
    int status = 0;
    std::array<std::byte, 256> private_data{};
    std::size_t private_data_len = 0;
};

bool copy_and_ack(rdma_cm_event* source, CmEvent& target) noexcept {
    target.type = source->event;
    target.status = source->status;
    if (source->event == RDMA_CM_EVENT_ESTABLISHED ||
        source->event == RDMA_CM_EVENT_REJECTED) {
        target.private_data_len = std::min<std::size_t>(
            source->param.conn.private_data_len, target.private_data.size());
        if (target.private_data_len && source->param.conn.private_data)
            std::memcpy(target.private_data.data(), source->param.conn.private_data,
                        target.private_data_len);
    }
    return rdma_ack_cm_event(source) == 0;
}

int remaining_ms(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
}

bool wait_for_event(rdma_event_channel* channel, rdma_cm_event_type expected,
                    Clock::time_point deadline, CmEvent& copy, std::string& error) {
    const int timeout = remaining_ms(deadline);
    if (timeout == 0) {
        error = "timed out waiting for an RDMA-CM event";
        return false;
    }
    pollfd descriptor{.fd = channel->fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready <= 0) {
        error = ready == 0 ? "timed out waiting for an RDMA-CM event"
                           : errno_message("poll RDMA-CM channel");
        return false;
    }
    rdma_cm_event* raw = nullptr;
    if (rdma_get_cm_event(channel, &raw) != 0) {
        error = errno_message("read RDMA-CM event");
        return false;
    }
    if (!copy_and_ack(raw, copy)) {
        error = errno_message("acknowledge RDMA-CM event");
        return false;
    }
    if (copy.type != expected || copy.status != 0) {
        error = "expected RDMA-CM event " + std::string(rdma_event_str(expected)) +
                ", received " + rdma_event_str(copy.type);
        if (copy.status != 0) error += " (status " + std::to_string(copy.status) + ')';
        return false;
    }
    return true;
}

class RdmaTransport final : public Transport {
public:
    ~RdmaTransport() override { reset(); }

    static std::unique_ptr<RdmaTransport> open(const Options& options, std::string& error) {
        auto result = std::unique_ptr<RdmaTransport>(new RdmaTransport);
        if (!result->open_impl(options, error)) return nullptr;
        return result;
    }

    std::size_t max_fragment_size() const noexcept override { return wire::kInlineBytes; }

    std::size_t max_bulk_fragment_size() const noexcept override {
        return negotiated_bulk_window_bytes_;
    }

    bool try_send(std::string_view payload) noexcept override {
        progress();
        if (!established_ || failed_ || disconnected_ || bulk_release_pending_) return false;
        std::array<std::byte, wire::kControlRecordBytes> frame{};
        const std::size_t length = wire::encode_inline(payload, frame);
        return length != 0 && post_control_record(
            std::string_view(reinterpret_cast<const char*>(frame.data()), length));
    }

    bool try_send_bulk(std::string_view payload) noexcept override {
        progress();
        return established_ && !failed_ && !disconnected_ &&
               !bulk_release_pending_ && post_bulk(payload);
    }

    std::optional<std::string_view> peek() noexcept override {
        progress();
        if (!established_ || failed_ || disconnected_) return std::nullopt;
        if (exposed_kind_ != ExposedKind::none) return exposed_view_;

        for (;;) {
            const auto record = peek_control_record();
            if (!record) return std::nullopt;
            wire::FrameView frame;
            if (!wire::decode_frame(*record, frame)) {
                fail("RDMA control ring published an invalid version-3 frame");
                return std::nullopt;
            }
            switch (frame.type) {
                case wire::FrameType::inline_bytes:
                    exposed_kind_ = ExposedKind::inline_bytes;
                    exposed_view_ = frame.inline_payload;
                    return exposed_view_;
                case wire::FrameType::bulk_ready: {
                    if (frame.sequence != bulk_receive_sequence_ ||
                        frame.length > negotiated_bulk_window_bytes_) {
                        fail("RDMA bulk-ready frame has an invalid sequence or length");
                        return std::nullopt;
                    }
                    const std::uint64_t slot =
                        (bulk_receive_sequence_ - 1) % local_bulk_window_count_;
                    const std::size_t receive_half =
                        static_cast<std::size_t>(local_bulk_window_bytes_) *
                        local_bulk_window_count_;
                    const std::size_t offset = receive_half +
                        static_cast<std::size_t>(slot) * local_bulk_window_bytes_;
                    // READY is posted after the data write on the same strictly
                    // ordered RC QP. Its acquire publication makes that prior
                    // write visible before the application sees this view.
                    std::atomic_thread_fence(std::memory_order_acquire);
                    exposed_kind_ = ExposedKind::bulk_ready;
                    exposed_view_ = std::string_view(
                        reinterpret_cast<const char*>(bulk_region_ + offset), frame.length);
                    return exposed_view_;
                }
                case wire::FrameType::bulk_release:
                    if (!accept_bulk_release(frame.sequence)) return std::nullopt;
                    consume_control_record();
                    break;
            }
        }
    }

    void pop() noexcept override {
        if (failed_ || disconnected_ || !region_ || exposed_kind_ == ExposedKind::none) return;
        const bool release_bulk = exposed_kind_ == ExposedKind::bulk_ready;
        exposed_kind_ = ExposedKind::none;
        exposed_view_ = {};
        consume_control_record();
        if (release_bulk) {
            bulk_release_sequence_ = bulk_receive_sequence_++;
            bulk_release_pending_ = true;
            flush_bulk_release();
        }
    }

    bool failed() const noexcept override { return failed_; }
    bool disconnected() const noexcept override { return disconnected_; }
    std::string_view error() const noexcept override { return error_; }
    void wait() noexcept override {
        progress();
        cpu_relax();
    }

    void close() noexcept override {
        if (id_ && established_ && !disconnected_) {
            if (rdma_disconnect(id_) != 0 && !failed_) {
                fail(errno_message("disconnect RDMA connection"));
            } else {
                // RDMA-CM teardown is asynchronous. Give the local CM channel a
                // bounded opportunity to deliver and acknowledge DISCONNECTED
                // before the QP/id/channel are destroyed by reset().
                const auto deadline = Clock::now() + std::chrono::seconds(1);
                while (!disconnect_event_seen_ && Clock::now() < deadline) {
                    const int timeout = remaining_ms(deadline);
                    if (timeout == 0 || !poll_cm_event(timeout)) break;
                }
            }
        }
        disconnected_ = true;
    }

private:
    enum class ExposedKind { none, inline_bytes, bulk_ready };

    rdma_event_channel* channel_ = nullptr;
    rdma_cm_id* id_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_cq* send_cq_ = nullptr;
    ibv_mr* region_mr_ = nullptr;
    ibv_mr* bulk_mr_ = nullptr;
    std::byte* region_ = nullptr;
    std::byte* bulk_region_ = nullptr;
    std::size_t region_bytes_ = 0;
    std::size_t bulk_region_bytes_ = 0;
    std::uint32_t local_slot_count_ = 0;
    std::uint32_t local_bulk_window_bytes_ = 0;
    std::uint32_t local_bulk_window_count_ = 0;
    std::size_t negotiated_bulk_window_bytes_ = 0;
    wire::Descriptor remote_{};
    bool established_ = false;
    bool disconnected_ = false;
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
    bool disconnect_event_seen_ = false;
    std::uint64_t bulk_send_sequence_ = 1;
    std::uint64_t bulk_remote_released_ = 0;
    std::uint64_t bulk_receive_sequence_ = 1;
    std::uint64_t bulk_release_sequence_ = 0;
    bool bulk_release_pending_ = false;
    std::vector<std::uint64_t> bulk_tx_completion_wqes_;
    ExposedKind exposed_kind_ = ExposedKind::none;
    std::string_view exposed_view_{};

    void fail(std::string message) noexcept {
        if (!failed_) error_ = std::move(message);
        failed_ = true;
    }

    void reset() noexcept {
        close();
        if (id_ && id_->qp) rdma_destroy_qp(id_);
        if (bulk_mr_) (void)ibv_dereg_mr(bulk_mr_);
        if (region_mr_) (void)ibv_dereg_mr(region_mr_);
        if (send_cq_) (void)ibv_destroy_cq(send_cq_);
        if (pd_) (void)ibv_dealloc_pd(pd_);
        if (id_) (void)rdma_destroy_id(id_);
        if (channel_) rdma_destroy_event_channel(channel_);
        if (bulk_region_) (void)::munmap(bulk_region_, bulk_region_bytes_);
        if (region_) (void)::munmap(region_, region_bytes_);
        bulk_mr_ = nullptr;
        region_mr_ = nullptr;
        send_cq_ = nullptr;
        pd_ = nullptr;
        id_ = nullptr;
        channel_ = nullptr;
        bulk_region_ = nullptr;
        region_ = nullptr;
        bulk_region_bytes_ = 0;
        region_bytes_ = 0;
    }

    wire::Descriptor local_descriptor() const noexcept {
        return wire::Descriptor{
            .ring_address = reinterpret_cast<std::uintptr_t>(region_),
            .nonce = reinterpret_cast<std::uintptr_t>(this) ^ static_cast<std::uint64_t>(::getpid()),
            .bulk_address = reinterpret_cast<std::uintptr_t>(bulk_region_),
            .ring_rkey = region_mr_->rkey,
            .ring_region_bytes = static_cast<std::uint32_t>(region_bytes_),
            .ring_slot_count = local_slot_count_,
            .bulk_rkey = bulk_mr_->rkey,
            .bulk_region_bytes = static_cast<std::uint32_t>(bulk_region_bytes_),
            .bulk_window_bytes = local_bulk_window_bytes_,
            .bulk_window_count = local_bulk_window_count_,
        };
    }

    bool poll_cm_event(int timeout_ms) noexcept {
        if (!channel_) return false;
        pollfd descriptor{.fd = channel_->fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&descriptor, 1, timeout_ms);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) fail(errno_message("poll RDMA-CM channel"));
            return false;
        }
        rdma_cm_event* raw = nullptr;
        if (rdma_get_cm_event(channel_, &raw) != 0) {
            fail(errno_message("read RDMA-CM event"));
            return false;
        }
        const rdma_cm_event_type type = raw->event;
        const int status = raw->status;
        if (rdma_ack_cm_event(raw) != 0) {
            fail(errno_message("acknowledge RDMA-CM event"));
            return false;
        }
        switch (type) {
            case RDMA_CM_EVENT_DISCONNECTED:
            case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                disconnect_event_seen_ = true;
                disconnected_ = true;
                break;
            case RDMA_CM_EVENT_DEVICE_REMOVAL:
                disconnect_event_seen_ = true;
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
        return true;
    }

    void drain_cm_events() noexcept {
        while (!failed_ && poll_cm_event(0)) {}
    }

    void progress() noexcept {
        drain_cm_events();
        reap_completions();
        drain_bulk_releases();
        flush_bulk_release();
    }

    bool initialize(const Options& options) {
        if (!id_ || !id_->verbs) {
            fail("RDMA-CM did not select a verbs device");
            return false;
        }
        if (!compute_region_geometry(options.ring_bytes, local_slot_count_,
                                     region_bytes_, error_)) {
            failed_ = true;
            return false;
        }
        if (!wire::valid_bulk_geometry(options.bulk_window_bytes,
                                       options.bulk_window_count)) {
            fail("RDMA bulk window bytes must be a power of two of at least 4096, "
                 "and twice bytes times count must fit in 32 bits");
            return false;
        }
        local_bulk_window_bytes_ = options.bulk_window_bytes;
        local_bulk_window_count_ = options.bulk_window_count;
        bulk_region_bytes_ = static_cast<std::size_t>(
            std::uint64_t{2} * local_bulk_window_bytes_ * local_bulk_window_count_);
        void* mapping = ::mmap(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            fail(errno_message("mmap RDMA ring"));
            return false;
        }
        region_ = static_cast<std::byte*>(mapping);
        mapping = ::mmap(nullptr, bulk_region_bytes_, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            fail(errno_message("mmap RDMA bulk window"));
            return false;
        }
        bulk_region_ = static_cast<std::byte*>(mapping);
        const int node = hca_numa_node(id_->verbs);
        if (node >= 0) {
            if (!bind_range(region_, region_bytes_, node)) {
                fail(errno_message("bind RDMA ring to HCA NUMA node"));
                return false;
            }
            if (!bind_range(bulk_region_, bulk_region_bytes_, node)) {
                fail(errno_message("bind RDMA bulk window to HCA NUMA node"));
                return false;
            }
        }
        std::memset(region_, 0, region_bytes_);
        std::memset(bulk_region_, 0, bulk_region_bytes_);
        if (node >= 0) {
            if (!all_on_node(region_, region_bytes_, node)) {
                fail("RDMA ring could not be placed entirely on HCA NUMA node " +
                     std::to_string(node));
                return false;
            }
            if (!all_on_node(bulk_region_, bulk_region_bytes_, node)) {
                fail("RDMA bulk window could not be placed entirely on HCA NUMA node " +
                     std::to_string(node));
                return false;
            }
        }
        pd_ = ibv_alloc_pd(id_->verbs);
        if (!pd_) {
            fail(errno_message("allocate RDMA protection domain"));
            return false;
        }
        send_cq_ = ibv_create_cq(id_->verbs, kCqDepth, nullptr, nullptr, 0);
        if (!send_cq_) {
            fail(errno_message("create RDMA send completion queue"));
            return false;
        }
        region_mr_ = ibv_reg_mr(pd_, region_, region_bytes_,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!region_mr_) {
            fail(errno_message("register RDMA ring memory"));
            return false;
        }
        bulk_mr_ = ibv_reg_mr(pd_, bulk_region_, bulk_region_bytes_,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!bulk_mr_) {
            fail(errno_message("register RDMA bulk window"));
            return false;
        }
        ibv_qp_init_attr qp{};
        qp.send_cq = send_cq_;
        qp.recv_cq = send_cq_;
        qp.cap.max_send_wr = kSendQueueDepth;
        qp.cap.max_recv_wr = 1;
        qp.cap.max_send_sge = 1;
        qp.cap.max_recv_sge = 1;
        qp.cap.max_inline_data = static_cast<std::uint32_t>(
            wire::kControlRecordBytes + sizeof(std::uint64_t));
        qp.qp_type = IBV_QPT_RC;
        qp.sq_sig_all = 0;
        if (rdma_create_qp(id_, pd_, &qp) != 0) {
            fail(errno_message("create RC RDMA queue pair"));
            return false;
        }
        if (qp.cap.max_inline_data < wire::kControlRecordBytes + sizeof(std::uint64_t)) {
            fail("RDMA device cannot inline the 200-byte Goblin slot write");
            return false;
        }
        // Providers may round max_send_wr up.  Keep our logical limit at the
        // depth used to size the CQ: every bulk DATA write is signalled, so
        // adopting a larger returned depth could permit more completions than
        // the fixed CQ can hold.
        send_queue_depth_ = std::min<std::uint32_t>(qp.cap.max_send_wr,
                                                    kSendQueueDepth);
        if (send_queue_depth_ < 3) {
            fail("RDMA device supplied an unusable send queue");
            return false;
        }
        signal_every_ = std::min<std::uint32_t>(kSignalEvery, send_queue_depth_ / 2);
        try {
            bulk_tx_completion_wqes_.assign(local_bulk_window_count_, 0);
        } catch (const std::bad_alloc&) {
            fail("allocate RDMA bulk completion state");
            return false;
        }
        return true;
    }

    void reap_completions() noexcept {
        if (!send_cq_ || failed_) return;
        std::array<ibv_wc, 16> completions{};
        for (;;) {
            const int count = ibv_poll_cq(send_cq_, static_cast<int>(completions.size()),
                                          completions.data());
            if (count < 0) {
                fail("poll RDMA send completion queue failed");
                return;
            }
            if (count == 0) return;
            for (int i = 0; i < count; ++i) {
                const auto& completion = completions[static_cast<std::size_t>(i)];
                if (completion.status != IBV_WC_SUCCESS) {
                    credit_read_pending_ = false;
                    fail("RDMA work request failed: " +
                         std::string(ibv_wc_status_str(completion.status)));
                    return;
                }
                const std::uint64_t sequence = completion.wr_id & kWrSequenceMask;
                completed_wqes_ = std::max(completed_wqes_, sequence);
                if ((completion.wr_id & kWrTagMask) == kWrCreditRead) {
                    if (!credit_read_pending_ || sequence != credit_read_wqe_) {
                        fail("RDMA credit-read completion did not match the pending request");
                        return;
                    }
                    auto* source = reinterpret_cast<std::uint64_t*>(
                        region_ + wire::kCreditReadbackOffset);
                    const std::uint64_t consumed =
                        wire::sequence_from_wire(
                            __atomic_load_n(source, __ATOMIC_ACQUIRE));
                    credit_read_pending_ = false;
                    if (consumed < cached_remote_consumed_ || consumed >= send_sequence_) {
                        fail("RDMA peer returned an invalid consumed sequence");
                        return;
                    }
                    cached_remote_consumed_ = consumed;
                }
            }
        }
    }

    bool reserve_wqes(std::uint32_t count) noexcept {
        reap_completions();
        const std::uint64_t outstanding = posted_wqes_ - completed_wqes_;
        return count <= send_queue_depth_ - 1 &&
               outstanding + count <= send_queue_depth_ - 1;
    }

    bool start_remote_consumed_read() noexcept {
        if (!established_ || failed_ || disconnected_ || credit_read_pending_ ||
            !reserve_wqes(1))
            return false;
        auto* destination = reinterpret_cast<std::uint64_t*>(
            region_ + wire::kCreditReadbackOffset);
        ibv_sge sge{};
        sge.addr = reinterpret_cast<std::uintptr_t>(destination);
        sge.length = sizeof(*destination);
        sge.lkey = region_mr_->lkey;
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
            send_sequence_ >= kSequenceLimit || !control_has_credit() ||
            !reserve_wqes(1)) {
            if (send_sequence_ >= kSequenceLimit) fail("RDMA ring sequence space exhausted");
            return false;
        }
        std::array<std::byte, wire::kControlRecordBytes + sizeof(std::uint64_t)> inline_data{};
        std::memcpy(inline_data.data(), payload.data(), payload.size());
        const std::uint64_t commit =
            wire::control_commit_to_wire(send_sequence_, payload.size());
        std::memcpy(inline_data.data() + payload.size(), &commit, sizeof(commit));

        const std::uint64_t slot = (send_sequence_ - 1) % remote_.ring_slot_count;
        const std::uint64_t remote_commit =
            remote_.ring_address + wire::kControlRegionHeaderBytes +
            slot * wire::kControlSlotStride + wire::kControlCommitOffset;
        ibv_sge sge{};
        sge.addr = reinterpret_cast<std::uintptr_t>(inline_data.data());
        sge.length = static_cast<std::uint32_t>(payload.size() + sizeof(commit));
        sge.lkey = 0;
        const std::uint64_t wqe_sequence = ++posted_wqes_;
        const bool signaled = (wqe_sequence % signal_every_) == 0;
        ibv_send_wr request{};
        request.wr_id = kWrWrite | wqe_sequence;
        request.sg_list = &sge;
        request.num_sge = 1;
        request.opcode = IBV_WR_RDMA_WRITE;
        request.send_flags = IBV_SEND_INLINE | (signaled ? IBV_SEND_SIGNALED : 0);
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

    bool post_bulk(std::string_view payload) noexcept {
        if (payload.empty() || payload.size() > negotiated_bulk_window_bytes_ ||
            bulk_send_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            send_sequence_ >= kSequenceLimit)
            return false;

        const std::uint64_t capacity = std::min<std::uint64_t>(
            local_bulk_window_count_, remote_.bulk_window_count);
        if ((bulk_send_sequence_ - 1) - bulk_remote_released_ >= capacity)
            return false;

        const std::size_t local_slot = static_cast<std::size_t>(
            (bulk_send_sequence_ - 1) % local_bulk_window_count_);
        const std::uint64_t prior_completion = bulk_tx_completion_wqes_[local_slot];
        if (prior_completion != 0 && completed_wqes_ < prior_completion) return false;
        if (!control_has_credit() || !reserve_wqes(2)) return false;

        std::byte* source = bulk_region_ + local_slot * local_bulk_window_bytes_;
        std::memcpy(source, payload.data(), payload.size());

        const auto ready = wire::encode_bulk_ready(
            static_cast<std::uint32_t>(payload.size()), bulk_send_sequence_);
        std::array<std::byte, wire::kControlRecordBytes + sizeof(std::uint64_t)>
            ready_inline{};
        std::memcpy(ready_inline.data(), ready.data(), ready.size());
        const std::uint64_t commit =
            wire::control_commit_to_wire(send_sequence_, ready.size());
        std::memcpy(ready_inline.data() + ready.size(), &commit, sizeof(commit));

        const std::uint64_t remote_bulk_slot =
            (bulk_send_sequence_ - 1) % remote_.bulk_window_count;
        const std::uint64_t remote_receive_half =
            static_cast<std::uint64_t>(remote_.bulk_window_bytes) *
            remote_.bulk_window_count;
        const std::uint64_t remote_bulk_address = remote_.bulk_address +
            remote_receive_half + remote_bulk_slot * remote_.bulk_window_bytes;
        const std::uint64_t remote_control_slot =
            (send_sequence_ - 1) % remote_.ring_slot_count;
        const std::uint64_t remote_control_commit = remote_.ring_address +
            wire::kControlRegionHeaderBytes +
            remote_control_slot * wire::kControlSlotStride +
            wire::kControlCommitOffset;

        ibv_sge bulk_sge{};
        bulk_sge.addr = reinterpret_cast<std::uintptr_t>(source);
        bulk_sge.length = static_cast<std::uint32_t>(payload.size());
        bulk_sge.lkey = bulk_mr_->lkey;
        ibv_sge ready_sge{};
        ready_sge.addr = reinterpret_cast<std::uintptr_t>(ready_inline.data());
        ready_sge.length = static_cast<std::uint32_t>(ready.size() + sizeof(commit));
        ready_sge.lkey = 0;

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
        data_request.wr.rdma.remote_addr = remote_bulk_address;
        data_request.wr.rdma.rkey = remote_.bulk_rkey;

        ibv_send_wr* bad = nullptr;
        const int result = ibv_post_send(id_->qp, &data_request, &bad);
        if (result != 0) {
            // A verbs provider may have accepted a prefix of the WR chain.
            // The logical byte stream is no longer retryable in that case.
            fail(errno_message("post RDMA bulk write and READY", result));
            return false;
        }
        posted_wqes_ += 2;
        bulk_tx_completion_wqes_[local_slot] = data_wqe;
        ++bulk_send_sequence_;
        ++send_sequence_;
        return true;
    }

    std::optional<std::string_view> peek_control_record() noexcept {
        const std::uint64_t slot = (receive_sequence_ - 1) % local_slot_count_;
        auto* commit_address = reinterpret_cast<std::uint64_t*>(
            region_ + wire::kControlRegionHeaderBytes +
            slot * wire::kControlSlotStride + wire::kControlCommitOffset);
        const std::uint64_t commit = wire::control_commit_from_wire(
            __atomic_load_n(commit_address, __ATOMIC_ACQUIRE));
        const std::uint64_t sequence = commit >> 8;
        if (sequence < receive_sequence_) return std::nullopt;
        if (sequence != receive_sequence_) {
            fail("RDMA ring sequence jumped from " + std::to_string(receive_sequence_) +
                 " to " + std::to_string(sequence));
            return std::nullopt;
        }
        const std::size_t length = static_cast<std::size_t>(commit & 0xffU);
        if (length == 0 || length > wire::kControlRecordBytes) {
            fail("RDMA ring published an invalid control record length");
            return std::nullopt;
        }
        return std::string_view(
            reinterpret_cast<const char*>(commit_address) - length, length);
    }

    void consume_control_record() noexcept {
        const std::uint64_t consumed = receive_sequence_++;
        auto* word = reinterpret_cast<std::uint64_t*>(
            region_ + wire::kConsumedOffset);
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

    // A writer may fill every remote bulk slot before it begins reading the
    // memcache reply. RELEASE is transport credit, not application data, so
    // service consecutive leading releases from every progress path. Stop at
    // the first INLINE/READY record to preserve the logical byte stream.
    void drain_bulk_releases() noexcept {
        if (exposed_kind_ != ExposedKind::none || failed_ || disconnected_) return;
        for (;;) {
            const auto record = peek_control_record();
            if (!record) return;
            wire::FrameView frame;
            if (!wire::decode_frame(*record, frame)) {
                fail("RDMA control ring published an invalid version-3 frame");
                return;
            }
            if (frame.type != wire::FrameType::bulk_release) return;
            if (!accept_bulk_release(frame.sequence)) return;
            consume_control_record();
        }
    }

    void flush_bulk_release() noexcept {
        if (!bulk_release_pending_ || !established_ || failed_ || disconnected_) return;
        const auto frame = wire::encode_bulk_release(bulk_release_sequence_);
        if (post_control_record(std::string_view(
                reinterpret_cast<const char*>(frame.data()), frame.size())))
            bulk_release_pending_ = false;
    }

    bool open_impl(const Options& options, std::string& error) {
        std::uint32_t ignored_slots = 0;
        std::size_t ignored_bytes = 0;
        if (!compute_region_geometry(options.ring_bytes, ignored_slots, ignored_bytes, error))
            return false;
        if (!wire::valid_bulk_geometry(options.bulk_window_bytes,
                                       options.bulk_window_count)) {
            error = "RDMA bulk window bytes must be a power of two of at least 4096, "
                    "and twice bytes times count must fit in 32 bits";
            return false;
        }
        const auto deadline = Clock::now() + options.connect_timeout;
        channel_ = rdma_create_event_channel();
        if (!channel_) {
            error = errno_message("create RDMA-CM client event channel");
            return false;
        }
        if (rdma_create_id(channel_, &id_, this, RDMA_PS_TCP) != 0) {
            error = errno_message("create RDMA-CM client id");
            return false;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST; // keep name resolution inside the advertised deadline
        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(options.port);
        const int lookup = ::getaddrinfo(options.address.c_str(), port.c_str(), &hints, &addresses);
        if (lookup != 0) {
            error = "parse numeric RDMA server address: " + std::string(gai_strerror(lookup));
            return false;
        }
        const int resolve_timeout = remaining_ms(deadline);
        if (resolve_timeout == 0 ||
            rdma_resolve_addr(id_, nullptr, addresses->ai_addr, resolve_timeout) != 0) {
            ::freeaddrinfo(addresses);
            error = resolve_timeout == 0 ? "timed out resolving RDMA-CM address"
                                         : errno_message("resolve RDMA-CM address");
            return false;
        }
        ::freeaddrinfo(addresses);
        CmEvent event;
        if (!wait_for_event(channel_, RDMA_CM_EVENT_ADDR_RESOLVED, deadline, event, error))
            return false;
        const int route_timeout = remaining_ms(deadline);
        if (route_timeout == 0 || rdma_resolve_route(id_, route_timeout) != 0) {
            error = route_timeout == 0 ? "timed out resolving RDMA-CM route"
                                       : errno_message("resolve RDMA-CM route");
            return false;
        }
        if (!wait_for_event(channel_, RDMA_CM_EVENT_ROUTE_RESOLVED, deadline, event, error))
            return false;
        if (!initialize(options)) {
            error = error_;
            return false;
        }
        const auto descriptor = wire::encode_descriptor(local_descriptor());
        rdma_conn_param parameters{};
        parameters.private_data = descriptor.data();
        parameters.private_data_len = descriptor.size();
        parameters.responder_resources = 1;
        parameters.initiator_depth = 1;
        parameters.retry_count = 7;
        parameters.rnr_retry_count = 7;
        if (rdma_connect(id_, &parameters) != 0) {
            error = errno_message("connect RDMA-CM endpoint");
            return false;
        }
        if (!wait_for_event(channel_, RDMA_CM_EVENT_ESTABLISHED, deadline, event, error))
            return false;
        if (!decode_descriptor(event.private_data.data(), event.private_data_len, remote_, error))
            return false;
        negotiated_bulk_window_bytes_ = std::min<std::size_t>(
            local_bulk_window_bytes_, remote_.bulk_window_bytes);
        established_ = true;
        return true;
    }
};

} // namespace

bool rdma_available() noexcept { return true; }

std::unique_ptr<Transport> connect_rdma(const Options& options) {
    std::string error;
    auto transport = RdmaTransport::open(options, error);
    if (!transport) {
        if (error.find("timed out") != std::string::npos) throw TimeoutError(error);
        throw ConnectionError(error.empty() ? "could not open RDMA connection" : error);
    }
    return transport;
}

} // namespace goblin::client
