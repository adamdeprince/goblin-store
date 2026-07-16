#include "goblin/storage/numa_head_scores.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/net/numa.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace goblin::storage {

namespace {

constexpr double kEmptyScore = std::numeric_limits<double>::quiet_NaN();

void consider_min(std::optional<ScoreCandidate>& current, ScoreCandidate candidate) noexcept {
    if (!current || candidate.score < current->score ||
        (candidate.score == current->score && candidate.index < current->index))
        current = candidate;
}

void consider_max(std::optional<ScoreCandidate>& current, ScoreCandidate candidate) noexcept {
    if (!current || candidate.score > current->score ||
        (candidate.score == current->score && candidate.index < current->index))
        current = candidate;
}

bool checked_mul(std::size_t a, std::size_t b, std::size_t& result) noexcept {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return false;
    result = a * b;
    return true;
}

bool checked_add(std::size_t a, std::size_t b, std::size_t& result) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a) return false;
    result = a + b;
    return true;
}

bool align_checked(std::size_t value, std::size_t alignment, std::size_t& result) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) return false;
    result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

void aggregate_complete_blocks_scalar(const std::atomic<double>* scores, std::size_t block_count,
                                      std::size_t slots_per_block, double* totals,
                                      double* eligible) noexcept {
    for (std::size_t block = 0; block < block_count; ++block) {
        double total = 0.0;
        bool complete = true;
        const std::atomic<double>* block_scores = scores + block * slots_per_block;
        for (std::size_t slot = 0; slot < slots_per_block; ++slot) {
            const double score = block_scores[slot].load(std::memory_order_relaxed);
            if (std::isnan(score)) {
                complete = false;
                continue;
            }
            total += score;
        }
        totals[block] = total;
        eligible[block] = complete ? 1.0 : 0.0;
    }
}

} // namespace

ScoreExtrema scan_score_extrema_scalar(std::span<const double> scores,
                                       std::span<const double> eligible) noexcept {
    ScoreExtrema out;
    const std::size_t n = std::min(scores.size(), eligible.size());
    for (std::size_t i = 0; i < n; ++i) {
        const double value = scores[i];
        if (eligible[i] != 1.0 || std::isnan(value)) continue;
        consider_min(out.min, ScoreCandidate{i, value});
        consider_max(out.max, ScoreCandidate{i, value});
    }
    return out;
}

struct NumaHeadScoreTable::Impl {
    struct RegionState {
        NumaScoreRegionConfig config;
        std::unique_ptr<core::BlockPool> backing;
        std::atomic<double>* scores = nullptr; // one per fixed-head slot; NaN means empty
        double* block_totals = nullptr; // promotion scratch, one per allocation block
        double* eligible = nullptr;     // promotion scratch, exactly zero or one
        std::size_t slot_count = 0;
        std::size_t worker_index = 0;
        bool scores_constructed = false;
    };

    struct alignas(64) WorkerState {
        std::optional<unsigned> numa_node;
        std::vector<unsigned> cpus;
        std::vector<std::size_t> region_indexes;
        bool contains_preferred_region = false;
        bool startup_reported = false; // protected by control_mutex
        // One compact cross-node result: preferred worker publishes its minimum; every foreign
        // worker publishes its maximum. The full node-local extrema never leave the worker.
        std::optional<ScoreCandidate> result;
        std::vector<unsigned> pinned_blocks; // compact input containing only this node's pins
        std::thread thread;
    };

    enum class Command { none, promotion, decay };

    Size block_bytes = 0;
    Size head_bytes = 0;
    std::size_t slots_per_block = 0;
    std::vector<RegionState> regions;
    std::vector<std::unique_ptr<WorkerState>> workers;
    std::size_t preferred_worker = 0;

    std::mutex control_mutex;
    std::condition_variable control_cv;
    bool stopping = false;
    std::uint64_t epoch = 0;
    Command command = Command::none;
    double decay_factor = 1.0;
    std::size_t completed = 0;
    std::size_t started = 0;
    std::optional<Error> startup_error;
    std::optional<Error> runtime_error;

