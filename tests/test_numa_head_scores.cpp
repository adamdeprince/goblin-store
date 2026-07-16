#include "mini_test.hpp"

#include "goblin/storage/numa_head_scores.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <vector>

using namespace goblin;
using namespace goblin::storage;

namespace {

ScoreExtrema reference_extrema(std::span<const double> scores,
                               std::span<const double> eligible) {
    ScoreExtrema out;
    const std::size_t count = std::min(scores.size(), eligible.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (eligible[i] == 0.0 || std::isnan(scores[i])) continue;
        if (!out.min || scores[i] < out.min->score ||
            (scores[i] == out.min->score && i < out.min->index))
            out.min = ScoreCandidate{i, scores[i]};
        if (!out.max || scores[i] > out.max->score ||
            (scores[i] == out.max->score && i < out.max->index))
            out.max = ScoreCandidate{i, scores[i]};
    }
    return out;
}

void check_candidate(const std::optional<ScoreCandidate>& actual,
                     const std::optional<ScoreCandidate>& expected) {
    CHECK_EQ(actual.has_value(), expected.has_value());
    if (!actual || !expected) return;
    CHECK_EQ(actual->index, expected->index);
    CHECK_EQ(actual->score, expected->score);
}

void check_extrema(const ScoreExtrema& actual, const ScoreExtrema& expected) {
    check_candidate(actual.min, expected.min);
    check_candidate(actual.max, expected.max);
}

HeadLoc head(unsigned block, std::uint32_t offset, std::uint32_t len = 4 * KiB) {
    return HeadLoc{block, offset, len};
}

void publish(NumaHeadScoreTable& table, unsigned block, double first, double second) {
    CHECK(table.publish(head(block, 0), first).has_value());
    CHECK(table.publish(head(block, 4 * KiB), second).has_value());
}

} // namespace

TEST("numa score scan: scalar path handles every short-span length") {
    // Exercise empty input, unequal spans, and a range of short and longer arrays. The fixed
    // pattern puts eligible and ineligible values at several positions.
    for (std::size_t count = 0; count <= 19; ++count) {
        std::vector<double> scores(count);
        std::vector<double> eligible(count, 1.0);
        for (std::size_t i = 0; i < count; ++i) {
            scores[i] = static_cast<double>((i * 37 + count * 11) % 29) - 14.0;
            if ((i + count) % 5 == 2) eligible[i] = 0.0;
        }
        if (count > 1) {
            scores[count - 1] = -1000.0 - static_cast<double>(count);
            eligible[count - 1] = 1.0;
        }
        if (count > 4) {
            scores[3] = 1000.0 + static_cast<double>(count);
            eligible[3] = 1.0;
        }

        const auto expected = reference_extrema(scores, eligible);
        check_extrema(scan_score_extrema_scalar(scores, eligible), expected);
    }
}

TEST("numa score scan: ties, eligibility, NaN, infinity, and common-prefix length") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::array scores{nan, 4.0, -inf, 4.0, inf, -99.0, 4000.0};
    const std::array eligible{1.0, 1.0, 1.0, 1.0, 1.0, 0.0};

    // scores[6] is outside the common prefix, scores[5] is ineligible, NaN is always ignored,
    // infinities remain valid, and equal finite maxima would choose the lower index.
    const ScoreExtrema expected{ScoreCandidate{2, -inf}, ScoreCandidate{4, inf}};
    check_extrema(scan_score_extrema_scalar(scores, eligible), expected);

    const std::array tied_scores{7.0, 2.0, 7.0, 2.0, 7.0};
    const std::array all_eligible{1.0, 1.0, 1.0, 1.0, 1.0};
    const ScoreExtrema tied_expected{ScoreCandidate{1, 2.0}, ScoreCandidate{0, 7.0}};
    check_extrema(scan_score_extrema_scalar(tied_scores, all_eligible), tied_expected);

    const std::array none_eligible{0.0, 0.0, 0.0, 0.0, 0.0};
    check_extrema(scan_score_extrema_scalar(tied_scores, none_eligible), ScoreExtrema{});
}

