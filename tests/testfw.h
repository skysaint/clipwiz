// testfw.h — Minimal assertion framework for clipwiz_tests
//
// No third-party test library: clipwiz is a single offline exe with zero
// dependencies, and pulling gtest in just to check pure functions would break
// that invariant for no gain. Three macros are enough:
//
//   CWZ_CHECK(expr)        — boolean assertion
//   CWZ_CHECK_EQ(lhs, rhs) — equality assertion, prints both values on failure
//   CWZ_RUN(fn)            — run fn() as one named case (name = fn)
//
// Header-only, `inline` variables so every translation unit shares one counter.
#pragma once

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "util.h"

namespace testfw {

inline int g_passed = 0;
inline int g_failed = 0;
inline int g_failedAtCaseStart = 0;
inline const char* g_case = "";

// Render a value for failure output. One template with `if constexpr` rather
// than an overload set: overloads on integral types go ambiguous the moment a
// caller compares e.g. size_t against uint32_t, and /WX turns that into an
// error.
template <typename T>
std::string Desc(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "\"" + v + "\"";
    } else if constexpr (std::is_same_v<T, std::wstring>) {
        return "L\"" + util::Narrow(v) + "\"";
    } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "<%u bytes>", static_cast<unsigned>(v.size()));
        return buf;
    } else if constexpr (std::is_enum_v<T>) {
        return Desc(static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<T>) {
        char buf[32];
        if constexpr (std::is_signed_v<T>) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        } else {
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        }
        return buf;
    } else {
        return "<value>";
    }
}

inline void BeginCase(const char* name) {
    g_case = name;
    g_failedAtCaseStart = g_failed;
}

inline void EndCase() {
    if (g_failed == g_failedAtCaseStart) {
        std::printf("[ OK ] %s\n", g_case);
    } else {
        std::printf("[FAIL] %s\n", g_case);
    }
}

inline void Fail(const char* file, int line, const std::string& detail) {
    ++g_failed;
    std::printf("       %s:%d\n         %s\n", file, line, detail.c_str());
}

inline int Summary() {
    std::printf("\n%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace testfw

#define CWZ_CHECK(expr)                                                            \
    do {                                                                           \
        if (expr) {                                                                \
            ++testfw::g_passed;                                                    \
        } else {                                                                   \
            testfw::Fail(__FILE__, __LINE__, std::string("CHECK failed: ") + #expr); \
        }                                                                          \
    } while (false)

#define CWZ_CHECK_EQ(lhs, rhs)                                                     \
    do {                                                                           \
        const auto& cwzLhs = (lhs);                                                \
        const auto& cwzRhs = (rhs);                                                \
        if (cwzLhs == cwzRhs) {                                                    \
            ++testfw::g_passed;                                                    \
        } else {                                                                   \
            testfw::Fail(__FILE__, __LINE__,                                       \
                         std::string(#lhs " == " #rhs "\n           lhs=") +        \
                             testfw::Desc(cwzLhs) + "\n           rhs=" +          \
                             testfw::Desc(cwzRhs));                                \
        }                                                                          \
    } while (false)

#define CWZ_RUN(fn)                                                                \
    do {                                                                           \
        testfw::BeginCase(#fn);                                                    \
        fn();                                                                      \
        testfw::EndCase();                                                         \
    } while (false)