    ~Impl() {
        shutdown();
        for (auto& region : regions)
            if (region.scores_constructed)
                std::destroy_n(region.scores, region.slot_count);
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(control_mutex);
            stopping = true;
        }
        control_cv.notify_all();
        for (auto& worker : workers)
            if (worker->thread.joinable()) worker->thread.join();
    }

    static Error invalid_location(const HeadLoc& loc) {
        return Error{Errc::invalid_argument,
                     "fixed-head score location is outside the configured NUMA regions: block " +
                         std::to_string(loc.block) + ", offset " + std::to_string(loc.offset) +
                         ", len " + std::to_string(loc.len)};
    }

    struct SlotRef {
        RegionState* region = nullptr;
        std::size_t local_block = 0;
        std::size_t slot_in_block = 0;
        std::atomic<double>* value = nullptr;
    };

    struct ConstSlotRef {
        const RegionState* region = nullptr;
        std::size_t local_block = 0;
        std::size_t slot_in_block = 0;
        const std::atomic<double>* value = nullptr;
    };

    std::optional<SlotRef> find_slot(HeadLoc loc) noexcept {
        if (loc.len != head_bytes || static_cast<Size>(loc.offset) >= block_bytes ||
            static_cast<Size>(loc.offset) % head_bytes != 0 ||
            static_cast<Size>(loc.offset) + head_bytes > block_bytes)
            return std::nullopt;
        for (auto& region : regions) {
            const std::uint64_t begin = region.config.first_block;
            const std::uint64_t end = begin + region.config.block_count;
            if (loc.block < begin || loc.block >= end) continue;
            const std::size_t local_block = static_cast<std::size_t>(loc.block - begin);
            const std::size_t slot_in_block = static_cast<std::size_t>(loc.offset / head_bytes);
            const std::size_t slot = local_block * slots_per_block + slot_in_block;
            return SlotRef{&region, local_block, slot_in_block, region.scores + slot};
        }
        return std::nullopt;
    }

    std::optional<ConstSlotRef> find_slot(HeadLoc loc) const noexcept {
        if (loc.len != head_bytes || static_cast<Size>(loc.offset) >= block_bytes ||
            static_cast<Size>(loc.offset) % head_bytes != 0 ||
            static_cast<Size>(loc.offset) + head_bytes > block_bytes)
            return std::nullopt;
        for (const auto& region : regions) {
            const std::uint64_t begin = region.config.first_block;
            const std::uint64_t end = begin + region.config.block_count;
            if (loc.block < begin || loc.block >= end) continue;
            const std::size_t local_block = static_cast<std::size_t>(loc.block - begin);
            const std::size_t slot_in_block = static_cast<std::size_t>(loc.offset / head_bytes);
            const std::size_t slot = local_block * slots_per_block + slot_in_block;
            return ConstSlotRef{&region, local_block, slot_in_block, region.scores + slot};
        }
        return std::nullopt;
    }

    struct BlockRef {
        RegionState* region = nullptr;
        std::size_t local_block = 0;
    };

    std::optional<BlockRef> find_block(unsigned block) noexcept {
        for (auto& region : regions) {
            const std::uint64_t begin = region.config.first_block;
            const std::uint64_t end = begin + region.config.block_count;
            if (block >= begin && block < end)
                return BlockRef{&region, static_cast<std::size_t>(block - begin)};
        }
        return std::nullopt;
    }

    ScoreExtrema scan_worker(const WorkerState& worker,
                             std::span<const unsigned> pinned) noexcept {
        ScoreExtrema summary;
        for (const std::size_t region_index : worker.region_indexes) {
            RegionState& region = regions[region_index];
            aggregate_complete_blocks_scalar(region.scores, region.config.block_count,
                                             slots_per_block, region.block_totals,
                                             region.eligible);

            // Pins are an epoch input. Each node reads this compact list once, then changes only its
            // node-local eligibility scratch; the dense score and block arrays never cross nodes.
            const std::uint64_t begin = region.config.first_block;
            const std::uint64_t end = begin + region.config.block_count;
            for (const unsigned pinned_block : pinned)
                if (pinned_block >= begin && pinned_block < end)
                    region.eligible[static_cast<std::size_t>(pinned_block - begin)] = 0.0;

            ScoreExtrema local = scan_score_extrema_scalar(
                std::span<const double>(region.block_totals, region.config.block_count),
                std::span<const double>(region.eligible, region.config.block_count));
            if (local.min) {
                local.min->index += region.config.first_block;
                consider_min(summary.min, *local.min);
            }
            if (local.max) {
                local.max->index += region.config.first_block;
                consider_max(summary.max, *local.max);
            }
        }
        return summary;
    }

    void decay_worker(const WorkerState& worker, double factor) noexcept {
        for (const std::size_t region_index : worker.region_indexes) {
            RegionState& region = regions[region_index];
            for (std::size_t slot = 0; slot < region.slot_count; ++slot) {
                auto& score = region.scores[slot];
                double current = score.load(std::memory_order_relaxed);
                while (!std::isnan(current) &&
                       !score.compare_exchange_weak(current, current * factor,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                }
            }
        }
    }

    void worker_main(WorkerState& worker) {
        Status bound;
        if (worker.numa_node)
            bound = net::bind_numa_worker(*worker.numa_node, worker.cpus);

        if (bound) {
            // First-touch every node-bound page only after this worker has installed both its CPU
            // affinity and default memory policy. Explicit mbind on the backing mappings is the
            // placement guarantee; local first touch also covers null-node portable test regions.
            for (const std::size_t region_index : worker.region_indexes) {
                RegionState& region = regions[region_index];
                for (std::size_t slot = 0; slot < region.slot_count; ++slot)
                    std::construct_at(region.scores + slot, kEmptyScore);
                region.scores_constructed = true;
                std::fill_n(region.block_totals, region.config.block_count, 0.0);
                std::fill_n(region.eligible, region.config.block_count, 0.0);
            }
        }

        {
            std::lock_guard lock(control_mutex);
            if (!bound && !startup_error) startup_error = std::move(bound.error());
            worker.startup_reported = true;
            ++started;
        }
        control_cv.notify_all();
        if (!bound) return;

        std::uint64_t seen_epoch = 0;
        for (;;) {
            Command next = Command::none;
            double factor = 1.0;
            std::span<const unsigned> pins;
            {
                std::unique_lock lock(control_mutex);
                control_cv.wait(lock, [&] { return stopping || epoch != seen_epoch; });
                if (stopping) return;
                seen_epoch = epoch;
                next = command;
                factor = decay_factor;
                pins = worker.pinned_blocks;
            }

            if (next == Command::promotion) {
                const ScoreExtrema local = scan_worker(worker, pins);
                worker.result = worker.contains_preferred_region ? local.min : local.max;
            } else if (next == Command::decay) {
                decay_worker(worker, factor);
            }

            {
                std::lock_guard lock(control_mutex);
                ++completed;
            }
            control_cv.notify_all();
        }
    }

    void worker_entry(WorkerState& worker) noexcept {
        try {
            worker_main(worker);
        } catch (const std::bad_alloc&) {
            worker_failed(worker, Error{Errc::out_of_memory, "NUMA score worker allocation failed"});
        } catch (const std::system_error& e) {
            worker_failed(worker,
                          Error{Errc::io_error,
                                std::string("NUMA score worker synchronization failed: ") + e.what()});
        } catch (const std::exception& e) {
            worker_failed(worker,
                          Error{Errc::io_error,
                                std::string("NUMA score worker failed: ") + e.what()});
        } catch (...) {
            worker_failed(worker, Error{Errc::io_error, "NUMA score worker failed"});
        }
    }

    void worker_failed(WorkerState& worker, Error failure) noexcept {
        try {
            {
                std::lock_guard lock(control_mutex);
                if (!worker.startup_reported) {
                    worker.startup_reported = true;
                    if (!startup_error) startup_error = std::move(failure);
                    ++started;
                } else {
                    if (!runtime_error) runtime_error = std::move(failure);
                    stopping = true;
                }
            }
            control_cv.notify_all();
        } catch (...) {
            // There is no recoverable action if even recording a worker failure cannot allocate.
            std::terminate();
        }
    }

    Status dispatch(Command next, double factor = 1.0,
                    std::span<const unsigned> pins = {}) {
        std::unique_lock lock(control_mutex);
        if (stopping) {
            if (runtime_error) return std::unexpected(*runtime_error);
            return err(Errc::io_error, "NUMA score workers are stopped");
        }
        completed = 0;
        runtime_error.reset();
        command = next;
        decay_factor = factor;
        try {
            for (auto& worker : workers) worker->pinned_blocks.clear();
            for (const unsigned block : pins) {
                const auto location = find_block(block);
                if (!location) continue;
                workers[location->region->worker_index]->pinned_blocks.push_back(block);
            }
            for (auto& worker : workers) {
                std::sort(worker->pinned_blocks.begin(), worker->pinned_blocks.end());
                worker->pinned_blocks.erase(
                    std::unique(worker->pinned_blocks.begin(), worker->pinned_blocks.end()),
                    worker->pinned_blocks.end());
            }
        } catch (const std::bad_alloc&) {
            return err(Errc::out_of_memory, "allocate NUMA score pin epoch");
        }
        ++epoch;
        control_cv.notify_all();
        control_cv.wait(lock, [&] { return completed == workers.size() || runtime_error.has_value(); });
        if (runtime_error) return std::unexpected(*runtime_error);
        return {};
    }
};

