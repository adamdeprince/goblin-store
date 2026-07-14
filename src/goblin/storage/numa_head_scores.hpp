// NUMA-local dense access scores for fixed-size RAM heads.
//
// A fixed head has a deterministic slot: block * (block_bytes / head_bytes) +
// offset / head_bytes.  Keeping those doubles in node-bound arrays lets one worker on each NUMA
// node rank its own blocks without streaming the score table over the socket interconnect.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"
#include "goblin/storage/index.hpp" // HeadLoc

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace goblin::storage {

struct ScoreCandidate {
    std::size_t index = 0;
    double score = 0.0;
};

struct ScoreExtrema {
    std::optional<ScoreCandidate> min;
    std::optional<ScoreCandidate> max;
};

// `eligible` contains zero or one for each score. NaN scores are always ignored. Both scanners use
// the lowest array index to break equal-score ties. If the spans differ, only their common prefix is
// examined. The automatic entry point uses AVX1 when the compiler, CPU, and OS support it.
ScoreExtrema scan_score_extrema_scalar(std::span<const double> scores,
                                       std::span<const double> eligible) noexcept;
ScoreExtrema scan_score_extrema(std::span<const double> scores,
                                std::span<const double> eligible) noexcept;
bool avx_score_scan_available() noexcept;

struct NumaScoreRegionConfig {
    unsigned first_block = 0;
    std::size_t block_count = 0;
    std::optional<unsigned> numa_node;
    std::vector<unsigned> cpus;
};

// Block indexes in these results are global BufferPool block indexes, not indexes into a worker's
// scratch array. Region zero defines the preferred side of a promotion exchange.
struct NumaPromotionCandidates {
    std::optional<ScoreCandidate> cold_preferred;
    std::optional<ScoreCandidate> hot_foreign;
};

class NumaHeadScoreTable {
public:
    static Result<NumaHeadScoreTable> create(
        Size block_bytes, Size head_bytes, std::span<const NumaScoreRegionConfig> regions);

    ~NumaHeadScoreTable();
    NumaHeadScoreTable(NumaHeadScoreTable&&) noexcept;
    NumaHeadScoreTable& operator=(NumaHeadScoreTable&&) noexcept;
    NumaHeadScoreTable(const NumaHeadScoreTable&) = delete;
    NumaHeadScoreTable& operator=(const NumaHeadScoreTable&) = delete;

    // The caller serializes every operation with TierManager's exclusive lock/maintenance gate.
    // Only fixed heads (`loc.len == head_bytes`) belong in this table. NaN is reserved as the empty
    // slot marker; positive infinity remains a valid (saturated) score.
    Status publish(HeadLoc loc, double score);
    Status erase(HeadLoc loc);
    Status increment(HeadLoc loc, double amount);
    Status decay(double factor); // dispatched so every score is read/written by its node-local worker

    Result<NumaPromotionCandidates> promotion_candidates(
        std::span<const unsigned> pinned_blocks);
    Status swap_blocks(unsigned first, unsigned second); // score slices follow the moved head bytes

    std::optional<double> score(HeadLoc loc) const noexcept;
    std::size_t worker_count() const noexcept;

private:
    struct Impl;
    explicit NumaHeadScoreTable(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace goblin::storage