TEST("numa head score table: slot ownership is exclusive and transferable") {
    const std::array regions{NumaScoreRegionConfig{0, 1, std::nullopt, {}}};
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;
    const HeadLoc loc = head(0, 0);

    CHECK(table.publish(loc, 7.5).has_value());
    CHECK(!table.publish(loc, 99.0).has_value());
    CHECK_EQ(*table.score(loc), 7.5);

    const auto extracted = table.extract(loc);
    CHECK(extracted.has_value());
    if (extracted) CHECK_EQ(*extracted, 7.5);
    CHECK(!table.score(loc).has_value());
    CHECK(!table.extract(loc).has_value());
    CHECK(!table.erase(loc).has_value());

    CHECK(table.publish(loc, 3.0).has_value());
    CHECK(table.erase(loc).has_value());
    CHECK(!table.score(loc).has_value());
    CHECK(!table.erase(loc).has_value());
}

TEST("numa head score table: dense slots map block and offset to the right score") {
    const std::array regions{NumaScoreRegionConfig{10, 2, std::nullopt, {}}};
    auto made = NumaHeadScoreTable::create(16 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;
    CHECK_EQ(table.worker_count(), std::size_t(1));

    for (unsigned block = 10; block < 12; ++block) {
        for (std::uint32_t offset = 0; offset < 16 * KiB; offset += 4 * KiB) {
            const double value = static_cast<double>(block * 100 + offset / (4 * KiB));
            CHECK(table.publish(head(block, offset), value).has_value());
        }
    }
    for (unsigned block = 10; block < 12; ++block) {
        for (std::uint32_t offset = 0; offset < 16 * KiB; offset += 4 * KiB) {
            const double expected = static_cast<double>(block * 100 + offset / (4 * KiB));
            const auto actual = table.score(head(block, offset));
            CHECK(actual.has_value());
            if (actual) CHECK_EQ(*actual, expected);
        }
    }
    CHECK(!table.score(head(12, 0)).has_value());
}

TEST("numa head score table: promotion ranks the sum of every head in a full block") {
    const std::array regions{
        NumaScoreRegionConfig{0, 1, std::nullopt, {}},
        NumaScoreRegionConfig{1, 2, std::nullopt, {}},
    };
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;

    publish(table, 0, 1.0, 1.0);
    publish(table, 1, 6.0, 6.0);  // sum 12
    publish(table, 2, 10.0, 0.0); // sum 10, despite owning the hottest individual head

    auto candidates = table.promotion_candidates({});
    CHECK(candidates.has_value());
    if (!candidates) return;
    check_candidate(candidates->cold_preferred, ScoreCandidate{0, 2.0});
    check_candidate(candidates->hot_foreign, ScoreCandidate{1, 12.0});
}

TEST("numa head score table: three logical regions reduce extrema with stable ties") {
    const std::array regions{
        NumaScoreRegionConfig{0, 2, std::nullopt, {}},
        NumaScoreRegionConfig{2, 2, std::nullopt, {}},
        NumaScoreRegionConfig{4, 2, std::nullopt, {}},
    };
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;
    CHECK_EQ(table.worker_count(), std::size_t(3));

    publish(table, 0, 3.0, 3.0); // preferred sum 6
    publish(table, 1, 1.0, 1.0); // preferred minimum, sum 2
    publish(table, 2, 9.0, 1.0); // foreign maximum, sum 10
    publish(table, 3, 4.0, 5.0); // sum 9
    publish(table, 4, 6.0, 4.0); // tied foreign maximum, sum 10
    publish(table, 5, 2.0, 2.0); // sum 4

    auto candidates = table.promotion_candidates({});
    CHECK(candidates.has_value());
    if (!candidates) return;
    check_candidate(candidates->cold_preferred, ScoreCandidate{1, 2.0});
    check_candidate(candidates->hot_foreign, ScoreCandidate{2, 10.0});
}

TEST("numa head score table: partial and pinned hot blocks are excluded") {
    const std::array regions{
        NumaScoreRegionConfig{0, 1, std::nullopt, {}},
        NumaScoreRegionConfig{1, 3, std::nullopt, {}},
    };
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;

    publish(table, 0, 1.0, 1.0);
    CHECK(table.publish(head(1, 0), 1000.0).has_value()); // incomplete block: never eligible
    publish(table, 2, 50.0, 50.0);                       // hottest complete block, but pinned
    publish(table, 3, 7.0, 7.0);                         // fallback complete block

    const std::array pinned{2u};
    auto candidates = table.promotion_candidates(pinned);
    CHECK(candidates.has_value());
    if (!candidates) return;
    check_candidate(candidates->cold_preferred, ScoreCandidate{0, 2.0});
    check_candidate(candidates->hot_foreign, ScoreCandidate{3, 14.0});

    candidates = table.promotion_candidates({});
    CHECK(candidates.has_value());
    if (candidates)
        check_candidate(candidates->hot_foreign, ScoreCandidate{2, 100.0});
}

TEST("numa head score table: swapping blocks moves complete score slices") {
    const std::array regions{
        NumaScoreRegionConfig{0, 1, std::nullopt, {}},
        NumaScoreRegionConfig{1, 1, std::nullopt, {}},
    };
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;

    publish(table, 0, 1.0, 2.0);
    publish(table, 1, 10.0, 20.0);
    CHECK(table.swap_blocks(0, 1).has_value());
    CHECK_EQ(*table.score(head(0, 0)), 10.0);
    CHECK_EQ(*table.score(head(0, 4 * KiB)), 20.0);
    CHECK_EQ(*table.score(head(1, 0)), 1.0);
    CHECK_EQ(*table.score(head(1, 4 * KiB)), 2.0);
}

TEST("numa head score table: increment and decay preserve dense slots") {
    const std::array regions{
        NumaScoreRegionConfig{0, 1, std::nullopt, {}},
        NumaScoreRegionConfig{1, 1, std::nullopt, {}},
    };
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;

    publish(table, 0, 4.0, 8.0);
    publish(table, 1, 12.0, std::numeric_limits<double>::infinity());
    CHECK(table.increment(head(0, 0), 2.0).has_value());
    CHECK(table.decay(0.25).has_value());
    CHECK_EQ(*table.score(head(0, 0)), 1.5);
    CHECK_EQ(*table.score(head(0, 4 * KiB)), 2.0);
    CHECK_EQ(*table.score(head(1, 0)), 3.0);
    CHECK(std::isinf(*table.score(head(1, 4 * KiB))));

    CHECK(table.erase(head(0, 4 * KiB)).has_value());
    CHECK(!table.score(head(0, 4 * KiB)).has_value());
}

TEST("numa head score table: concurrent increments produce an exact atomic total") {
    const std::array regions{NumaScoreRegionConfig{0, 1, std::nullopt, {}}};
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;
    const HeadLoc loc = head(0, 0);
    CHECK(table.publish(loc, 0.0).has_value());

    constexpr int kThreads = 8;
    constexpr int kIncrements = 10'000;
    std::atomic<bool> start{false};
    std::atomic<bool> operations_ok{true};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < kIncrements; ++i)
                if (!table.increment(loc, 1.0))
                    operations_ok.store(false, std::memory_order_relaxed);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    CHECK(operations_ok.load(std::memory_order_relaxed));
    const auto final_score = table.score(loc);
    CHECK(final_score.has_value());
    if (final_score) CHECK_EQ(*final_score, static_cast<double>(kThreads * kIncrements));
}