Result<NumaHeadScoreTable> NumaHeadScoreTable::create(
    Size block_bytes, Size head_bytes, std::span<const NumaScoreRegionConfig> configs) {
    if (!is_power_of_two(block_bytes) || !is_power_of_two(head_bytes) || head_bytes == 0 ||
        head_bytes > block_bytes || block_bytes % head_bytes != 0)
        return err(Errc::invalid_argument,
                   "NUMA head scores require power-of-two head/block sizes and head must divide block");
    if (configs.empty())
        return err(Errc::invalid_argument, "NUMA head scores require at least one region");
    if (block_bytes > std::numeric_limits<std::uint32_t>::max() ||
        head_bytes > std::numeric_limits<std::uint32_t>::max())
        return err(Errc::invalid_argument, "NUMA head score geometry exceeds HeadLoc width");
    try {
        auto impl = std::make_unique<Impl>();
        impl->block_bytes = block_bytes;
        impl->head_bytes = head_bytes;
        impl->slots_per_block = static_cast<std::size_t>(block_bytes / head_bytes);
        impl->regions.reserve(configs.size());

        for (std::size_t i = 0; i < configs.size(); ++i) {
            const auto& config = configs[i];
            if (config.block_count == 0 ||
                config.block_count >
                    static_cast<std::size_t>(std::numeric_limits<unsigned>::max() -
                                             config.first_block))
                return err(Errc::invalid_argument,
                           "NUMA score region has an empty or overflowing block range");
            const std::uint64_t begin = config.first_block;
            const std::uint64_t end = begin + config.block_count;
            for (std::size_t prior = 0; prior < i; ++prior) {
                const std::uint64_t prior_begin = configs[prior].first_block;
                const std::uint64_t prior_end = prior_begin + configs[prior].block_count;
                if (begin < prior_end && prior_begin < end)
                    return err(Errc::invalid_argument, "NUMA score block regions overlap");
            }

            std::size_t slot_count = 0;
            std::size_t score_bytes = 0;
            std::size_t block_bytes_needed = 0;
            if (!checked_mul(config.block_count, impl->slots_per_block, slot_count) ||
                !checked_mul(slot_count, sizeof(std::atomic<double>), score_bytes) ||
                !checked_mul(config.block_count, sizeof(double), block_bytes_needed))
                return err(Errc::out_of_memory, "NUMA score array size overflows address space");

            std::size_t totals_offset = 0;
            std::size_t eligibility_offset = 0;
            std::size_t used = 0;
            std::size_t mapping_bytes = 0;
            if (!align_checked(score_bytes,
                               std::max<std::size_t>(64, alignof(std::atomic<double>)),
                               totals_offset) ||
                !checked_add(totals_offset, block_bytes_needed, used) ||
                !align_checked(used, 64, eligibility_offset) ||
                !checked_add(eligibility_offset, block_bytes_needed, used) ||
                !align_checked(used, static_cast<std::size_t>(kDeviceBlock), mapping_bytes))
                return err(Errc::out_of_memory, "NUMA score mapping size overflows address space");

            const std::array<core::BlockPoolRegion, 1> backing_region{{
                {static_cast<Size>(mapping_bytes), config.numa_node},
            }};
            auto backing = core::BlockPool::create_regions(
                kDeviceBlock, backing_region, /*lock_memory=*/false, /*try_hugetlb=*/false,
                kDeviceBlock);
            if (!backing) return std::unexpected(backing.error());
            auto owned_backing = std::make_unique<core::BlockPool>(std::move(*backing));
            std::byte* base = owned_backing->block_data(0);

            Impl::RegionState region;
            region.config = config;
            region.scores = reinterpret_cast<std::atomic<double>*>(base);
            region.block_totals = reinterpret_cast<double*>(base + totals_offset);
            region.eligible = reinterpret_cast<double*>(base + eligibility_offset);
            region.slot_count = slot_count;
            region.backing = std::move(owned_backing);
            impl->regions.push_back(std::move(region));
        }

        // A physical node gets one persistent worker even if a caller describes it with several
        // logical regions. A null node is deliberately not grouped: portable tests use null regions
        // as independently owned logical NUMA slices.
        for (std::size_t region_index = 0; region_index < impl->regions.size(); ++region_index) {
            auto& region = impl->regions[region_index];
            std::optional<std::size_t> worker_index;
            if (region.config.numa_node) {
                for (std::size_t candidate = 0; candidate < impl->workers.size(); ++candidate)
                    if (impl->workers[candidate]->numa_node == region.config.numa_node) {
                        worker_index = candidate;
                        break;
                    }
            }
            if (!worker_index) {
                worker_index = impl->workers.size();
                auto worker = std::make_unique<Impl::WorkerState>();
                worker->numa_node = region.config.numa_node;
                impl->workers.push_back(std::move(worker));
            }
            auto& worker = *impl->workers[*worker_index];
            worker.region_indexes.push_back(region_index);
            worker.cpus.insert(worker.cpus.end(), region.config.cpus.begin(), region.config.cpus.end());
            std::sort(worker.cpus.begin(), worker.cpus.end());
            worker.cpus.erase(std::unique(worker.cpus.begin(), worker.cpus.end()), worker.cpus.end());
            region.worker_index = *worker_index;
            if (region_index == 0) {
                worker.contains_preferred_region = true;
                impl->preferred_worker = *worker_index;
            }
        }

        try {
            for (auto& worker : impl->workers)
                worker->thread = std::thread([state = impl.get(), worker = worker.get()] {
                    state->worker_entry(*worker);
                });
        } catch (const std::system_error& e) {
            return err(Errc::io_error,
                       std::string("start NUMA score worker: ") + e.what());
        }

        {
            std::unique_lock lock(impl->control_mutex);
            impl->control_cv.wait(lock, [&] { return impl->started == impl->workers.size(); });
            if (impl->startup_error) return std::unexpected(*impl->startup_error);
        }
        return NumaHeadScoreTable(std::move(impl));
    } catch (const std::bad_alloc&) {
        return err(Errc::out_of_memory, "allocate NUMA head score table");
    }
}

