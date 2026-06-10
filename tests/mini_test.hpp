// Tiny dependency-free test framework (self-registering cases + CHECK macros).
// A single tests/*.cpp provides main() via:  int main(){ return goblin::test::run_all(); }
#pragma once

#include <functional>
#include <print>
#include <string_view>
#include <vector>

namespace goblin::test {

struct Case {
    std::string_view name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(std::string_view name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all() {
    int passed = 0;
    for (const auto& c : registry()) {
        const int before = failures();
        c.fn();
        if (failures() == before) {
            ++passed;
            std::println("  ok   {}", c.name);
        } else {
            std::println("  FAIL {}", c.name);
        }
    }
    std::println("{}/{} cases passed", passed, registry().size());
    return failures() == 0 ? 0 : 1;
}

} // namespace goblin::test

#define GOBLIN_CAT_(a, b) a##b
#define GOBLIN_CAT(a, b) GOBLIN_CAT_(a, b)
#define TEST(name)                                                                      \
    static void GOBLIN_CAT(goblin_test_fn_, __LINE__)();                                \
    static ::goblin::test::Registrar GOBLIN_CAT(goblin_test_reg_, __LINE__){            \
        name, &GOBLIN_CAT(goblin_test_fn_, __LINE__)};                                  \
    static void GOBLIN_CAT(goblin_test_fn_, __LINE__)()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            ++::goblin::test::failures();                                               \
            std::println("    CHECK failed: {}   [{}:{}]", #cond, __FILE__, __LINE__);  \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        if (!((a) == (b))) {                                                            \
            ++::goblin::test::failures();                                               \
            std::println("    CHECK_EQ failed: {} == {}   [{}:{}]", #a, #b, __FILE__,   \
                         __LINE__);                                                     \
        }                                                                               \
    } while (0)
