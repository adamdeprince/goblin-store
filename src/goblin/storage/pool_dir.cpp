#include "goblin/storage/pool_dir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace goblin::storage {

static fs::path marker_path(const std::string& dir) {
    return fs::path(dir) / std::string(kMarkerName);
}

Result<bool> has_marker(const std::string& dir) {
    std::error_code ec;
    const bool ok = fs::is_regular_file(marker_path(dir), ec); // false (no error) if absent
    if (ec) return err(Errc::io_error, ec.message());
    return ok;
}

Status prepare_pool_dir(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec); // mkdir -p; not an error if it already exists
    if (ec) return err(Errc::io_error, "mkdir -p " + dir + ": " + ec.message());
    if (!fs::is_directory(dir, ec))
        return err(Errc::invalid_argument, dir + " is not a directory");

    const bool empty = fs::is_empty(dir, ec);
    if (ec) return err(Errc::io_error, "stat " + dir + ": " + ec.message());
    if (!empty)
        return err(Errc::already_exists,
                   dir + " is not empty; refusing to mark it as a goblin store");

    std::ofstream m(marker_path(dir));
    m << "goblin-store-marker v1\n";
    if (!m) return err(Errc::io_error, "cannot write marker in " + dir);
    return {};
}

Status wipe_pool_dir(const std::string& dir) {
    auto marked = has_marker(dir);
    if (!marked) return std::unexpected(marked.error());
    if (!*marked)
        return err(Errc::invalid_argument,
                   dir + " has no " + std::string(kMarkerName) +
                       "; refusing to wipe (run goblin-store-path-prep first)");

    std::error_code ec;
    const fs::path keep = marker_path(dir);
    fs::directory_iterator it(dir, ec), end;
    if (ec) return err(Errc::io_error, "iterate " + dir + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) return err(Errc::io_error, "iterate " + dir + ": " + ec.message());
        if (it->path() == keep) continue; // keep the marker
        std::error_code rec;
        fs::remove_all(it->path(), rec);
        if (rec) return err(Errc::io_error, "remove " + it->path().string() + ": " + rec.message());
    }
    return {};
}

} // namespace goblin::storage
