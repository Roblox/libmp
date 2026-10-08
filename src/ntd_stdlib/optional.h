// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard optional compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <optional>
namespace ntd
{
using std::nullopt;
using std::nullopt_t;
using std::optional;
} // namespace ntd
#endif
