#include "mini_test.hpp"

#include "goblin/storage/drive_pool.hpp"

using namespace goblin;
using namespace goblin::storage;

TEST("drive_pool: a full row fans across all drives, one stripe unit each") {
    const DrivePool pool(/*N=*/4, /*S=*/64 * KiB);
    CHECK_EQ(pool.row_bytes(), Size(4 * 64 * KiB));

    const auto segs = pool.plan_reads(/*key_hash=*/0, /*off=*/0, /*len=*/pool.row_bytes());
    CHECK_EQ(segs.size(), std::size_t(4));
    bool seen[4] = {false, false, false, false};
    for (const auto& s : segs) {
        CHECK_EQ(s.length, Size(64 * KiB));
        CHECK_EQ(s.file_offset, Offset(0));
        seen[s.drive] = true;
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
}

TEST("drive_pool: key_hash rotates the starting drive; rows advance the file offset") {
    const DrivePool pool(4, 64 * KiB);
    CHECK_EQ(pool.drive_of(0, 0), 0u);            // (hash 0 + chunk 0) % 4
    CHECK_EQ(pool.drive_of(1, 0), 1u);            // hash rotates start
    CHECK_EQ(pool.drive_of(0, 64 * KiB), 1u);     // chunk 1 -> drive 1
    CHECK_EQ(pool.drive_of(0, 4 * 64 * KiB), 0u); // chunk 4 -> drive 0 (row 1)
    CHECK_EQ(pool.file_offset_of(4 * 64 * KiB), Size(64 * KiB)); // chunk 4 -> row 1 -> offset S
}

TEST("drive_pool: single drive splits a read into per-chunk segments") {
    const DrivePool pool(1, 64 * KiB);
    const auto segs = pool.plan_reads(0, 0, 200 * KiB); // chunks of 64,64,64,8 KiB
    CHECK_EQ(segs.size(), std::size_t(4));
    for (const auto& s : segs) CHECK_EQ(s.drive, 0u);
    CHECK_EQ(segs[0].file_offset, Offset(0));
    CHECK_EQ(segs[1].file_offset, Offset(64 * KiB));
    CHECK_EQ(segs[3].file_offset, Offset(192 * KiB));
    CHECK_EQ(segs[3].length, Size(8 * KiB));
}

TEST("drive_pool: unaligned sub-row read splits at the stripe boundary") {
    const DrivePool pool(4, 64 * KiB);
    // [32K, 96K) crosses chunk 0 (drive 0) into chunk 1 (drive 1).
    const auto segs = pool.plan_reads(0, 32 * KiB, 64 * KiB);
    CHECK_EQ(segs.size(), std::size_t(2));
    CHECK_EQ(segs[0].drive, 0u);
    CHECK_EQ(segs[0].file_offset, Offset(32 * KiB));
    CHECK_EQ(segs[0].length, Size(32 * KiB));
    CHECK_EQ(segs[1].drive, 1u);
    CHECK_EQ(segs[1].file_offset, Offset(0));
    CHECK_EQ(segs[1].length, Size(32 * KiB));
}

TEST("drive_pool: two rows -> per-chunk segments, each drive twice at successive file offsets") {
    const DrivePool pool(4, 64 * KiB);
    const auto segs = pool.plan_reads(0, 0, 2 * pool.row_bytes()); // 8 chunks
    CHECK_EQ(segs.size(), std::size_t(8));
    for (unsigned c = 0; c < 8; ++c) { // chunks 0..7 -> drives 0,1,2,3,0,1,2,3
        CHECK_EQ(segs[c].drive, c % 4u);
        CHECK_EQ(segs[c].file_offset, Offset((c / 4) * 64 * KiB));
        CHECK_EQ(segs[c].length, Size(64 * KiB));
    }
}

TEST("drive_pool: files_used caps at N") {
    const DrivePool pool(4, 64 * KiB);
    CHECK_EQ(pool.files_used(0), 0u);
    CHECK_EQ(pool.files_used(1), 1u);
    CHECK_EQ(pool.files_used(64 * KiB), 1u);
    CHECK_EQ(pool.files_used(3 * 64 * KiB), 3u);
    CHECK_EQ(pool.files_used(100 * 64 * KiB), 4u); // capped at N
}
