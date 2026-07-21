// Operator-facing configuration and its validation (encodes ADR invariants).
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace goblin {

enum class WriteMode {  // ADR-0010
    evict,              // cache: evict low-value objects to admit
    block,              // this admission does not reclaim on filesystem exhaustion
};

enum class CacheBypass { // ADR-0011
    o_direct,            // bypass the kernel page cache (default)
    rwf_dontcache,       // uncached buffered I/O (benchmark alternative)
    buffered,            // ordinary buffered I/O (fallback)
};

// Positional per-object tier sizes (ADR-0006). Invariant: ram_head <= ssd_prefix.
struct TierSizes {
    Size ram_head   = 256 * KiB;  // bytes cached in RAM at the head of each object
    Size ssd_prefix = 32  * MiB;  // bytes that live on the SSD pool (includes the head)
};                                // remainder (size - ssd_prefix) lives on the HDD pool

// A storage pool == a list of drive directories, striped round-robin (ADR-0009).
struct PoolConfig {
    std::vector<std::string> dirs;     // one directory per drive
    Size stripe_unit = 64 * KiB;       // round-robin stripe granularity (HDD wants MBs)
};

struct NumaMemoryRegionConfig {
    // main() always supplies a physical node. nullopt keeps logical multi-region layouts usable by
    // portable library callers and tests without asking a non-Linux host to install NUMA policy.
    std::optional<unsigned> node;
    Size bytes = 0;
    // CPUs on `node` that were allowed before main narrowed itself to the serving node. The
    // per-node score scanners use this saved intersection to broaden their inherited affinity
    // safely without escaping an operator-supplied taskset/cgroup restriction.
    std::vector<unsigned> cpus;

    NumaMemoryRegionConfig() = default;
    NumaMemoryRegionConfig(std::optional<unsigned> region_node, Size region_bytes,
                           std::vector<unsigned> region_cpus = {})
        : node(region_node), bytes(region_bytes), cpus(std::move(region_cpus)) {}
};

#if defined(__aarch64__) || defined(__arm__) || defined(__loongarch__)
inline constexpr Size kDefaultHugeTlbPage = 32 * MiB;
#else
inline constexpr Size kDefaultHugeTlbPage = 2 * MiB;
#endif
inline constexpr Size kDefaultMemoryBlock = kDefaultHugeTlbPage;

struct MemoryConfig {                  // ADR-0008
    Size total_bytes  = 1 * GiB;       // --memory: preferred head arena (selected node normally)
    Size sub_bytes    = 0;             // --sub-memory: head arena bytes on each other NUMA node
    // Supplying --small-memory opts into physically separate fixed-head and small-object pools.
    // nullopt preserves the historical shared pool for existing invocations and library callers.
    std::optional<Size> small_total_bytes; // --small-memory: preferred small-object arena
    std::optional<Size> small_sub_bytes;   // --small-sub-memory: bytes on each other NUMA node
    // Logical allocation/promotion blocks may span several physical HugeTLB pages. Keeping the two
    // sizes distinct prevents --block 4M on x86 from requesting a nonexistent 4 MiB page order.
    Size block_bytes  = kDefaultMemoryBlock; // --block; power-of-two multiple of hugetlb_page_bytes
    Size hugetlb_page_bytes = kDefaultHugeTlbPage; // platform backing-page order (not a CLI knob)
    Size small_min_alloc = 16;         // arena alignment for RAM-only objects smaller than ram_head.
                                       // Exact ram_head objects and larger-object heads use buddy
                                       // slots, making complete allocation blocks promotable.
    bool lock_memory  = true;          // mlock normal mappings; explicit hugetlb is unswappable
    bool use_hugepages = true;          // best-effort explicit hugetlb; normal memory on failure
    // Runtime-resolved preferred-first layout. Empty for direct library/test callers that want the
    // ordinary single-region allocator; main() populates it after NUMA selection.
    std::vector<NumaMemoryRegionConfig> numa_regions;
    std::vector<NumaMemoryRegionConfig> small_numa_regions;

    bool split_pools() const noexcept { return small_total_bytes.has_value(); }

    Size arena_bytes() const noexcept {
        if (numa_regions.empty()) return total_bytes;
        Size total = 0;
        for (const auto& region : numa_regions) total += region.bytes;
        return total;
    }

    Size small_arena_bytes() const noexcept {
        if (!small_total_bytes) return 0;
        if (small_numa_regions.empty()) {
            const Size foreign_nodes =
                numa_regions.empty() ? 0 : static_cast<Size>(numa_regions.size() - 1);
            return *small_total_bytes + small_sub_bytes.value_or(0) * foreign_nodes;
        }
        Size total = 0;
        for (const auto& region : small_numa_regions) total += region.bytes;
        return total;
    }
};

enum class EvictionPolicyKind { sieve, s3fifo, tinylfu }; // selectable (ADR-0007)
enum class NetMode { blocking, async, exasock };          // --net (ADR-0018)
enum class MirrorClient { curl, uring };                   // --mirror-client upstream transport

struct AccessScoreConfig {
    double decay = 0.5;     // once/minute multiplier; strictly between zero and one
    double increment = 1.0; // added once for each successful logical object read
};

struct EvictionConfig {                // ADR-0007 / ADR-0012
    EvictionPolicyKind policy = EvictionPolicyKind::s3fifo;
    std::uint64_t max_ssd_objects = 0; // disk-backed object count bound; 0 => unbounded
    double high_watermark = 0.90;      // start background reclaim
    double low_watermark  = 0.80;      // reclaim down to here
    unsigned reclaim_interval_ms = 1000; // live filesystem check/reclaim cadence; 0 disables
};

// Native reliable-connected RDMA memcache endpoint. The control ring carries only typed command,
// status, credit, and completion records; object bodies use the separately registered bulk windows.
// One connection owns two bulk halves (TX staging and peer-write RX), each containing window_count
// fixed-size windows. The v3 wire descriptor derives the count from this geometry.
struct RdmaConfig {
    bool enabled = false;
    std::string address;                    // numeric IPv4/IPv6 bind address; --rdma enables
    std::uint16_t port = 11211;
    Size ring_bytes = 64 * KiB;             // requested control-ring slot budget / connection
    Size bulk_window_bytes = 256 * KiB;     // power of two; one large RDMA WRITE per body piece
    unsigned bulk_window_count = 4;         // per direction; >=2 preserves head/tail overlap
    unsigned backlog = 128;
};

struct ServerConfig {
    // Numeric IPv4/IPv6 address shared by the TCP listeners. Loopback-only by default: exposing an
    // unauthenticated cache on an external interface must be an explicit operator decision.
    // ExaSock requires an exact SmartNIC address so acceleration and NUMA locality cannot silently
    // resolve to the management interface.
    std::string   listen_address = "127.0.0.1";
    std::uint16_t memcache_port = 11211;  // memcache over TCP only (ADR-0005)
    std::uint16_t http_port     = 8080;   // plaintext HTTP listener
    std::uint16_t https_port    = 8443;   // TLS listener (ADR-0005)
    bool          enable_memcache = true;
    bool          enable_http     = true;  // serve plaintext HTTP
    bool          enable_https    = false; // serve TLS; HTTP+HTTPS may both run
    bool          memcache_tls    = false; // replace plaintext on memcache_port with TLS 1.3
    std::optional<std::string> memcache_auth_file; // memcached ASCII auth file: user:password
    std::optional<std::string> memcache_socket;    // AF_UNIX memcache listener; no TLS
    std::uint32_t memcache_socket_mode = 0600;     // chmod after bind
    // One cert/key pair per domain (SNI selects, ADR-0005); paired by index. Repeat --tls-cert/--tls-key.
    std::vector<std::string> tls_cert_paths; // PEM certificate chains
    std::vector<std::string> tls_key_paths;  // PEM private keys
    unsigned      cores = 0;              // 0 => all CPUs allowed on the selected NUMA node
    bool          numa_enabled = true;    // --no-numa disables affinity, placement, and promotion
    bool          numa_promotion = true;  // --no-numa-promotion keeps placement but disables swaps
    bool          numa_perverse = false;  // --perverse: prefer farthest head-memory node (test only)
    std::optional<unsigned> numa_node;     // --numa NODE; null => derive from listener NIC locality
    std::vector<unsigned> numa_cpus;       // runtime-resolved node CPUs allowed by process/cgroup

    // HTTP key derivation (ADR-0015)
    bool          http_vhost   = false;   // key = Host + URI  (default: key = URI path)
    bool          key_on_query = false;   // include the query string in the key (default: strip)
    bool          key_strip_slash = false; // path mode: drop the key's leading '/' (so `set foo` == GET /foo)
    std::string   http_index = "index.html"; // HTTP-only dir index for paths ending in '/'; empty = off
    std::vector<std::string> sources;     // --source dirs preloaded at startup
    // Reverse-cache origin. Mirror mode has its own URI key derivation (including the query) and
    // therefore cannot share virtual-host key space. Empty means the ordinary object endpoint.
    std::optional<std::string> mirror_url; // --mirror URL; http[s]://host[/base/path]
    MirrorClient  mirror_client = MirrorClient::curl;

    // Streaming I/O buffers (ADR-0017) — separate from the head pool. Read and write quanta are
    // deliberately independent: large mirror admission writes must not inflate warmed-tail reads.
    Size          io_chunk_bytes = 256 * KiB; // cache-hit/read streaming chunk size
    Size          write_io_chunk_bytes = 256 * KiB; // SET/mirror write-staging chunk size
    unsigned      io_buffers     = 64;         // streaming chunk buffers (read pool/worker; write staging)
    unsigned      io_timeout_ms  = 30000;      // drop a stalled in-flight transfer (slow client); 0 = off
    unsigned      idle_timeout_ms = 300000;    // expire idle keepalive connections; 0 = off
    unsigned      queue_timeout_ms = 30000;    // max wait for a GET/SET streaming buffer; 0 = off
    unsigned      max_get_waiters = 64;        // bounded read-buffer wait queue, per worker
    unsigned      max_set_waiters = 64;        // bounded write-buffer wait queue, per worker
    unsigned      max_connections = 1024;      // aggregate TCP + Unix connections across listeners
    unsigned      listen_backlog = 1024;       // TCP completed-connection queue (kernel may clamp)
    Size          max_object_size = kMaxObjectSize; // storage admission bound for every protocol
    unsigned      ttl_reap_ms    = 1000;       // TTL reaper sweep period (ms); 0 = off (lazy-skip only)
    unsigned      shutdown_grace_ms = 5000;    // graceful-shutdown drain deadline for in-flight transfers
    bool          read_ahead     = true;       // double-buffered GET read-ahead; off => serial (A/B knob)
    NetMode       net = NetMode::async;        // async io_uring loop (default); --net blocking falls back

    MemoryConfig   memory;
    TierSizes      tiers;
    PoolConfig     ssd;
    PoolConfig     hdd;                   // empty dirs => 2-layer mode
    EvictionConfig eviction;
    AccessScoreConfig access_score;
    WriteMode      memcache_write_mode = WriteMode::evict;  // cache default
    WriteMode      http_write_mode     = WriteMode::block;  // origin default
    CacheBypass    cache_bypass = CacheBypass::o_direct;
    RdmaConfig     rdma;                    // native InfiniBand/RoCE memcache (not TCP/IPoIB)

    bool three_layer() const noexcept { return !hdd.dirs.empty(); }
};

// Enforce the cross-cutting invariants from the ADRs. Returns the first violation.
Status validate(const ServerConfig&);

} // namespace goblin
