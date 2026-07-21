// Goblin Store entry point.
// Parse args, validate config (ADR invariants), wipe the pool dirs to a blank slate (ADR-0013),
// build the index + tier manager + io_uring reactor, and run the memcache/TCP server.
// (Single-threaded blocking server for now; io_uring-network + thread-per-core land later.)
#include "goblin/common/config.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/http/key_derivation.hpp"
#include "goblin/http/source_loader.hpp"
#include "goblin/net/numa.hpp"
#include "goblin/net/listener.hpp"
#include "goblin/net/rdma_server.hpp"
#include "goblin/protocol/memcache/server.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/storage/pool_dir.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace goblin;

std::atomic<bool> g_shutdown{false};
void on_term(int) { g_shutdown.store(true, std::memory_order_relaxed); } // async-signal-safe (atomic store)

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
    if (v > std::numeric_limits<Size>::max() / mult) return std::nullopt;
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

std::optional<double> parse_float(std::string_view s) {
    double v = 0.0;
    const char* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, v, std::chars_format::general);
    if (ec != std::errc{} || p != end || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<std::uint32_t> parse_octal_mode(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::uint32_t value = 0;
    const char* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, value, 8);
    if (ec != std::errc{} || p != end || value > 0777) return std::nullopt;
    return value;
}

std::string format_size(Size bytes) {
    if (bytes % GiB == 0) return std::to_string(bytes / GiB) + " GiB";
    if (bytes % MiB == 0) return std::to_string(bytes / MiB) + " MiB";
    if (bytes % KiB == 0) return std::to_string(bytes / KiB) + " KiB";
    return std::to_string(bytes) + " B";
}

