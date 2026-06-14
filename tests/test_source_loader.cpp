#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/key_derivation.hpp"
#include "goblin/http/source_loader.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::core;
using goblin::crypto::hash_key;
using goblin::http::KeyOptions;
using goblin::storage::Index;
using goblin::storage::TierManager;

static std::string pattern(int seed, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t j = 0; j < n; ++j)
        s[j] = static_cast<char>((j * 7 + static_cast<std::size_t>(seed) * 131 + 3) & 0xFF);
    return s;
}

static void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static Result<TierManager> open_tm(const std::string& base, Index& index) {
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs = {base + "/s0", base + "/s1"};
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    return TierManager::open(tiers, mem, ev, ssd, hdd, index);
}

static std::string tmp_base(const char* tag) {
    return (fs::temp_directory_path() / ("goblin-src-" + std::string(tag) + std::to_string(::getpid())))
        .string();
}

TEST("source loader: preloads a tree; each file is stored under its derived key with right content") {
    const std::string base = tmp_base("tree");
    const fs::path root = fs::path(base) / "tree";
    Index index;
    auto tm = open_tm(base, index);
    CHECK(tm.has_value());
    if (!tm) { fs::remove_all(base); return; }

    // head-resident, disk-tail, empty, and nested files.
    const std::vector<std::pair<std::string, std::string>> files = {
        {"index.html", "<h1>hi</h1>"},
        {"big.bin", pattern(5, 40 * 1024)},
        {"empty", ""},
        {"a/b/deep.txt", "nested"},
    };
    for (const auto& [rel, content] : files) write_file(root / rel, content);

    KeyOptions opt; // path mode (default)
    const std::size_t n = http::preload_sources({root.string()}, opt, *tm);
    CHECK_EQ(n, files.size());

    // every file is indexed under its relpath-derived key with the right size
    bool sizes_ok = true;
    for (const auto& [rel, content] : files) {
        const auto meta = index.lookup(hash_key(http::derive_key_from_relpath(rel, opt)));
        sizes_ok = sizes_ok && meta && meta->size == content.size();
    }
    CHECK(sizes_ok);

    // content round-trips (needs io_uring for the read path)
    if (Reactor::available()) {
        auto r = Reactor::create();
        if (r) {
            bool content_ok = true;
            for (const auto& [rel, content] : files) {
                if (content.empty()) continue;
                std::string out(content.size(), '\0');
                const auto rd = tm->read(*r, hash_key(http::derive_key_from_relpath(rel, opt)), 0,
                                         MutBytes(reinterpret_cast<std::byte*>(out.data()), out.size()));
                content_ok = content_ok && rd && *rd == content.size() && out == content;
            }
            CHECK(content_ok);
        }
    }
    fs::remove_all(base);
}
