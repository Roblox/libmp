// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// std::vector compatibility wrapper with optional ntd allocator routing.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <vector>
#if NTD_ROUTED_STD_TYPES
#include "allocator.h"
#endif
namespace ntd
{
#if NTD_ROUTED_STD_TYPES
template <class T>
using vector = std::vector<T, allocator<T>>;
#else
using std::vector;
#endif
} // namespace ntd
#endif
