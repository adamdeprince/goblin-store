// Goblin Store entry point.
// Parse args, validate config (ADR invariants), wipe the pool dirs to a blank slate (ADR-0013),
// build the index + tier manager + io_uring reactor, and run the memcache/TCP server.
// (Single-threaded blocking server for now; io_uring-network + thread-per-core land later.)
#include "goblin/common/config.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/protocol/memcache/server.hpp"
#include "goblin/storage/pool_dir.hpp"

#include <charconv>
#include <cstdio>
#include <optional>
#include <print>
#include <span>
#include <string_view>

namespace {
using namespace goblin;

std::optional<Size> parse_size(std::string_view s) {
    if (s.empty()) return std::nullopt;
    Size mult = 1;
    switch (s.back()) {
        case 'k': case 'K': mult = KiB; s.remove_suffix(1); break;
        case 'm': case 'M': mult = MiB; s.remove_suffix(1); break;
        case 'g': case 'G': mult = GiB; s.remove_suffix(1); break;
        default: break;
    }
    Size v = 0;
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return v * mult;
}

template <class T>
std::optional<T> parse_int(std::string_view s) {
    T v{};
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return v;
}

void print_help() {
    std::println(
        "goblincache — large-object tiered cache / HTTP object server (scaffold)\n"
        "usage: goblincache [options]\n"
        "  --memory SIZE       RAM budget, mlock'd (e.g. 4G, 512M)   [default 1G]\n"
        "  --block SIZE        RAM block size, power of two          [default 1M]\n"
        "  --ssd-dir DIR       SSD pool directory (repeatable, >=1 required)\n"
        "  --hdd-dir DIR       HDD cold-pool directory (repeatable; enables 3-layer)\n"
        "  --ram-head SIZE     per-object RAM head                   [default 256K]\n"
        "  --ssd-prefix SIZE   per-object SSD prefix                 [default 32M]\n"
        "  --io-chunk SIZE     streaming I/O chunk size              [default 256K]\n"
        "  --io-buffers N      streaming I/O buffers per worker      [default 64]\n"
        "  --io-timeout MS     drop a stalled transfer (slow client) [default 30000, 0=off]\n"
        "  --memcache-port N   memcache/TCP port                     [default 11211]\n"
        "  --http-port N       HTTP port                             [default 8080]\n"
        "  --cores N           worker cores, 0=all                   [default 0]\n"
        "  --source DIR        preload a directory tree at startup (repeatable)\n"
        "  --http-vhost        HTTP key = Host + URI (default: key = URI path)\n"
        "  --key-on-query      include the query string in the key (default: strip)\n"
        "  --no-http           disable the plaintext HTTP listener\n"
        "  --no-memcache       disable the memcache/TCP listener\n"
        "  --no-mlock          don't mlock the RAM pool (dev; raise RLIMIT_MEMLOCK in prod)\n"
        "  --eviction NAME     head-cache eviction policy: s3fifo (only one implemented yet)\n"
        "  --max-objects N     cap on stored objects (0 = unbounded); evicts whole objects over it\n"
        "  --net MODE          network: async (default, io_uring loop) | blocking\n"
        "  --tls-cert FILE     PEM certificate; enables HTTPS (with --tls-key)\n"
        "  --tls-key FILE      PEM private key for HTTPS\n"
        "  --https-port N      TLS listener port                     [default 8443]\n"
        "  --help");
}
} // namespace

