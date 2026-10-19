#pragma once
// Minimal test framework for pse_tests.exe (no game needed). Usage:
//   PSE_TEST("hooks: chained call site") { ... PSE_CHECK(x == 1); PSE_CHECK_EQ(a, b); PSE_REQUIRE(p); }
// Run: pse_tests.exe [substring filter] - exit code = failed test count (capped at 255).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace pse_test {
struct Case {
    const char* name;
    void (*fn)();
    const char* file;
    int line;
};
inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}
struct Registrar {
    Registrar(const char* name, void (*fn)(), const char* file, int line) { Registry().push_back({name, fn, file, line}); }
};
inline int& Failures() {
    static int failures = 0;
    return failures;
}
struct RequireFailed {};
// Thrown by PSE_SKIP: the test can't run here (e.g. no game exe); printed as SKIP, not counted as a failure.
struct Skipped {
    std::string why;
};
inline void Fail(const char* file, int line, const std::string& what) {
    ++Failures();
    std::printf("    FAIL %s(%d): %s\n", file, line, what.c_str());
}
template <class T>
std::string Show(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) return "\"" + v + "\"";
    else if constexpr (std::is_convertible_v<T, const char*>) return v ? std::string("\"") + v + "\"" : "(null)";
    else if constexpr (std::is_pointer_v<T>) { char b[32]; std::snprintf(b, sizeof b, "%p", (const void*)v); return b; }
    else if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
    else if constexpr (std::is_integral_v<T>) {
        char b[64];
        std::snprintf(b, sizeof b, "%lld (0x%llX)", (long long)v, (unsigned long long)v);
        return b;
    } else if constexpr (std::is_floating_point_v<T>) return std::to_string(v);
    else return "?";
}
}  // namespace pse_test

#define PSE_TEST_CAT2(a, b) a##b
#define PSE_TEST_CAT(a, b) PSE_TEST_CAT2(a, b)
#define PSE_TEST(name)                                                                                        \
    static void PSE_TEST_CAT(pse_test_fn_, __LINE__)();                                                       \
    static ::pse_test::Registrar PSE_TEST_CAT(pse_test_reg_, __LINE__)(name, &PSE_TEST_CAT(pse_test_fn_, __LINE__), \
                                                                       __FILE__, __LINE__);                   \
    static void PSE_TEST_CAT(pse_test_fn_, __LINE__)()

#define PSE_CHECK(cond) \
    do { if (!(cond)) ::pse_test::Fail(__FILE__, __LINE__, #cond); } while (0)
#define PSE_REQUIRE(cond) \
    do { if (!(cond)) { ::pse_test::Fail(__FILE__, __LINE__, "REQUIRE " #cond); throw ::pse_test::RequireFailed{}; } } while (0)
#define PSE_CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                      \
        auto&& pse_a_ = (a); auto&& pse_b_ = (b);                                                             \
        if (!(pse_a_ == pse_b_))                                                                              \
            ::pse_test::Fail(__FILE__, __LINE__, std::string(#a " == " #b ": ") + ::pse_test::Show(pse_a_) +  \
                                                     " vs " + ::pse_test::Show(pse_b_));                      \
    } while (0)
#define PSE_CHECK_STR(a, b)                                                                                   \
    do {                                                                                                      \
        std::string pse_a_ = (a) ? std::string(a) : std::string("(null)");                                    \
        std::string pse_b_ = (b) ? std::string(b) : std::string("(null)");                                    \
        if (pse_a_ != pse_b_) ::pse_test::Fail(__FILE__, __LINE__, std::string(#a " == " #b ": \"") + pse_a_ + "\" vs \"" + pse_b_ + "\""); \
    } while (0)
// Skips the rest of the current test (reason printed). Checks that already failed still count.
#define PSE_SKIP(why) throw ::pse_test::Skipped{std::string(why)}
