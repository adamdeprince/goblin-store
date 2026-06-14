#include "goblin/http/source_loader.hpp"

#include "goblin/crypto/sha256.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <print>
#include <span>
#include <unistd.h>

namespace goblin::http {
namespace fs = std::filesystem;
namespace {

// Stream one file into the store through the chunked write path (bounded RAM = one `buf`).
Status load_file(storage::TierManager& tm, const crypto::Digest& digest, const fs::path& path,
                 Size size, std::span<std::byte> buf) {
    auto h = tm.begin_store(digest, size); // startup is single-threaded, so the staging pool is free
    if (!h) return std::unexpected(h.error());
    if (size > 0) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return err(Errc::io_error, "open");
        Size left = size;
        while (left > 0) {
            const ssize_t r = ::read(fd, buf.data(), std::min<Size>(left, buf.size()));
            if (r <= 0) { ::close(fd); return err(Errc::io_error, "read"); }
            if (auto st = h->write(ByteView(buf.data(), static_cast<std::size_t>(r))); !st) {
                ::close(fd);
                return st;
            }
            left -= static_cast<Size>(r);
        }
        ::close(fd);
    }
    return h->commit(0);
}

} // namespace

std::size_t preload_sources(const std::vector<std::string>& dirs, const KeyOptions& opt,
                            storage::TierManager& tm) {
    std::vector<std::byte> buf(256 * 1024); // shared read buffer, reused across files
    std::size_t loaded = 0;
    for (const auto& dir : dirs) {
        std::error_code ec;
        const fs::path root(dir);
        if (!fs::is_directory(root, ec)) {
            std::println(stderr, "source: {} is not a directory — skipped", dir);
            continue;
        }
        auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
            const fs::path& p = it->path();
            const Size size = fs::file_size(p, ec);
            if (ec) { ec.clear(); continue; }
            const std::string rel = fs::relative(p, root, ec).generic_string();
            if (ec || rel.empty()) { ec.clear(); continue; }
            const std::string key = derive_key_from_relpath(rel, opt);
            if (auto st = load_file(tm, crypto::hash_key(key), p, size, buf); !st) {
                std::println(stderr, "source: {} — {}", p.string(), st.error().detail);
                continue;
            }
            ++loaded;
        }
        if (ec) std::println(stderr, "source: walking {} — {}", dir, ec.message());
    }
    return loaded;
}

} // namespace goblin::http
