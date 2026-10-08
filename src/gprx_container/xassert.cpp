// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "xassert.h"

namespace GPRX
{
namespace detail
{
namespace
{

DiagnosticAssertionHandler diagnostic_assertion_handler = nullptr;

} // namespace

DiagnosticAssertionHandler SetDiagnosticAssertionHandler(
    DiagnosticAssertionHandler handler) noexcept
{
    DiagnosticAssertionHandler previous =
        ntd::detail::AtomicLoadPointerAcquire(&diagnostic_assertion_handler);
    ntd::detail::AtomicStorePointerRelease(&diagnostic_assertion_handler, handler);
    return previous;
}

DiagnosticAssertionAction DispatchDiagnosticAssertion(
    const DiagnosticAssertionInfo& info) noexcept
{
    DiagnosticAssertionHandler handler =
        ntd::detail::AtomicLoadPointerAcquire(&diagnostic_assertion_handler);
    return handler ? handler(info) : DiagnosticAssertionAction::UseDefault;
}

} // detail
} // GPRX
