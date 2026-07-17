#pragma once

#include "goblin/store/transport.hpp"

#include <memory>

namespace goblin::client::detail {

// Test-only seams compiled into enabled ExaSock test builds. They are not
// installed and are absent from production and Python-wheel builds.
std::unique_ptr<Transport> make_exasock_transport_for_test(int fd);
[[noreturn]] void throw_exasock_open_failure_for_test(bool timed_out);

} // namespace goblin::client::detail
