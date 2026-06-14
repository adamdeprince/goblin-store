// The single main() for the goblin_tests executable. Test cases live in the other tests/*.cpp
// files and self-register via the TEST() macro (see mini_test.hpp).
#include "mini_test.hpp"

#include <csignal>

int main() {
    std::signal(SIGPIPE, SIG_IGN); // OpenSSL's socket BIO can write to a peer that already closed
    return goblin::test::run_all();
}