void print_help() {
    std::println(
        "goblin-store — large-object tiered cache / HTTP object server (scaffold)\n"
        "usage: goblin-store [options]\n"
        "  --memory SIZE       local NUMA fixed-head RAM, mlock'd    [default 1G]\n"
        "  --sub-memory SIZE   fixed-head RAM on each remote node    [requires --numa]\n"
        "  --small-memory SIZE dedicated local small-object RAM      [omitted: legacy shared pool]\n"
        "  --small-sub-memory SIZE  small-object RAM on each remote node [requires --small-memory, --numa]\n"
        "  --block SIZE        allocation/promotion block, power of two HugeTLB multiple [default {}]\n"
        "  --small-min-alloc SIZE  RAM min-order for <ram-head heads  [default 16]\n"
        "  --ssd-dir DIR       SSD pool directory (repeatable, >=1 required)\n"
        "  --hdd-dir DIR       HDD cold-pool directory (repeatable; enables 3-layer)\n"
        "  --ram-head SIZE     packed per-object RAM head, power of two [default 256K]\n"
        "  --ssd-prefix SIZE   per-object SSD prefix                 [default 32M]\n"
        "  --io-chunk SIZE     cache-hit/read streaming chunk size   [default 256K]\n"
        "  --write-io-chunk SIZE  write-staging chunk size           [default 256K; mirror 1M]\n"
        "  --io-buffers N      streaming I/O buffers per worker      [default 64]\n"
        "  --io-timeout MS     drop a stalled transfer (slow client) [default 30000, 0=off]\n"
        "  --idle-timeout MS   expire idle TCP connections            [default 300000, 0=off]\n"
        "  --queue-timeout MS  max GET/SET buffer wait                 [default 30000, 0=off]\n"
        "  --max-get-waiters N max queued GETs per worker              [default 64]\n"
        "  --max-set-waiters N max queued SETs per worker              [default 64]\n"
        "  --max-connections N aggregate TCP/Unix connection limit     [default 1024]\n"
        "  --listen-backlog N  TCP listen backlog                      [default 1024]\n"
        "  --max-object-size SIZE  reject larger objects               [default 4G]\n"
        "  --listen-address A  numeric IPv4/IPv6 TCP bind address    [default 127.0.0.1]\n"
        "  --memcache-port N   memcache/TCP port                     [default 11211]\n"
        "  --memcache-tls      serve TLS 1.3 instead of plaintext on memcache-port\n"
        "  --auth-file FILE    memcached ASCII auth file (user:password; mode 0600)\n"
        "  --memcache-socket PATH  also serve memcache on an AF_UNIX socket\n"
        "  --memcache-socket-mode OCTAL  Unix socket permissions     [default 0600]\n"
        "  --rdma ADDRESS      native RDMA memcache bind address      [disabled]\n"
        "  --rdma-port N       native RDMA memcache port              [default 11211]\n"
        "  --rdma-ring SIZE    control-ring budget per connection     [default 64K]\n"
        "  --rdma-window SIZE  registered bulk window size            [default 256K]\n"
        "  --rdma-windows N    bulk windows per direction             [default 4]\n"
        "  --http-port N       HTTP port                             [default 8080]\n"
        "  --cores N           workers/protocol, 0=node CPU count    [default 0]\n"
        "  --numa NODE         bind all threads to this NUMA node    [default listener NIC]\n"
        "  --no-numa           disable NUMA affinity, placement, and promotion\n"
        "  --perverse          put preferred head RAM on farthest node [benchmark only]\n"
        "  --no-numa-promotion keep NUMA placement but disable hot-block swaps\n"
        "  --increment FLOAT   score added per successful key read   [default 1.0]\n"
        "  --decay FLOAT       per-minute score multiplier, 0 < d < 1 [default 0.5]\n"
        "  --source DIR        preload a directory tree at startup (repeatable)\n"
        "  --mirror URL        cache misses from an HTTP(S) origin/base path\n"
        "  --mirror-client M   upstream client: curl|uring            [default curl]\n"
        "  --http-vhost        HTTP key = Host + URI (default: key = URI path)\n"
        "  --virtual-host      alias for --http-vhost\n"
        "  --key-on-query      include the query string in the key (default: strip)\n"
        "  --key-strip-slash   drop the leading '/' from path-mode keys (memcache 'set foo' == GET /foo)\n"
        "  --http-index NAME   index file for HTTP paths ending in '/'  [default index.html]\n"
        "  --no-http-index     disable HTTP directory-index mapping\n"
        "  --no-http           disable the plaintext HTTP listener\n"
        "  --no-memcache       disable the memcache/TCP listener\n"
        "  --no-mlock          don't mlock the RAM pool (dev; raise RLIMIT_MEMLOCK in prod)\n"
        "  --no-read-ahead     disable double-buffered GET read-ahead (serial; for A/B benchmarking)\n"
        "  --eviction NAME     head-cache eviction policy: s3fifo (only one implemented yet)\n"
        "  --max-objects N     cap disk-backed objects (0 = unbounded); evicts whole objects over it\n"
        "  --disk-high-watermark F  start proactive filesystem reclaim [default 0.90]\n"
        "  --disk-low-watermark F   reclaim filesystem down to this use [default 0.80]\n"
        "  --disk-reclaim-interval MS  storage-health/reclaim cadence  [default 1000, 0=off]\n"
        "  --net MODE          network: async (default) | blocking | exasock\n"
        "  --tls-cert FILE     PEM cert chain; enables HTTPS. Repeat per domain — SNI selects\n"
        "  --tls-key FILE      PEM private key, paired with the preceding --tls-cert\n"
        "  --https-port N      TLS listener port                     [default 8443]\n"
        "  --https             enable HTTPS when certs are shared with memcache TLS\n"
        "  --no-https          do not infer HTTPS from TLS cert/key options\n"
        "  --help", format_size(kDefaultMemoryBlock));
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN); // a peer that closes mid-write must not kill us (TLS BIO writes)
    std::signal(SIGTERM, on_term); // graceful shutdown: drain in-flight transfers, then exit
    std::signal(SIGINT, on_term);
    ServerConfig cfg;
    bool write_io_chunk_explicit = false;
    bool https_disabled = false;
    bool https_requested = false;
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
        else if (a == "--mirror") {
            auto v = take(a); if (!v) return 2;
            cfg.mirror_url = std::string(*v);
        }
        else if (a == "--listen-address") {
            auto v = take(a); if (!v) return 2;
            cfg.listen_address = std::string(*v);
        }
        else if (a == "--auth-file") {
            auto v = take(a); if (!v) return 2;
            cfg.memcache_auth_file = std::string(*v);
        }
        else if (a == "--memcache-socket") {
            auto v = take(a); if (!v) return 2;
            cfg.memcache_socket = std::string(*v);
        }
        else if (a == "--memcache-socket-mode") {
            auto v = take(a); if (!v) return 2;
            auto mode = parse_octal_mode(*v);
            if (!mode) { bad("memcache socket mode", *v); return 2; }
            cfg.memcache_socket_mode = *mode;
        }
        else if (a == "--memcache-tls") { cfg.memcache_tls = true; }
        else if (a == "--rdma") {
            auto v = take(a); if (!v) return 2;
            cfg.rdma.enabled = true;
            cfg.rdma.address = std::string(*v);
        }
        else if (a == "--http-vhost" || a == "--virtual-host") { cfg.http_vhost = true; }
        else if (a == "--key-on-query")   { cfg.key_on_query = true; }
        else if (a == "--key-strip-slash") { cfg.key_strip_slash = true; }
        else if (a == "--http-index")     { auto v = take(a); if (!v) return 2; cfg.http_index = std::string(*v); }
        else if (a == "--no-http-index")  { cfg.http_index.clear(); }
        else if (a == "--no-http")     { cfg.enable_http = false; }
        else if (a == "--https")       { https_requested = true; }
        else if (a == "--no-https")    { https_disabled = true; }
        else if (a == "--no-memcache") { cfg.enable_memcache = false; }
        else if (a == "--no-numa")     { cfg.numa_enabled = false; }
        else if (a == "--perverse")    { cfg.numa_perverse = true; }
        else if (a == "--no-numa-promotion") { cfg.numa_promotion = false; }
        else if (a == "--no-read-ahead") { cfg.read_ahead = false; }
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
        else if (a == "--tls-cert")  { auto v = take(a); if (!v) return 2; cfg.tls_cert_paths.emplace_back(*v); }
        else if (a == "--tls-key")   { auto v = take(a); if (!v) return 2; cfg.tls_key_paths.emplace_back(*v); }
        else if (a == "--https-port") { auto v = take(a); if (!v) return 2; auto p = parse_int<std::uint16_t>(*v); if (!p) { bad("port", *v); return 2; } cfg.https_port = *p; }
        else if (a == "--memory" || a == "--sub-memory" || a == "--small-memory" ||
                 a == "--small-sub-memory" || a == "--block" || a == "--ram-head" ||
                 a == "--ssd-prefix" || a == "--io-chunk" || a == "--write-io-chunk" ||
                 a == "--small-min-alloc" || a == "--max-object-size" ||
                 a == "--rdma-ring" || a == "--rdma-window") {
            auto v = take(a); if (!v) return 2;
            auto s = parse_size(*v); if (!s) { bad("size", *v); return 2; }
            if (a == "--memory")               cfg.memory.total_bytes = *s;
            else if (a == "--sub-memory") {
                if (*s == 0) { bad("sub-memory", *v); return 2; }
                cfg.memory.sub_bytes = *s;
            }
            else if (a == "--small-memory") {
                if (*s == 0) { bad("small-memory", *v); return 2; }
                cfg.memory.small_total_bytes = *s;
            }
            else if (a == "--small-sub-memory") {
                if (*s == 0) { bad("small-sub-memory", *v); return 2; }
                cfg.memory.small_sub_bytes = *s;
            }
            else if (a == "--block")           cfg.memory.block_bytes = *s;
            else if (a == "--ram-head")        cfg.tiers.ram_head = *s;
            else if (a == "--ssd-prefix")      cfg.tiers.ssd_prefix = *s;
            else if (a == "--small-min-alloc") cfg.memory.small_min_alloc = *s;
            else if (a == "--max-object-size") cfg.max_object_size = *s;
            else if (a == "--rdma-ring")       cfg.rdma.ring_bytes = *s;
            else if (a == "--rdma-window")     cfg.rdma.bulk_window_bytes = *s;
            else if (a == "--io-chunk") cfg.io_chunk_bytes = *s;
            else {
                cfg.write_io_chunk_bytes = *s;
                write_io_chunk_explicit = true;
            }
        }
        else if (a == "--memcache-port" || a == "--http-port" || a == "--rdma-port") {
            auto v = take(a); if (!v) return 2;
            auto p = parse_int<std::uint16_t>(*v); if (!p) { bad("port", *v); return 2; }
            if (a == "--memcache-port") cfg.memcache_port = *p;
            else if (a == "--rdma-port") cfg.rdma.port = *p;
            else cfg.http_port = *p;
        }
        else if (a == "--cores") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad("cores", *v); return 2; }
            cfg.cores = *n;
        }
        else if (a == "--numa") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad("NUMA node", *v); return 2; }
            cfg.numa_node = *n;
        }
        else if (a == "--increment" || a == "--decay" ||
                 a == "--disk-high-watermark" || a == "--disk-low-watermark") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_float(*v); if (!n) { bad(a.substr(2), *v); return 2; }
            if (a == "--increment") cfg.access_score.increment = *n;
            else if (a == "--decay") cfg.access_score.decay = *n;
            else if (a == "--disk-high-watermark") cfg.eviction.high_watermark = *n;
            else cfg.eviction.low_watermark = *n;
        }
        else if (a == "--net") {
            auto v = take(a); if (!v) return 2;
            if (*v == "blocking") cfg.net = NetMode::blocking;
            else if (*v == "async") cfg.net = NetMode::async;
            else if (*v == "exasock") cfg.net = NetMode::exasock;
            else { bad("net mode", *v); return 2; }
        }
        else if (a == "--mirror-client") {
            auto v = take(a); if (!v) return 2;
            if (*v == "curl") cfg.mirror_client = MirrorClient::curl;
            else if (*v == "uring") cfg.mirror_client = MirrorClient::uring;
            else { bad("mirror client", *v); return 2; }
        }
        else if (a == "--io-buffers") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n || *n == 0) { bad("io-buffers", *v); return 2; }
            cfg.io_buffers = *n;
        }
        else if (a == "--rdma-windows") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n || *n == 0) { bad("rdma-windows", *v); return 2; }
            cfg.rdma.bulk_window_count = *n;
        }
        else if (a == "--io-timeout" || a == "--idle-timeout" ||
                 a == "--queue-timeout" || a == "--disk-reclaim-interval") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad(a.substr(2), *v); return 2; }
            if (a == "--io-timeout") cfg.io_timeout_ms = *n;
            else if (a == "--idle-timeout") cfg.idle_timeout_ms = *n;
            else if (a == "--queue-timeout") cfg.queue_timeout_ms = *n;
            else cfg.eviction.reclaim_interval_ms = *n;
        }
        else if (a == "--max-get-waiters" || a == "--max-set-waiters" ||
                 a == "--max-connections" || a == "--listen-backlog") {
            auto v = take(a); if (!v) return 2;
            auto n = parse_int<unsigned>(*v); if (!n) { bad(a.substr(2), *v); return 2; }
            if (a == "--max-get-waiters") cfg.max_get_waiters = *n;
            else if (a == "--max-set-waiters") cfg.max_set_waiters = *n;
            else if (a == "--max-connections") cfg.max_connections = *n;
            else cfg.listen_backlog = *n;
        }
        else { std::println(stderr, "error: unknown option '{}' (try --help)", a); return 2; }
    }

    if (https_requested && https_disabled) {
        std::println(stderr, "config error: --https and --no-https are mutually exclusive");
        return 2;
    }
    // Preserve the historical cert-implies-HTTPS behavior unless the cert is being introduced for
    // memcache TLS. In that case opening a second network service requires an explicit --https.
    cfg.enable_https = !https_disabled && !cfg.tls_cert_paths.empty() &&
                       (!cfg.memcache_tls || https_requested);
    if (https_requested && cfg.tls_cert_paths.empty()) cfg.enable_https = true; // validation explains

    // Mirror fills use a large admission quantum, independently of cache-hit tail reads.
    if (cfg.mirror_url && !write_io_chunk_explicit &&
        cfg.write_io_chunk_bytes < kMirrorIoChunk)
        cfg.write_io_chunk_bytes = kMirrorIoChunk;

    if (auto ok = validate(cfg); !ok) {
        std::println(stderr, "config error: {}", ok.error().detail);
        return 1;
    }
    if (cfg.rdma.enabled && !net::rdma_server_available()) {
        std::println(stderr,
                     "config error: --rdma requested, but this build has no "
                     "libibverbs/librdmacm support");
        return 1;
    }
