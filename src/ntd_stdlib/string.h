// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// std::string compatibility wrapper with optional ntd allocator routing.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <string>
#if NTD_ROUTED_STD_TYPES
#include "allocator.h"
#endif
namespace ntd
{
#if NTD_ROUTED_STD_TYPES
using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;
#else
using std::string;
#endif
} // namespace ntd
#endif
