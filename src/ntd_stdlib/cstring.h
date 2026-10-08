// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard byte/string operation compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <cctype>
#include <cstring>
#include <string.h>
namespace ntd
{
using std::memcmp;
using std::memcpy;
using std::memmove;
using std::memset;
using std::strlen;
using std::tolower;
using ::strnlen;
} // namespace ntd
#endif
