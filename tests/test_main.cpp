// The single main() for the goblin_tests executable. Test cases live in the other tests/*.cpp
// files and self-register via the TEST() macro (see mini_test.hpp).
#include "mini_test.hpp"

int main() {
    return goblin::test::run_all();
}
