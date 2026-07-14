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
    block,              // origin: pin existing objects, refuse/backpressure on full
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

    Size arena_bytes() const noexcept {
        if (numa_regions.empty()) return total_bytes;
        Size total = 0;
        for (const auto& region : numa_regions) total += region.bytes;
        return total;
    }
};

enum class EvictionPolicyKind { sieve, s3fifo, tinylfu }; // selectable (ADR-0007)
enum class NetMode { blocking, async };                   // --net (ADR-0018: async = io_uring loop)

struct AccessScoreConfig {
    double decay = 0.5;     // once/minute multiplier; strictly between zero and one
    double increment = 1.0; // added once for each successful logical object read
};

struct EvictionConfig {                // ADR-0007 / ADR-0012
    EvictionPolicyKind policy = EvictionPolicyKind::s3fifo;
    std::uint64_t max_ssd_objects = 0; // SSD count bound; 0 => derive ssd_capacity/ssd_prefix
    double high_watermark = 0.90;      // start background reclaim
    double low_watermark  = 0.80;      // reclaim down to here
};

struct ServerConfig {
    std::uint16_t memcache_port = 11211;  // memcache over TCP only (ADR-0005)
    std::uint16_t http_port     = 8080;   // plaintext HTTP listener
    std::uint16_t https_port    = 8443;   // TLS listener (ADR-0005)
    bool          enable_memcache = true;
    bool          enable_http     = true;  // serve plaintext HTTP
    bool          enable_https    = false; // serve TLS; HTTP+HTTPS may both run
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

    // Streaming I/O buffers (ADR-0017) — separate from the head pool. io_buffers applies per worker
    // to the read pool and (once) to the write-staging pool; each is io_chunk_bytes.
    Size          io_chunk_bytes = 256 * KiB; // per-chunk streaming buffer size
    unsigned      io_buffers     = 64;         // streaming chunk buffers (read pool/worker; write staging)
    unsigned      io_timeout_ms  = 30000;      // drop a stalled in-flight transfer (slow client); 0 = off
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

    bool three_layer() const noexcept { return !hdd.dirs.empty(); }
};

// Enforce the cross-cutting invariants from the ADRs. Returns the first violation.
Status validate(const ServerConfig&);

} // namespace goblin