#if !GOBLIN_HAVE_EXASOCK
    if (cfg.net == NetMode::exasock) {
        std::println(stderr,
                     "config error: --net exasock requested, but this build was not configured "
                     "with -DGOBLIN_ENABLE_EXASOCK=ON");
        return 1;
    }
#endif
#if !GOBLIN_HAVE_CURL
    if (cfg.mirror_url) {
        std::println(stderr,
                     "config error: --mirror requested, but this build has no libcurl support");
        return 1;
    }
#endif
#if !GOBLIN_HAVE_TLS
    if (cfg.enable_https || cfg.memcache_tls) {
        std::println(stderr,
                     "config error: TLS requested, but this build has no OpenSSL support");
        return 1;
    }
#endif
    if (cfg.memcache_auth_file) {
        if (auto auth = memcache::Authenticator::load(*cfg.memcache_auth_file); !auth) {
            std::println(stderr, "config error: {}", auth.error().detail);
            return 1;
        }
    }

    // Bind before allocating any pools. Local transient pools first-touch on this node; the head
    // arena receives explicit per-range NUMA policies below. Protocol workers inherit this node;
    // each score scanner later overrides the inherited CPU and memory policy for its own node.
    std::optional<net::NumaBinding> numa;
    if (cfg.numa_enabled) {
        std::vector<std::string> exact_listener_addresses;
        if (cfg.rdma.enabled) exact_listener_addresses.push_back(cfg.rdma.address);
        const bool tcp_listener = cfg.enable_memcache || cfg.enable_http || cfg.enable_https;
        if (tcp_listener && !net::wildcard_address(cfg.listen_address) &&
            !net::loopback_address(cfg.listen_address))
            exact_listener_addresses.push_back(cfg.listen_address);
        const bool wildcard_ethernet_listener =
            tcp_listener && net::wildcard_address(cfg.listen_address);
        auto configured = net::configure_numa(cfg.numa_node, cfg.numa_perverse,
                                              exact_listener_addresses,
                                              wildcard_ethernet_listener);
        if (!configured) {
            std::println(stderr, "numa error: {}", configured.error().detail);
            return 1;
        }
        numa.emplace(std::move(*configured));
        cfg.numa_node = numa->node;
        cfg.numa_cpus = numa->cpus;
        auto memory_plan = net::plan_numa_memory(numa->preferred_memory_node, numa->online_nodes,
                                                 cfg.memory.total_bytes, cfg.memory.sub_bytes);
        if (!memory_plan) {
            std::println(stderr, "numa memory error: {}", memory_plan.error().detail);
            return 1;
        }
        cfg.memory.numa_regions.clear();
        cfg.memory.numa_regions.reserve(memory_plan->size());
        for (const auto& region : *memory_plan) {
            auto cpus = net::numa_node_cpus(region.node, numa->allowed_cpus);
            if (!cpus) {
                std::println(stderr, "numa worker error: {}", cpus.error().detail);
                return 1;
            }
            cfg.memory.numa_regions.push_back(
                {region.node, region.bytes, std::move(*cpus)});
        }
        if (cfg.memory.small_total_bytes) {
            const Size small_sub = cfg.memory.small_sub_bytes.value_or(0);
            auto small_plan = net::plan_numa_memory(
                numa->preferred_memory_node, numa->online_nodes,
                *cfg.memory.small_total_bytes, small_sub, "--small-memory",
                "--small-sub-memory");
            if (!small_plan) {
                std::println(stderr, "NUMA small-object memory error: {}",
                             small_plan.error().detail);
                return 1;
            }
            cfg.memory.small_numa_regions.clear();
            cfg.memory.small_numa_regions.reserve(small_plan->size());
            // Small-object arenas are placement-only; unlike fixed-head score regions, they do
            // not own scanner threads and therefore do not require CPUs on the memory node.
            for (const auto& region : *small_plan)
                cfg.memory.small_numa_regions.push_back({region.node, region.bytes, {}});
        }
    }

    std::println("┌─ goblin-store 0.0.3 ─────────────────────────");
    std::println("│ mode        : {}", cfg.three_layer() ? "3-layer (RAM+SSD+HDD)" : "2-layer (RAM+SSD)");
    if (numa && cfg.numa_perverse)
        std::println("│ head RAM preferred: {} on NUMA node {} (perverse; distance {} from serving node {})",
                     format_size(cfg.memory.total_bytes), numa->preferred_memory_node,
                     *numa->preferred_memory_distance, numa->node);
    else if (numa)
        std::println("│ head RAM local: {} on NUMA node {}", format_size(cfg.memory.total_bytes),
                     numa->preferred_memory_node);
    else
        std::println("│ head RAM    : {} (ordinary OS placement)", format_size(cfg.memory.total_bytes));
    if (numa && cfg.memory.sub_bytes > 0) {
        const Size foreign_nodes = cfg.memory.numa_regions.size() - 1;
        std::vector<unsigned> node_ids;
        node_ids.reserve(static_cast<std::size_t>(foreign_nodes));
        for (std::size_t i = 1; i < cfg.memory.numa_regions.size(); ++i)
            node_ids.push_back(*cfg.memory.numa_regions[i].node);
        std::println("│ head RAM remote: {}/node on NUMA nodes {} ({} total)",
                     format_size(cfg.memory.sub_bytes), net::format_cpu_list(node_ids),
                     format_size(cfg.memory.sub_bytes * foreign_nodes));
    }
    std::println("│ head RAM total: {} (allocation block {}, mlock={})",
                 format_size(cfg.memory.arena_bytes()), format_size(cfg.memory.block_bytes),
                 cfg.memory.lock_memory);
    if (cfg.memory.small_total_bytes) {
        if (numa && cfg.numa_perverse)
            std::println("│ small RAM preferred: {} on NUMA node {} (perverse)",
                         format_size(*cfg.memory.small_total_bytes),
                         numa->preferred_memory_node);
        else
            std::println("│ small RAM local: {}{}", format_size(*cfg.memory.small_total_bytes),
                         numa ? " on NUMA node " + std::to_string(numa->preferred_memory_node)
                              : " (ordinary OS placement)");
        if (numa && cfg.memory.small_sub_bytes.value_or(0) > 0) {
            const Size foreign_nodes = cfg.memory.small_numa_regions.size() - 1;
            std::vector<unsigned> node_ids;
            node_ids.reserve(static_cast<std::size_t>(foreign_nodes));
            for (std::size_t i = 1; i < cfg.memory.small_numa_regions.size(); ++i)
                node_ids.push_back(*cfg.memory.small_numa_regions[i].node);
            std::println("│ small RAM remote: {}/node on NUMA nodes {} ({} total)",
                         format_size(*cfg.memory.small_sub_bytes),
                         net::format_cpu_list(node_ids),
                         format_size(*cfg.memory.small_sub_bytes * foreign_nodes));
        }
        std::println("│ small RAM total: {}", format_size(cfg.memory.small_arena_bytes()));
    } else {
        std::println("│ small RAM   : shared with fixed-head pool (legacy mode)");
    }
    std::println("│ ram_head    : {} / object ({} per allocation block)",
                 format_size(cfg.tiers.ram_head), cfg.memory.block_bytes / cfg.tiers.ram_head);
    std::println("│ HugeTLB     : best effort, {} pages (ordinary memory fallback)",
                 format_size(cfg.memory.hugetlb_page_bytes));
    std::println("│ ssd_prefix  : {} MiB / object", cfg.tiers.ssd_prefix / MiB);
    std::println("│ ssd pool    : {} drive(s), stripe {} KiB", cfg.ssd.dirs.size(), cfg.ssd.stripe_unit / KiB);
    std::println("│ hdd pool    : {} drive(s), stripe {} KiB", cfg.hdd.dirs.size(), cfg.hdd.stripe_unit / KiB);
    std::println("│ memcache    : {}", cfg.enable_memcache ? net::format_endpoint(cfg.listen_address, cfg.memcache_port) +
                                                              (cfg.memcache_tls ? " (TLS 1.3)" : " (plaintext)")
                                                            : std::string("off"));
    std::println("│ memcache UDS: {}", cfg.memcache_socket.value_or("off"));
    std::println("│ auth        : {}", cfg.memcache_auth_file ? "memcached ASCII auth" : "off");
    std::println("│ rdma cache  : {}", cfg.rdma.enabled
                     ? cfg.rdma.address + ":" + std::to_string(cfg.rdma.port) + " (v3, " +
                           format_size(cfg.rdma.bulk_window_bytes) + " x " +
                           std::to_string(cfg.rdma.bulk_window_count) + "/direction)"
                     : std::string("off"));
    std::println("│ http        : {}", cfg.enable_http ? net::format_endpoint(cfg.listen_address, cfg.http_port)
                                                        : std::string("off"));
    std::println("│ https       : {}", cfg.enable_https ? net::format_endpoint(cfg.listen_address, cfg.https_port)
                                                         : std::string("off"));
    std::println("│ mirror      : {}", cfg.mirror_url.value_or("off"));
    if (cfg.mirror_url)
        std::println("│ mirror client: {}",
                     cfg.mirror_client == MirrorClient::uring ? "io_uring HTTP/1.1"
                                                              : "libcurl");
    std::println("│ io backend  : {}", GOBLIN_HAVE_URING ? "io_uring (available)" : "stub (no liburing)");
    const std::string worker_count = cfg.cores
        ? std::to_string(cfg.cores)
        : (numa ? std::to_string(cfg.numa_cpus.size()) + " (node CPUs)"
                : std::string("host CPU count"));
    const std::string_view net_mode = cfg.net == NetMode::async
        ? "async (io_uring loop)"
        : (cfg.net == NetMode::blocking ? "blocking (thread-per-core)"
                                        : "ExaSock (readiness loop; plaintext TCP)");
    std::println("│ net         : {}, {} workers/protocol", net_mode, worker_count);
    if (numa)
        std::println("│ numa        : serving node {} ({}), CPUs {}, promotion={}, perverse={}", numa->node,
                     numa->automatic ? "automatic" : "explicit --numa",
                     net::format_cpu_list(numa->cpus), cfg.numa_promotion, cfg.numa_perverse);
    else
        std::println("│ numa        : off");
    std::println("│ score       : +{} per read, x{} each minute", cfg.access_score.increment,
                 cfg.access_score.decay);
    if (numa && numa->automatic) {
        for (const auto& nic : numa->interfaces) {
            std::println("│ listener NIC: {} {} -> {}", nic.name, nic.address,
                         nic.numa_node ? "NUMA node " + std::to_string(*nic.numa_node)
                                       : std::string("NUMA node unknown"));
        }
    }
    std::println("│ io bufs     : {} x {} KiB read / {} KiB write, stall timeout {}",
                 cfg.io_buffers, cfg.io_chunk_bytes / KiB, cfg.write_io_chunk_bytes / KiB,
                 cfg.io_timeout_ms ? std::to_string(cfg.io_timeout_ms) + "ms" : std::string("off"));
    std::println("│ overload    : {} stream connections, backlog {}, idle {}ms, queue {}ms ({}/{} GET/SET waiters per worker)",
                 cfg.max_connections, cfg.listen_backlog, cfg.idle_timeout_ms,
                 cfg.queue_timeout_ms, cfg.max_get_waiters, cfg.max_set_waiters);
    std::println("│ max object  : {}", format_size(cfg.max_object_size));
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
                                         cfg.cache_bypass == CacheBypass::o_direct,
                                         cfg.access_score, cfg.write_io_chunk_bytes,
                                         cfg.max_object_size);
    if (!tm) {
        std::println(stderr, "startup: {}", tm.error().detail);
        return 1;
    }
    if (!core::Reactor::available()) {
        std::println(stderr, "startup: io_uring unavailable (need liburing + io_uring_setup allowed)");
        return 1;
    }
    // Each worker creates its own reactor + I/O-buffer pool (thread-per-core, ADR-0018).

    // --source preloading (ADR-0015): fill the freshly-wiped pools from disk trees before serving.
    if (!cfg.sources.empty()) {
        http::KeyOptions keyopt;
        keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
        keyopt.keep_query = cfg.key_on_query;
        keyopt.strip_leading_slash = cfg.key_strip_slash;
        const std::size_t n =
            http::preload_sources(cfg.sources, keyopt, *tm, cfg.http_write_mode);
        std::println("preloaded {} file(s) from {} source dir(s)", n, cfg.sources.size());
    }

    std::string listening = "listening:";
    if (cfg.enable_memcache)
        listening += " memcache/" + std::string(cfg.memcache_tls ? "tls " : "tcp ") +
                     net::format_endpoint(cfg.listen_address, cfg.memcache_port);
    if (cfg.memcache_socket)
        listening += " memcache/unix " + *cfg.memcache_socket;
    if (cfg.rdma.enabled)
        listening += " memcache/rdma " + cfg.rdma.address + ":" + std::to_string(cfg.rdma.port);
    if (cfg.enable_http)
        listening += " http/tcp " + net::format_endpoint(cfg.listen_address, cfg.http_port);
    if (cfg.enable_https)
        listening += " https/tcp " + net::format_endpoint(cfg.listen_address, cfg.https_port);
    std::println("{}  (Ctrl-C to stop)", listening);
    if (auto st = memcache::serve(cfg, *tm, index, g_shutdown); !st) {
        std::println(stderr, "serve: {}", st.error().detail);
        return 1;
    }
    std::println("shutdown: in-flight transfers drained, exiting cleanly");
    return 0;
}
