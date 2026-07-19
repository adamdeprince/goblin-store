# ExaSock transport

Goblin Store's ExaSock backend accelerates its ordinary TCP listeners. It does
not define a new wire protocol: memcached remains the binary-safe memcache text
protocol, and HTTP remains HTTP/1.1. A peer using a conventional Ethernet card,
kernel TCP, `curl`, or an ordinary memcache client therefore sees a normal
server. ExaSock is needed on the peer only when that peer should use its own
supported SmartNIC for acceleration.

The server backend covers both plaintext TCP services selected by
`--net exasock`:

- memcached on `--memcache-port` (11211 by default); and
- HTTP on `--http-port` (8080 by default).

The separately installable C++ and Python clients implement the memcache
protocol. They can use ExaSock against an ExaSock Goblin Store server, a Goblin
Store server using kernel TCP, or another wire-compatible memcache server.

## Dependency and license boundary

ExaSock is always a separately installed, system-only dependency. It is off by
default. This repository must not contain an ExaSock source file, binary,
header copy, submodule, package, patch, `FetchContent` declaration, or other
vendored artifact.

An explicitly enabled build consumes the public headers already installed on
the build host. Goblin Store does **not** link to an ExaSock ELF object. The
server and clients resolve the verification entry points exported by the
active preload library with `dlsym(RTLD_DEFAULT, ...)`; their only ordinary
runtime-loader dependency is the platform's `libdl` where one is required.
Installed Goblin libraries, CMake exports, pkg-config metadata, Python sdists,
and wheels do not embed an ExaSock library or advertise one as a link
dependency.

Install the SDK, driver, utilities, and launcher outside the Goblin Store
source and build trees, following Cisco's instructions and license. An
ExaSock-enabled Goblin artifact is intended for a system which has its own
licensed installation. Do not use wheel-repair or packaging tools to copy the
vendor runtime into a wheel.

## Building the server

The default build has no ExaSock support or dependency. Request it explicitly
with `GOBLIN_ENABLE_EXASOCK`:

```sh
cmake -S . -B build/exasock -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_ENABLE_EXASOCK=ON
cmake --build build/exasock
```

Configuration is Linux-only and fails when the installed public ExaSock
headers or `exasock` launcher cannot be found. Turning the option off restores
the ordinary dependency-free network build:

```sh
cmake -S . -B build/default -DGOBLIN_ENABLE_EXASOCK=OFF
```

## Running the server

Run the explicitly built backend through the system launcher in opt-in mode:

```sh
EXASOCK_DEBUG=1 exasock --no-auto ./build/exasock/goblin-store \
  --net exasock \
  --listen-address 192.0.2.10 \
  --memcache-port 11211 \
  --http-port 8080 \
  --ssd-dir /var/tmp/goblin-ssd
```

The example omits the site's normal memory, storage, and source options. Pool
directories must still be prepared in the usual way before the server starts.

`--listen-address` must be an exact numeric IPv4 address assigned to the
SmartNIC port. `0.0.0.0` is rejected in ExaSock mode. This prevents a wildcard
listener from accidentally selecting the management NIC, and gives automatic
NUMA selection an unambiguous network device. The same address is used by the
enabled memcache and HTTP listeners. Use `--no-memcache` or `--no-http` when
only one protocol is wanted.

`exasock --no-auto` leaves unrelated process sockets on kernel TCP. Goblin
explicitly opts its selected listeners into acceleration with
`SO_EXA_NO_ACCEL`. Startup is fail-closed: before listener opt-in, the server
verifies through the preload namespace that the ExaSock runtime is active.
It deliberately does not call the extension device query on a listening
socket. The centralized acceptor checks each connection with
`exasock_tcp_get_device` before assigning its fd to a protocol worker. A
missing preload, unsupported interface, or unaccelerated connection is
rejected rather than silently using kernel TCP.

The ExaSock path uses a nonblocking coordinator acceptor, then nonblocking
`recv` and `send` calls in the assigned worker's readiness loop so the preload
library sees all socket operations. Disk work remains asynchronous: the same
worker retains its `io_uring` reactor for SSD and HDD reads, and the reactor's
completion event is integrated into the readiness loop. Thus serving a
resident head and starting/overlapping its tail read does not turn into
blocking disk I/O merely because the socket transport changed.

HTTPS is not accelerated by `--net exasock`; the accelerated services are
memcached and plaintext HTTP. If HTTPS is enabled in the same process, it
retains the existing `io_uring`/OpenSSL path. External TLS termination remains
an alternative.

## Wire compatibility

Nothing special is required on an ordinary peer. For example:

```sh
printf 'version\r\n' | nc 192.0.2.10 11211
curl --http1.1 http://192.0.2.10:8080/example-object
```

Traffic can arrive through a normal Ethernet path and the peer does not need
Goblin code or ExaSock. The ExaSock launcher, SDK, and SmartNIC affect only the
local endpoint on which they are installed.

## C++ memcache client

The C++ client is its own CMake project. Its ExaSock backend is independently
opt-in:

