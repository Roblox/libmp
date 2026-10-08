// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard utility compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <utility>
namespace ntd
{
using std::forward;
using std::get;
using std::index_sequence;
using std::make_index_sequence;
using std::make_pair;
using std::move;
using std::pair;
using std::piecewise_construct;
using std::piecewise_construct_t;

namespace detail
{
using std::swap;

template <class T>
void AdlSwap(T& a, T& b) noexcept(noexcept(swap(a, b)))
{
    swap(a, b);
}

struct Swap
{
    template <class T>
    void operator()(T& a, T& b) const noexcept(noexcept(AdlSwap(a, b)))
    {
        AdlSwap(a, b);
    }
};
} // namespace detail

// Avoid making ntd::swap an ADL candidate for standard containers whose
// allocator is an ntd type.
inline constexpr detail::Swap swap{};
} // namespace ntd
#endif