NumaHeadScoreTable::NumaHeadScoreTable(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NumaHeadScoreTable::~NumaHeadScoreTable() = default;
NumaHeadScoreTable::NumaHeadScoreTable(NumaHeadScoreTable&&) noexcept = default;
NumaHeadScoreTable& NumaHeadScoreTable::operator=(NumaHeadScoreTable&&) noexcept = default;

Status NumaHeadScoreTable::publish(HeadLoc loc, double value) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    if (std::isnan(value) || value < 0.0)
        return err(Errc::invalid_argument, "fixed-head score must be nonnegative and not NaN");
    const auto slot = impl_->find_slot(loc);
    if (!slot) return std::unexpected(Impl::invalid_location(loc));
    double current = slot->value->load(std::memory_order_relaxed);
    if (!std::isnan(current))
        return err(Errc::invalid_argument, "fixed-head score slot is already occupied");
    if (!slot->value->compare_exchange_strong(current, value, std::memory_order_relaxed,
                                              std::memory_order_relaxed))
        return err(Errc::invalid_argument, "fixed-head score slot changed during publication");
    return {};
}

Result<double> NumaHeadScoreTable::extract(HeadLoc loc) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    const auto slot = impl_->find_slot(loc);
    if (!slot) return std::unexpected(Impl::invalid_location(loc));
    const double value = slot->value->exchange(kEmptyScore, std::memory_order_relaxed);
    if (std::isnan(value)) return err(Errc::not_found, "fixed-head score slot is empty");
    return value;
}

