// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard numeric-limits compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <limits>
namespace ntd
{
using std::numeric_limits;
} // namespace ntd
#endif
