// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "ntd_stdlib/compiler.h"

namespace GPRX
{
namespace detail
{

enum class DiagnosticAssertionAction
{
    UseDefault,
    Continue,
};

struct DiagnosticAssertionInfo
{
    const char* expression;
    const char* message;
    const char* file;
    int line;
};

// The handler is process-wide. Tests that replace it must serialize the replacement and
// restore the previous handler. Returning Continue is only safe when the assertion site
// has an explicit fallback for the failed condition; unexpected diagnostics should return
// UseDefault so the configured assert/trap behavior still runs.
using DiagnosticAssertionHandler = DiagnosticAssertionAction (*)(
    const DiagnosticAssertionInfo& info) noexcept;

DiagnosticAssertionHandler SetDiagnosticAssertionHandler(
    DiagnosticAssertionHandler handler) noexcept;
DiagnosticAssertionAction DispatchDiagnosticAssertion(
    const DiagnosticAssertionInfo& info) noexcept;

#if defined(GPRX_NO_ASSERT) || (!defined(GPRX_TRAP_ASSERT) && defined(NDEBUG))
    #define GPRX_DIAGNOSTIC_ASSERTIONS_ARMED 0
#else
    #define GPRX_DIAGNOSTIC_ASSERTIONS_ARMED 1
#endif
constexpr bool DiagnosticAssertionsArmed = GPRX_DIAGNOSTIC_ASSERTIONS_ARMED != 0;

} // detail
} // GPRX

// Diagnostic assertions may be disabled; callers must remain safe without them.
#if !GPRX_DIAGNOSTIC_ASSERTIONS_ARMED
    #define XASSERT_DIAG(expr, ...) \
        do {} while (0)
#else
    #if !defined(GPRX_TRAP_ASSERT)
        #include <cassert>
        #define GPRX_DIAGNOSTIC_ASSERT_DEFAULT(condition, ...) \
            assert((condition) && ("" __VA_ARGS__))
    #else
        #define GPRX_DIAGNOSTIC_ASSERT_DEFAULT(condition, ...) \
            ::ntd::detail::Trap()
    #endif

    #define XASSERT_DIAG(expr, ...) \
        do { \
            const bool gprx_diagnostic_assert_condition = static_cast<bool>(expr); \
            if (!gprx_diagnostic_assert_condition && \
                ::GPRX::detail::DispatchDiagnosticAssertion({ \
                    #expr, "" __VA_ARGS__, __FILE__, __LINE__ \
                }) != ::GPRX::detail::DiagnosticAssertionAction::Continue) \
            { \
                GPRX_DIAGNOSTIC_ASSERT_DEFAULT( \
                    gprx_diagnostic_assert_condition, __VA_ARGS__); \
            } \
        } while (0)
#endif

// Trap assertions enforce non-recoverable runtime requirements in every build.
#define XASSERT_TRAP(expr, ...) \
    do { if (!(expr)) ::ntd::detail::Trap(); } while (0)
