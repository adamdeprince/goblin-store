#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/file_handle_cache.hpp"

#include <atomic>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace goblin;
using namespace goblin::storage;

namespace {

void make_file(int directory, const char* name, char value) {
    const int fd = ::openat(directory, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK_EQ(::write(fd, &value, 1), ssize_t{1});
    ::close(fd);
}

} // namespace

TEST("file-handle cache requires power-of-two capacity") {
    CHECK(!FileHandleCache::create(0).has_value());
    CHECK(!FileHandleCache::create(3).has_value());
    auto cache = FileHandleCache::create(128);
    CHECK(cache.has_value());
    if (cache) {
        CHECK_EQ((*cache)->stats().capacity, std::size_t{128});
        CHECK((*cache)->stats().shards > 1);
    }
}

TEST("file-handle CLOCK cache reuses, evicts, and safely invalidates leased descriptors") {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() /
        ("goblin-fd-cache-" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0);
    if (directory < 0) {
        fs::remove_all(path);
        return;
    }
    make_file(directory, "a", 'a');
    make_file(directory, "b", 'b');
    make_file(directory, "c", 'c');

    auto made = FileHandleCache::create(2);
    CHECK(made.has_value());
    if (!made) {
        ::close(directory);
        fs::remove_all(path);
        return;
    }
    auto cache = *made;
    const FileHandleCache::Key a{crypto::hash_key("a"), 1, directory};
    const FileHandleCache::Key b{crypto::hash_key("b"), 1, directory};
    const FileHandleCache::Key c{crypto::hash_key("c"), 1, directory};

    auto first = cache->open_read(a, "a", O_RDONLY);
    CHECK(first.has_value());
    const int first_fd = first ? (*first)->get() : -1;
    if (first) first->reset();
    auto hit = cache->open_read(a, "a", O_RDONLY);
    CHECK(hit.has_value());
    if (hit) CHECK_EQ((*hit)->get(), first_fd);
    if (hit) hit->reset();
    CHECK_EQ(cache->stats().hits, std::uint64_t{1});

    auto second = cache->open_read(b, "b", O_RDONLY);
    CHECK(second.has_value());
    if (second) second->reset();
    auto third = cache->open_read(c, "c", O_RDONLY);
    CHECK(third.has_value());
    if (third) third->reset();
    CHECK_EQ(cache->stats().cached, std::size_t{2});
    CHECK_EQ(cache->stats().evictions, std::uint64_t{1});

    auto lease = cache->open_read(c, "c", O_RDONLY);
    CHECK(lease.has_value());
    CHECK_EQ(cache->stats().in_use, std::size_t{1});
    cache->invalidate(c);
    CHECK_EQ(cache->stats().invalidations, std::uint64_t{1});
    CHECK_EQ(cache->stats().cached, std::size_t{1});
    if (lease) {
        char value = 0;
        CHECK_EQ(::pread((*lease)->get(), &value, 1, 0), ssize_t{1});
        CHECK_EQ(value, 'c');
    }

    if (lease) lease->reset();
    cache.reset();
    ::close(directory);
    fs::remove_all(path);
}

TEST("file-handle cache shards tolerate concurrent opens and preserve total capacity") {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() /
        ("goblin-fd-cache-concurrent-" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0);
    if (directory < 0) {
        fs::remove_all(path);
        return;
    }

    constexpr std::size_t file_count = 64;
    std::vector<std::string> names;
    std::vector<FileHandleCache::Key> keys;
    names.reserve(file_count);
    keys.reserve(file_count);
    for (std::size_t i = 0; i < file_count; ++i) {
        names.push_back("object-" + std::to_string(i));
        make_file(directory, names.back().c_str(), static_cast<char>(i));
        keys.push_back({crypto::hash_key(names.back()), 1, directory});
    }

    auto made = FileHandleCache::create(32);
    CHECK(made.has_value());
    if (!made) {
        ::close(directory);
        fs::remove_all(path);
        return;
    }
    auto cache = *made;
    CHECK(cache->stats().shards > 1);

    std::atomic<bool> okay{true};
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            for (std::size_t request = 0; request < 512; ++request) {
                const std::size_t i = (request * 17 + worker * 7) % file_count;
                auto opened = cache->open_read(keys[i], names[i].c_str(), O_RDONLY);
                if (!opened) {
                    okay.store(false, std::memory_order_relaxed);
                    continue;
                }
                char value = 0;
                if (::pread((*opened)->get(), &value, 1, 0) != 1 ||
                    value != static_cast<char>(i))
                    okay.store(false, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    CHECK(okay.load(std::memory_order_relaxed));
    const auto populated = cache->stats();
    CHECK(populated.cached <= populated.capacity);
    CHECK(populated.hits > 0);
    CHECK(populated.misses > 0);

    for (const auto& key : keys) cache->invalidate(key);
    CHECK_EQ(cache->stats().cached, std::size_t{0});
    for (std::size_t i = 0; i < 32; ++i) {
        auto opened = cache->open_read(keys[i], names[i].c_str(), O_RDONLY);
        CHECK(opened.has_value());
    }
    CHECK_EQ(cache->stats().cached, std::size_t{32});

    cache.reset();
    ::close(directory);
    fs::remove_all(path);
}
