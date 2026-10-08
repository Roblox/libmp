// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// std::unordered_map compatibility wrapper with optional ntd allocator routing.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <unordered_map>
#if NTD_ROUTED_STD_TYPES
#include "allocator.h"
#include "functional.h"
#endif
namespace ntd
{
#if NTD_ROUTED_STD_TYPES
template <
    class Key,
    class T,
    class Hash = ntd::hash<Key>,
    class KeyEqual = ntd::equal_to<Key>>
using unordered_map = std::unordered_map<
    Key, T, Hash, KeyEqual, allocator<std::pair<const Key, T>>>;
#else
using std::unordered_map;
#endif
} // namespace ntd
#endif
