// goblin-store-path-prep <dir>... — bless directories as Goblin pool stores (ADR-0013).
// For each dir: mkdir -p, refuse if it is non-empty, then drop the .goblin-store-marker that
// the server requires before it will ever wipe the directory on startup.
#include "goblin/storage/pool_dir.hpp"

#include <cstdio>
#include <print>
#include <span>
#include <string>

int main(int argc, char** argv) {
    const std::span<char*> args(argv + 1, argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    if (args.empty()) {
        std::println(stderr, "usage: goblin-store-path-prep <dir>...");
        std::println(stderr,
                     "  Prepares each dir as a Goblin pool store: mkdir -p, require it be empty,");
        std::println(stderr, "  then write the {} marker.", goblin::storage::kMarkerName);
        return 2;
    }

    int rc = 0;
    for (char* a : args) {
        const std::string dir = a;
        if (auto ok = goblin::storage::prepare_pool_dir(dir); ok) {
            std::println("prepared {}", dir);
        } else {
            std::println(stderr, "FAILED {}: {}", dir, ok.error().detail);
            rc = 1;
        }
    }
    return rc;
}
