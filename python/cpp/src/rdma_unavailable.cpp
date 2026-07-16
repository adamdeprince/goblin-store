#include "goblin/store/client.hpp"

namespace goblin::client {

bool rdma_available() noexcept { return false; }

std::unique_ptr<Transport> connect_rdma(const Options&) {
    throw ConnectionError(
        "native RDMA support is unavailable; build on Linux with libibverbs-dev and librdmacm-dev");
}

} // namespace goblin::client
