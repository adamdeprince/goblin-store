// Operator-facing configuration and its validation (encodes ADR invariants).
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <cstdint>
#include <string>
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

struct MemoryConfig {                  // ADR-0008
    Size total_bytes  = 1 * GiB;       // fixed, command-line specified; never grows
    Size block_bytes  = 1 * MiB;       // power-of-two block size
    bool lock_memory  = true;          // mlock / MAP_LOCKED (never swap the head out)
    bool use_hugepages = true;
};

enum class EvictionPolicyKind { sieve, s3fifo, tinylfu }; // selectable (ADR-0007)
enum class NetMode { blocking, async };                   // --net (ADR-0018: async = io_uring loop)

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
    unsigned      cores = 0;              // 0 => all available

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
    NetMode       net = NetMode::async;        // async io_uring loop (default); --net blocking falls back

    MemoryConfig   memory;
    TierSizes      tiers;
    PoolConfig     ssd;
    PoolConfig     hdd;                   // empty dirs => 2-layer mode
    EvictionConfig eviction;
    WriteMode      memcache_write_mode = WriteMode::evict;  // cache default
    WriteMode      http_write_mode     = WriteMode::block;  // origin default
    CacheBypass    cache_bypass = CacheBypass::o_direct;

    bool three_layer() const noexcept { return !hdd.dirs.empty(); }
};

// Enforce the cross-cutting invariants from the ADRs. Returns the first violation.
Status validate(const ServerConfig&);

} // namespace goblin
