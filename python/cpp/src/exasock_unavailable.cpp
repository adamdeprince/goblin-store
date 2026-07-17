#include "goblin/store/client.hpp"

namespace goblin::client {

bool exasock_available() noexcept { return false; }
bool exasock_active() noexcept { return false; }

std::unique_ptr<Transport> connect_exasock(const ExasockOptions&) {
    throw ConnectionError(
        "ExaSock support is unavailable; install the ExaSock SDK and configure "
        "with -DGOBLIN_STORE_CLIENT_ENABLE_EXASOCK=ON");
}

} // namespace goblin::client