Status NumaHeadScoreTable::erase(HeadLoc loc) {
    auto value = extract(loc);
    if (!value) return std::unexpected(value.error());
    return {};
}

Status NumaHeadScoreTable::increment(HeadLoc loc, double amount) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    if (!std::isfinite(amount) || amount <= 0.0)
        return err(Errc::invalid_argument, "fixed-head score increment must be finite and positive");
    const auto slot = impl_->find_slot(loc);
    if (!slot) return std::unexpected(Impl::invalid_location(loc));
    double current = slot->value->load(std::memory_order_relaxed);
    while (!std::isnan(current)) {
        if (slot->value->compare_exchange_weak(current, current + amount,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed))
            return {};
    }
    return err(Errc::not_found, "fixed-head score slot is empty");
}

Status NumaHeadScoreTable::decay(double factor) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    if (!std::isfinite(factor) || factor <= 0.0 || factor >= 1.0)
        return err(Errc::invalid_argument, "fixed-head score decay must be strictly between zero and one");
    return impl_->dispatch(Impl::Command::decay, factor);
}

Result<NumaPromotionCandidates> NumaHeadScoreTable::promotion_candidates(
    std::span<const unsigned> pinned_blocks) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    if (auto status = impl_->dispatch(Impl::Command::promotion, 1.0, pinned_blocks); !status)
        return std::unexpected(status.error());

    NumaPromotionCandidates out;
    out.cold_preferred = impl_->workers[impl_->preferred_worker]->result;
    for (std::size_t i = 0; i < impl_->workers.size(); ++i) {
        if (i == impl_->preferred_worker) continue;
        if (impl_->workers[i]->result)
            consider_max(out.hot_foreign, *impl_->workers[i]->result);
    }
    return out;
}