```sh
cmake -S python/cpp -B build/client-exasock -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_STORE_CLIENT_ENABLE_EXASOCK=ON \
  -DGOBLIN_STORE_CLIENT_ENABLE_RDMA=OFF
cmake --build build/client-exasock
ctest --test-dir build/client-exasock --output-on-failure
sudo cmake --install build/client-exasock
```

The RDMA option is independent; it may remain enabled when both transports and
their system SDKs are wanted. ExaSock configuration fails unless the system
headers and launcher are present.

Select the transport explicitly in code:

```cpp
#include <goblin/store/client.hpp>

int main() {
    goblin::client::ExasockOptions options;
    options.address = "192.0.2.10";
    options.port = 11211;

    auto cache = goblin::client::Client::connect_exasock(options);
    cache.set("goblin", "chickens eat software bugs");
    return cache.get("goblin") ? 0 : 1;
}
```

Then launch the application through ExaSock:

```sh
EXASOCK_DEBUG=1 exasock --no-auto ./my-client
```

`ExasockOptions::address` is a numeric IPv4 address; the accelerated TCP
extension verifies AF_INET sockets, so IPv6 and hostnames are rejected. The
library does not perform DNS inside the connection timeout.
`exasock_available()` reports whether the client was built with this backend,
while `exasock_active()` reports whether the preload runtime is active in the
current process.
`Client::connect_exasock()` additionally verifies that the connected socket is
accelerated. Any failed check raises an explicit connection error rather than
using kernel TCP.

## Python memcache client

Build an ExaSock-enabled wheel locally on a system with the external SDK:

```sh
python3 -m pip wheel ./python \
  --config-settings=cmake.define.GOBLIN_STORE_CLIENT_ENABLE_EXASOCK=ON \
  --config-settings=cmake.define.GOBLIN_STORE_CLIENT_ENABLE_RDMA=OFF
python3 -m pip install ./goblin_store_client-*.whl
```

Select `transport="exasock"` and run Python through the same system launcher:

```python
from goblin_store import Client, exasock_active

assert exasock_active()
with Client("192.0.2.10", 11211, transport="exasock") as cache:
    cache.set("goblin", b"chickens eat software bugs")
    assert cache.get("goblin") == b"chickens eat software bugs"
```

```sh
EXASOCK_DEBUG=1 exasock --no-auto python3 application.py
```

The Python extension uses the same fail-closed C++ transport. Calls on one
client are ordered and serialized; use independent clients for concurrent
connections.

## NUMA placement and CPU use

The server's exact `--listen-address` participates in its existing NIC-to-NUMA
discovery. Unless `--numa` overrides it, Goblin pins protocol workers to the
NUMA node local to that interface and allocates preferred head memory there.
Check the selected NIC and CPU list in the startup summary.

Pin standalone C++ and Python clients to CPUs local to their own SmartNIC. On a
typical Linux system the device node can be inspected with:

```sh
cat /sys/class/net/DEVICE/device/numa_node
cat /sys/class/net/DEVICE/device/local_cpulist
```

Then apply the site's CPU/memory policy outside the launcher, for example:

```sh
numactl --cpunodebind=NODE --membind=NODE \
  exasock --no-auto ./my-client
```

ExaSock's readiness calls may spin for latency. Do not oversubscribe the CPUs
assigned to accelerated workers, and reserve capacity for disk completion and
the existing NUMA score/promotion work.

## Operational verification

Use all three checks when commissioning a port:

1. Start the process with `EXASOCK_DEBUG=1` and
   `exasock --no-auto`; the vendor runtime should report acceleration for the
   selected descriptors.
2. Confirm the expected listener and established connection with the
   separately installed `exasock-stat` utility.
3. Confirm Goblin did not reject startup or connection: it resolves
   `exasock_loaded` and `exasock_tcp_get_device` from the preload namespace and
   treats an unavailable function or unaccelerated socket as fatal for the
   explicitly selected backend.

Also test both ordinary interoperability paths: a normal memcache client on
port 11211 and a normal HTTP/1.1 client on port 8080. This catches accidental
protocol changes independently of SmartNIC acceleration.

## Limits and security boundary

- ExaSock support is Linux-only and requires hardware, driver, firmware, and a
  userspace runtime supported by the installed vendor release.
- The server and ExaSock client require exact numeric IPv4 addresses assigned
  to SmartNIC paths; IPv6 and hostnames are rejected.
- The dedicated C++ and Python APIs speak memcache, not HTTP. HTTP remains a
  standard TCP service and can be consumed by any HTTP/1.1 client, accelerated
  or otherwise.
- Socket semantics and unsupported operations follow the installed ExaSock
  release. Keep deployment tests pinned to that release rather than assuming
  every kernel-socket feature is intercepted.
- A userspace-bypass network stack may not traverse the host kernel's normal
  packet filtering and observability path. Apply the vendor's hardening
  guidance and enforce required isolation or ACLs at the SmartNIC and network,
  rather than assuming host firewall rules inspect accelerated traffic.
- Treat the ExaSock launcher, preload path, SDK, and driver as privileged
  external deployment inputs. Install them from a trusted vendor source, keep
  their license separate, and run Goblin Store with only the OS capabilities
  needed for its configured memory and storage policy.
