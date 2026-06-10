#include "mini_test.hpp"

#include "goblin/storage/pool_dir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::storage;

static std::string scratch_base() {
    return (fs::temp_directory_path() / ("goblin-selftest-" + std::to_string(::getpid()))).string();
}

TEST("pool_dir: prepare blesses an empty dir; wipe clears files but keeps the marker") {
    const std::string base = scratch_base();
    fs::remove_all(base);
    const std::string dir = base + "/ssd0";

    CHECK(prepare_pool_dir(dir).has_value());
    CHECK(has_marker(dir).value_or(false));

    { std::ofstream(dir + "/obj-a"); }
    { std::ofstream(dir + "/obj-b"); }
    CHECK(fs::exists(dir + "/obj-a"));

    CHECK(wipe_pool_dir(dir).has_value());
    CHECK(!fs::exists(dir + "/obj-a"));
    CHECK(!fs::exists(dir + "/obj-b"));
    CHECK(has_marker(dir).value_or(false)); // marker survived the wipe

    fs::remove_all(base);
}

TEST("pool_dir: prepare refuses a non-empty directory") {
    const std::string base = scratch_base();
    fs::remove_all(base);
    const std::string dir = base + "/ssd1";
    fs::create_directories(dir);
    { std::ofstream(dir + "/preexisting"); }

    CHECK(!prepare_pool_dir(dir).has_value());

    fs::remove_all(base);
}

TEST("pool_dir: wipe aborts when the marker is absent (never erase an unblessed dir)") {
    const std::string base = scratch_base();
    fs::remove_all(base);
    const std::string dir = base + "/ssd2";
    fs::create_directories(dir);
    { std::ofstream(dir + "/important"); }

    CHECK(!wipe_pool_dir(dir).has_value());
    CHECK(fs::exists(dir + "/important")); // untouched

    fs::remove_all(base);
}
