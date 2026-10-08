// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// test_harness.h -- minimal, dependency-free harness shared by LibMP's C++ tests.
//
// CHECK records checks and failures while allowing execution to continue. TEST_CASE
// registers cases automatically, and LIBMP_TEST_MAIN supplies the entry point. The
// harness avoids heap allocation and standard containers and includes only
// <cstdio> and <cstdint>.
#pragma once

#include <cstdio>
#include <cstdint>

namespace libmp_test
{
using TestFn = void (*)();

struct Registry
{
    static constexpr int kMax = 512;
    const char* names[kMax] = {};
    TestFn fns[kMax] = {};
    int count = 0;
};

inline Registry& registry() { static Registry r; return r; }
inline int& checks() { static int n = 0; return n; }
inline int& failures() { static int n = 0; return n; }
inline const char*& current_case() { static const char* s = "?"; return s; }

inline int register_test(const char* name, TestFn fn)
{
    Registry& r = registry();
    if (r.count < Registry::kMax)
    {
        r.names[r.count] = name;
        r.fns[r.count] = fn;
        ++r.count;
    }
    return 0;
}

inline int run_all(const char* suite)
{
    Registry& r = registry();
    std::printf("[%s] running %d test case(s)\n", suite, r.count);
    for (int i = 0; i < r.count; ++i)
    {
        current_case() = r.names[i];
        r.fns[i]();
    }
    std::printf("[%s] %d checks, %d failure(s)\n", suite, checks(), failures());
    return failures() == 0 ? 0 : 1;
}
} // namespace libmp_test

// Records a failure (with file:line and the current case name) but keeps running so a
// single test file reports every problem in one pass.
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++::libmp_test::checks();                                                          \
        if (!(cond))                                                                       \
        {                                                                                  \
            ++::libmp_test::failures();                                                    \
            std::printf("  FAIL %s:%d  [%s]  %s\n", __FILE__, __LINE__,                    \
                ::libmp_test::current_case(), #cond);                                      \
        }                                                                                  \
    } while (0)

// Integer/equality check that prints both operands on mismatch (as signed 64-bit).
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++::libmp_test::checks();                                                          \
        const long long _va = (long long)(a);                                             \
        const long long _vb = (long long)(b);                                             \
        if (_va != _vb)                                                                    \
        {                                                                                  \
            ++::libmp_test::failures();                                                    \
            std::printf("  FAIL %s:%d  [%s]  %s (%lld) != %s (%lld)\n", __FILE__,          \
                __LINE__, ::libmp_test::current_case(), #a, _va, #b, _vb);                 \
        }                                                                                  \
    } while (0)

// Defines and auto-registers a test case. Usage: TEST_CASE(my_case) { CHECK(...); }
#define TEST_CASE(name)                                                                    \
    static void name();                                                                    \
    static const int name##_registered = ::libmp_test::register_test(#name, &name);        \
    static void name()

#define LIBMP_TEST_MAIN(suite)                                                             \
    int main() { return ::libmp_test::run_all(suite); }