Status NumaHeadScoreTable::swap_blocks(unsigned first, unsigned second) {
    if (!impl_) return err(Errc::invalid_argument, "moved-from NUMA head score table");
    if (first == second) return {};
    const auto first_ref = impl_->find_block(first);
    const auto second_ref = impl_->find_block(second);
    if (!first_ref || !second_ref)
        return err(Errc::invalid_argument, "NUMA score block swap is outside configured regions");
    std::atomic<double>* first_scores =
        first_ref->region->scores + first_ref->local_block * impl_->slots_per_block;
    std::atomic<double>* second_scores =
        second_ref->region->scores + second_ref->local_block * impl_->slots_per_block;
    for (std::size_t slot = 0; slot < impl_->slots_per_block; ++slot) {
        const double first_value = first_scores[slot].load(std::memory_order_relaxed);
        const double second_value = second_scores[slot].load(std::memory_order_relaxed);
        first_scores[slot].store(second_value, std::memory_order_relaxed);
        second_scores[slot].store(first_value, std::memory_order_relaxed);
    }
    return {};
}

std::optional<double> NumaHeadScoreTable::score(HeadLoc loc) const noexcept {
    if (!impl_) return std::nullopt;
    const auto slot = std::as_const(*impl_).find_slot(loc);
    if (!slot) return std::nullopt;
    const double value = slot->value->load(std::memory_order_relaxed);
    if (std::isnan(value)) return std::nullopt;
    return value;
}

std::size_t NumaHeadScoreTable::worker_count() const noexcept {
    return impl_ ? impl_->workers.size() : 0;
}

} // namespace goblin::storage