TEST("numa head score table: increments and node-local decay can race safely") {
    const std::array regions{NumaScoreRegionConfig{0, 1, std::nullopt, {}}};
    auto made = NumaHeadScoreTable::create(8 * KiB, 4 * KiB, regions);
    CHECK(made.has_value());
    if (!made) return;
    auto& table = *made;
    const HeadLoc loc = head(0, 0);
    CHECK(table.publish(loc, 0.0).has_value());

    constexpr int kReaders = 4;
    constexpr int kIncrements = 20'000;
    constexpr int kDecays = 2'000;
    constexpr double kAmount = 0.125;
    std::atomic<bool> start{false};
    std::atomic<bool> operations_ok{true};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < kIncrements; ++i)
                if (!table.increment(loc, kAmount))
                    operations_ok.store(false, std::memory_order_relaxed);
        });
    }
    std::thread decayer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < kDecays; ++i)
            if (!table.decay(0.999)) operations_ok.store(false, std::memory_order_relaxed);
    });

    start.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    decayer.join();

    CHECK(operations_ok.load(std::memory_order_relaxed));
    const auto final_score = table.score(loc);
    CHECK(final_score.has_value());
    if (final_score) {
        CHECK(std::isfinite(*final_score));
        CHECK(*final_score >= 0.0);
        CHECK(*final_score <= static_cast<double>(kReaders * kIncrements) * kAmount);
    }
}
