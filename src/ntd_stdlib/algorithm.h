// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard algorithm compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <algorithm>
namespace ntd
{
using std::copy;
using std::fill;
using std::fill_n;
using std::lower_bound;
using std::max;
using std::min;
using std::reverse;
using std::sort;
using std::transform;
using std::upper_bound;
} // namespace ntd
#endif
