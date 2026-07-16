# Goblin Store C++ client library

This directory is a standalone C++20 project. It installs the native
InfiniBand memcache client without Python or nanobind.

## Build and install

```sh
sudo apt-get install -y build-essential cmake ninja-build \
    libibverbs-dev librdmacm-dev

cmake -S python/cpp -B build/goblin-store-client -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON
cmake --build build/goblin-store-client
ctest --test-dir build/goblin-store-client --output-on-failure
sudo cmake --install build/goblin-store-client
```

Use `-DBUILD_SHARED_LIBS=OFF` for a static library. Installation provides:

- `goblin/store/client.hpp` and `goblin/store/transport.hpp`;
- `libgoblin-store-client`;
- `GoblinStoreClientConfig.cmake` and version metadata;
- `goblin-store-client.pc`.

## CMake consumer

```cmake
find_package(GoblinStoreClient CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE GoblinStoreClient::client)
```

```cpp
#include <goblin/store/client.hpp>

int main() {
    goblin::client::Options options;
    options.address = "10.88.88.1";
    options.port = 11211;
    options.ring_bytes = 64 * 1024;
    options.bulk_window_bytes = 256 * 1024;
    options.bulk_window_count = 4;

    auto client = goblin::client::Client::connect(options);
    client.set("key", "binary value", 0, 60);
    if (auto item = client.gets("key")) {
        client.compare_exchange("key", "new value", *item->cas);
    }
}
```

`Options::address` must be a numeric IPv4 or IPv6 address, not a hostname.
Avoiding DNS ensures that `connect_timeout` bounds the complete RDMA-CM setup.

Mutation timeouts are ambiguous by nature: if `set()`, `add()`, `replace()`,
`compare_exchange()`, or `erase()` throws after transmission starts, the server
may have committed the operation before the reply was lost. Reconnect and read
the key to reconcile the result; use `gets()` and CAS when concurrent writers
make a blind retry unsafe.

`get_to()` accepts a callback for bounded-memory retrieval. The callback must
not start another transaction on the same `Client`. A `Client` serializes
complete transactions on its one ordered queue pair; independent clients
provide parallelism.

Commands and trailers use the inline control ring. Value bodies use registered
bulk windows, including values smaller than the ring's inline payload limit;
only a zero-byte value emits no body fragment. `bulk_window_bytes` controls the
largest body fragment and must be a power of two of at least 4 KiB;
`bulk_window_count` must be between 1 and 65,535. Each connection registers both
receive and outbound-staging halves, consuming
`2 * bulk_window_bytes * bulk_window_count` bytes in addition to its control
ring, and that combined bulk mapping must fit in 32 bits. The defaults consume
2 MiB of bulk registered memory. These settings are independent of the server's
resident object-head size.

The public transport injection seam permits deterministic tests without
duplicating memcache framing and parsing. Incoming inline and bulk fragments
are exposed as one ordered byte stream, while outbound tests can verify that no
value byte enters the control ring.

## Native RDMA benchmark

Top-level builds also produce `goblin_store_rdma_benchmark`. Its three modes
load a fixed object set, capture single-outstanding-request latency, and measure
payload throughput with one sequential request stream per QP. For a 4 GiB
working set of 256 KiB values:

```sh
numactl --cpunodebind=1 --membind=1 \
  ./build/goblin-store-client/goblin_store_rdma_benchmark load \
  --address 10.88.88.1 --object-bytes 262144 --objects 16384 --threads 8

numactl --cpunodebind=1 --membind=1 \
  ./build/goblin-store-client/goblin_store_rdma_benchmark latency \
  --address 10.88.88.1 --object-bytes 262144 --objects 16384 \
  --requests 1000000 --seed 0x676f626c696e \
  --csv /var/tmp/goblin-rdma-latency.csv

numactl --cpunodebind=1 --membind=1 \
  ./build/goblin-store-client/goblin_store_rdma_benchmark throughput \
  --address 10.88.88.1 --object-bytes 262144 --objects 16384 \
  --threads 8 --warmup-seconds 5 --seconds 30 \
  --csv /var/tmp/goblin-rdma-throughput-8qp.csv
```

The latency CSV records monotonic nanoseconds from successful GET submission to
the first response bytes (`ttfb_ns`), first application-visible body bytes
(`body_ready_ns`), and consumption of the complete value and memcache trailer
(`total_ns`). Reads select keys uniformly with the documented SplitMix64 seed,
so repeated runs use the same sequence. The throughput result counts value
payload bytes, not protocol metadata. Run the client on the NUMA node local to
its InfiniBand HCA; CPU and memory binding are intentionally left to `numactl`
so the benchmark records an explicit test policy. The direct 40 Gbit/s R820
results and the important distinction between response-header TTFB and
full-window payload readiness are documented in
[`docs/native-rdma-256k-performance.md`](../../docs/native-rdma-256k-performance.md).
