# Goblin Store native C++ and Python client

`goblin-store-client` is a C++ memcache client with a thin nanobind Python
surface. It supports Goblin's native one-sided InfiniBand transport and an
explicitly built ExaSock transport. ExaSock uses the ordinary memcache TCP byte
stream, so either endpoint can interoperate with a compatible unaccelerated
TCP implementation.

The ExaSock SDK is never vendored, downloaded, copied into an sdist, or bundled
into a wheel. ExaSock support is off by default and is built only against a
separately installed system SDK when the user explicitly requests it.

## Native RDMA transport

RDMA-CM establishes a reliable-connected queue pair, after which commands and
values travel through receiver-polled, one-sided RDMA writes. It does not put
TCP on top of IPoIB.

The version-3 transport keeps commands, response headers, trailers, credits,
and completion notices in the Goblin/Packrat control ring. Object bodies move
through registered, credit-controlled bulk windows instead of being split into
192-byte inline writes. Memcache framing remains the ordinary binary-safe text
protocol, and the receive side transparently restores one ordered byte stream.

A matching version-3 native Goblin Store listener is required. The RDMA path
does not silently fall back to TCP or to the version-2 inline-body path.

## Python installation

The package is Linux-only. For the default RDMA build, install the system verbs
development files first:

```sh
sudo apt-get install -y build-essential cmake ninja-build \
    python3-dev libibverbs-dev librdmacm-dev
python3 -m pip install ./python
```

The resulting import has no runtime Python dependencies:

```python
from goblin_store import Client

with Client(
    "10.88.88.1",
    11211,
    bulk_window_bytes=256 * 1024,
    bulk_window_count=4,
) as cache:
    cache.set("report", b"binary\x00document", flags=7, expire=60)
    assert cache.get("report") == b"binary\x00document"

    current = cache.gets("report")
    if current is not None:
        cache.cas("report", b"replacement", current.cas, flags=current.flags)
```

## ExaSock build and use

Install Cisco ExaSock on the target system according to its license and vendor
instructions. Then request the backend explicitly while building locally:

```sh
python3 -m pip wheel ./python \
  --config-settings=cmake.define.GOBLIN_STORE_CLIENT_ENABLE_EXASOCK=ON \
  --config-settings=cmake.define.GOBLIN_STORE_CLIENT_ENABLE_RDMA=OFF
python3 -m pip install ./goblin_store_client-*.whl
```

The build fails if the installed `exasock` launcher and public SDK headers are
not found. Goblin resolves the verification functions published by the active
preload library at runtime; it does not link an ExaSock library or record one
in package metadata. Do not repair an ExaSock-enabled wheel in a way that
copies vendor libraries into it. Such wheels are intended to be built and used
locally on systems with their own ExaSock installation.

Select the transport in Python and start the process through the installed
launcher:

```sh
exasock --no-auto python3 application.py
```

```python
from goblin_store import Client, exasock_active

assert exasock_active()
with Client("192.0.2.20", 11211, transport="exasock") as cache:
    cache.set("report", b"ordinary memcache TCP")
```

An explicitly selected ExaSock connection fails if the interception library is
not active or if the connected socket did not map to an accelerated SmartNIC
port. It never silently becomes kernel TCP. The remote server does not need
ExaSock; it only needs a compatible memcache TCP endpoint.

The ExaSock address must be numeric IPv4, not IPv6 or a hostname. Native RDMA
accepts numeric IPv4 or IPv6. Name resolution is deliberately excluded so
`connect_timeout` bounds the complete selected-transport connection setup
rather than starting only after an unbounded DNS lookup.

`get()` materializes the value as Python `bytes`. For large objects,
`get_into()` keeps Python memory bounded and writes chunks no larger than
`chunk_bytes` to a binary writer:

```python
with open("object.bin", "wb") as output:
    info = cache.get_into("large-object", output)
```

The writer's `write()` method must return an integer exactly equal to the
number of bytes passed. `None`, `bool`, and partial-write results are errors;
this prevents a nonblocking writer from silently dropping value bytes.

The GIL is released during connect, send, polling, and receive. Operations on
one `Client` are serialized because memcache replies carry no request ID; use
one client per worker when parallel connections are desired.

`bulk_window_bytes` is the maximum body fragment transferred by one large RDMA
write. It must be a power of two of at least 4 KiB. `bulk_window_count` must be
between 1 and 65,535. A connection registers that many receive windows and the
same number of outbound staging windows, so its bulk mapping consumes
`2 * bulk_window_bytes * bulk_window_count` bytes. The combined mapping must fit
in 32 bits. The defaults therefore use 2 MiB: four 256-KiB receive windows and
four matching staging windows, in addition to the control ring. These transport
windows are independent of Goblin Store's configurable resident object-head
size.

A timeout or connection failure does not roll back a mutation. If `set()`,
`add()`, `replace()`, `cas()`, or `delete()` raises after transmission starts,
the server may already have applied it even though the reply was not observed.
Treat that result as indeterminate: reconnect and read the key to reconcile it,
using `gets()`/CAS when concurrent writers matter.

The integration test is opt-in because it needs configured hardware and a
matching server:

```sh
GOBLIN_STORE_RDMA_ADDRESS=10.88.88.1 \
GOBLIN_STORE_RDMA_PORT=11211 pytest -q python/tests/test_rdma_integration.py
```

The ExaSock integration test must itself run through the vendor launcher:

```sh
GOBLIN_STORE_EXASOCK_ADDRESS=192.0.2.20 \
GOBLIN_STORE_EXASOCK_PORT=11211 \
exasock --no-auto python3 -m pytest -q python/tests/test_exasock_integration.py
```

## C++ installation

The C++ library is an independent project beneath `python/cpp`; it has no
Python or nanobind dependency. From a fresh checkout:

```sh
git clone https://github.com/adamdeprince/goblin-store.git
sudo apt-get install -y build-essential cmake ninja-build \
    libibverbs-dev librdmacm-dev
cmake -S goblin-store/python/cpp -B goblin-store/build/client -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build goblin-store/build/client
ctest --test-dir goblin-store/build/client --output-on-failure
sudo cmake --install goblin-store/build/client
```

For a separately installed ExaSock SDK, add
`-DGOBLIN_STORE_CLIENT_ENABLE_EXASOCK=ON`. This option defaults to `OFF` and a
missing SDK is a configuration error only when it was explicitly enabled.

Installation provides headers, `libgoblin-store-client`, a relocatable CMake
package, and `goblin-store-client.pc`. See the
[complete C++ API and consumer example](https://github.com/adamdeprince/goblin-store/tree/master/python/cpp)
for `find_package()` and static-library usage.

## RDMA registered-memory boundary

Commands and status stay in the small inline ring; value bodies always use the
bulk path, including bodies smaller than one control slot. A zero-byte value has
no body fragment. Window credits prevent either peer from overwriting data that
the receiver has not consumed yet.

The peer receives rkeys only for this connection's control and bulk mappings,
not for Goblin Store's object arenas or disk buffers. The current bulk MR covers
both staging and receive halves even though a conforming sender targets only the
receive half, so the transport assumes a trusted fabric: there is no transport
authentication or encryption.
