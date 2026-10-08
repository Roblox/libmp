// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard tuple compatibility wrapper.

#include "ntd_config.h"

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <tuple>
namespace ntd
{
using std::forward_as_tuple;
using std::get;
using std::make_tuple;
using std::tie;
using std::tuple;
using std::tuple_element;
using std::tuple_element_t;
using std::tuple_size;
} // namespace ntd
#endif
