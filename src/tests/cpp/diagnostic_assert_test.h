// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/xassert.h"

#include <cstdint>
#include <cstring>

namespace libmp_test
{

// Installs the process-wide diagnostic handler for one exact, expected assertion.
// Unexpected diagnostics retain the assertion policy configured for the build.
class ScopedExpectedDiagnostic
{
    const char* m_message;
    uint32_t m_hits = 0;
    ScopedExpectedDiagnostic* m_parent;
    GPRX::detail::DiagnosticAssertionHandler m_previous_handler = nullptr;

    static ScopedExpectedDiagnostic*& Active() noexcept
    {
        static ScopedExpectedDiagnostic* active = nullptr;
        return active;
    }

    static GPRX::detail::DiagnosticAssertionAction Handle(
        const GPRX::detail::DiagnosticAssertionInfo& info) noexcept
    {
        for (ScopedExpectedDiagnostic* expected = Active(); expected;
             expected = expected->m_parent)
        {
            if (info.message && std::strcmp(info.message, expected->m_message) == 0)
            {
                ++expected->m_hits;
                return GPRX::detail::DiagnosticAssertionAction::Continue;
            }
        }

        ScopedExpectedDiagnostic* outermost = Active();
        while (outermost && outermost->m_parent)
            outermost = outermost->m_parent;
        return outermost && outermost->m_previous_handler
            ? outermost->m_previous_handler(info)
            : GPRX::detail::DiagnosticAssertionAction::UseDefault;
    }

public:
    explicit ScopedExpectedDiagnostic(const char* message) noexcept
        : m_message(message)
        , m_parent(Active())
    {
        if (!m_parent)
            m_previous_handler = GPRX::detail::SetDiagnosticAssertionHandler(Handle);
        Active() = this;
    }

    ~ScopedExpectedDiagnostic()
    {
        Active() = m_parent;
        if (!m_parent)
            GPRX::detail::SetDiagnosticAssertionHandler(m_previous_handler);
    }

    ScopedExpectedDiagnostic(const ScopedExpectedDiagnostic&) = delete;
    ScopedExpectedDiagnostic& operator=(const ScopedExpectedDiagnostic&) = delete;

    uint32_t Hits() const noexcept
    {
        return m_hits;
    }
};

inline uint32_t ExpectedDiagnosticHits(uint32_t armed_hits) noexcept
{
    return GPRX::detail::DiagnosticAssertionsArmed ? armed_hits : 0;
}

} // namespace libmp_test