int main(int argc, char** argv) {
    ServerConfig cfg;
    std::span<char*> args(argv + 1, argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        auto take = [&](std::string_view name) -> std::optional<std::string_view> {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} needs an argument", name);
                return std::nullopt;
            }
            return std::string_view(args[++i]);
        };
        auto bad = [&](std::string_view what, std::string_view v) {
            std::println(stderr, "error: bad {} '{}'", what, v);
        };

        if (a == "--help" || a == "-h") { print_help(); return 0; }
        else if (a == "--ssd-dir") { auto v = take(a); if (!v) return 2; cfg.ssd.dirs.emplace_back(*v); }
        else if (a == "--hdd-dir") { auto v = take(a); if (!v) return 2; cfg.hdd.dirs.emplace_back(*v); }
        else if (a == "--source")  { auto v = take(a); if (!v) return 2; cfg.sources.emplace_back(*v); }
        else if (a == "--http-vhost")   { cfg.http_vhost = true; }
        else if (a == "--key-on-query") { cfg.key_on_query = true; }
        else if (a == "--no-http")     { cfg.enable_http = false; }
        else if (a == "--no-memcache") { cfg.enable_memcache = false; }
        else if (a == "--no-mlock")    { cfg.memory.lock_memory = false; }
        else if (a == "--eviction") {
            auto v = take(a); if (!v) return 2;
            if (*v == "s3fifo") cfg.eviction.policy = EvictionPolicyKind::s3fifo;
            else { std::println(stderr, "error: eviction '{}' not implemented yet (only s3fifo)", *v); return 2; }
        }
        else if (a == "--max-objects") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<std::uint64_t>(*v); if (!n) { bad("max-objects", *v); return 2; }
            cfg.eviction.max_ssd_objects = *n;
        }
        else if (a == "--tls-cert")  { auto v = take(a); if (!v) return 2; cfg.tls_cert_path = std::string(*v); }
        else if (a == "--tls-key")   { auto v = take(a); if (!v) return 2; cfg.tls_key_path = std::string(*v); }
        else if (a == "--https-port") { auto v = take(a); if (!v) return 2; auto p = parse_int<std::uint16_t>(*v); if (!p) { bad("port", *v); return 2; } cfg.https_port = *p; }
        else if (a == "--memory" || a == "--block" || a == "--ram-head" || a == "--ssd-prefix" || a == "--io-chunk") {
            auto v = take(a); if (!v) return 2;
            auto s = parse_size(*v); if (!s) { bad("size", *v); return 2; }
            if (a == "--memory")          cfg.memory.total_bytes = *s;
            else if (a == "--block")      cfg.memory.block_bytes = *s;
            else if (a == "--ram-head")   cfg.tiers.ram_head = *s;
            else if (a == "--ssd-prefix") cfg.tiers.ssd_prefix = *s;
            else                          cfg.io_chunk_bytes = *s;
        }
        else if (a == "--memcache-port" || a == "--http-port") {
            auto v = take(a); if (!v) return 2;
            auto p = parse_int<std::uint16_t>(*v); if (!p) { bad("port", *v); return 2; }
            if (a == "--memcache-port") cfg.memcache_port = *p; else cfg.http_port = *p;
        }
        else if (a == "--cores") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad("cores", *v); return 2; }
            cfg.cores = *n;
        }
        else if (a == "--net") {
            auto v = take(a); if (!v) return 2;
            if (*v == "blocking") cfg.net = NetMode::blocking;
            else if (*v == "async") cfg.net = NetMode::async;
            else { bad("net mode", *v); return 2; }
        }
        else if (a == "--io-buffers") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n || *n == 0) { bad("io-buffers", *v); return 2; }
            cfg.io_buffers = *n;
        }
        else if (a == "--io-timeout") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad("io-timeout", *v); return 2; }
            cfg.io_timeout_ms = *n;
        }
        else { std::println(stderr, "error: unknown option '{}' (try --help)", a); return 2; }
    }

    cfg.enable_https = !cfg.tls_cert_path.empty() && !cfg.tls_key_path.empty();

    if (auto ok = validate(cfg); !ok) {
        std::println(stderr, "config error: {}", ok.error().detail);
        return 1;
    }

    std::println("┌─ goblincache 0.0.1 ─────────────────────────");
    std::println("│ mode        : {}", cfg.three_layer() ? "3-layer (RAM+SSD+HDD)" : "2-layer (RAM+SSD)");
    std::println("│ RAM budget  : {} MiB (block {} KiB, mlock={})",
                 cfg.memory.total_bytes / MiB, cfg.memory.block_bytes / KiB, cfg.memory.lock_memory);
    std::println("│ ram_head    : {} KiB / object", cfg.tiers.ram_head / KiB);
    std::println("│ ssd_prefix  : {} MiB / object", cfg.tiers.ssd_prefix / MiB);
    std::println("│ ssd pool    : {} drive(s), stripe {} KiB", cfg.ssd.dirs.size(), cfg.ssd.stripe_unit / KiB);
    std::println("│ hdd pool    : {} drive(s), stripe {} KiB", cfg.hdd.dirs.size(), cfg.hdd.stripe_unit / KiB);
    std::println("│ memcache    : {}", cfg.enable_memcache ? ":" + std::to_string(cfg.memcache_port)
                                                            : std::string("off"));
    std::println("│ http        : {}", cfg.enable_http ? ":" + std::to_string(cfg.http_port)
                                                        : std::string("off"));
    std::println("│ https       : {}", cfg.enable_https ? ":" + std::to_string(cfg.https_port)
                                                         : std::string("off"));
    std::println("│ io backend  : {}", GOBLIN_HAVE_URING ? "io_uring (available)" : "stub (no liburing)");
    std::println("│ net         : {}, {} workers",
                 cfg.net == NetMode::async ? "async (io_uring loop)" : "blocking (thread-per-core)",
                 cfg.cores ? std::to_string(cfg.cores) : std::string("auto"));
    std::println("│ io bufs     : {} x {} KiB / worker, stall timeout {}", cfg.io_buffers,
                 cfg.io_chunk_bytes / KiB,
                 cfg.io_timeout_ms ? std::to_string(cfg.io_timeout_ms) + "ms" : std::string("off"));
    std::println("└─────────────────────────────────────────────");
    // Blank-slate the pool dirs (ADR-0013): wipe requires the .goblin-store-marker
    // (run goblin-store-path-prep first), else we refuse to start.
    for (const auto& dir : cfg.ssd.dirs) {
        if (auto w = storage::wipe_pool_dir(dir); !w) {
            std::println(stderr, "startup: {}", w.error().detail);
            return 1;
        }
    }
    for (const auto& dir : cfg.hdd.dirs) {
        if (auto w = storage::wipe_pool_dir(dir); !w) {
            std::println(stderr, "startup: {}", w.error().detail);
            return 1;
        }
    }

    storage::Index index;
    auto tm = storage::TierManager::open(cfg.tiers, cfg.memory, cfg.eviction, cfg.ssd, cfg.hdd, index,
                                         cfg.io_chunk_bytes, cfg.io_buffers,
                                         cfg.cache_bypass == CacheBypass::o_direct);
    if (!tm) {
        std::println(stderr, "startup: {}", tm.error().detail);
        return 1;
    }
    if (!core::Reactor::available()) {
        std::println(stderr, "startup: io_uring unavailable (need liburing + io_uring_setup allowed)");
        return 1;
    }
    // Each worker creates its own reactor + I/O-buffer pool (thread-per-core, ADR-0018).

    if (cfg.enable_https)
        std::println("note: HTTPS (TLS) not wired yet — serving plaintext listeners only for now");
    if (!cfg.enable_memcache && !cfg.enable_http) {
        std::println("no plaintext listener enabled (HTTPS not wired yet) — nothing to serve");
        return 0;
    }
    std::string listening = "listening:";
    if (cfg.enable_memcache) listening += " memcache/tcp :" + std::to_string(cfg.memcache_port);
    if (cfg.enable_http) listening += " http/tcp :" + std::to_string(cfg.http_port);
    std::println("{}  (Ctrl-C to stop)", listening);
    if (auto st = memcache::serve(cfg, *tm, index); !st) {
        std::println(stderr, "serve: {}", st.error().detail);
        return 1;
    }
    return 0;
}
