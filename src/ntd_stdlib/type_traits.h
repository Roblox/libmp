// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard type-trait compatibility wrapper.

#include "ntd_config.h"
#include <cstddef>

namespace ntd
{
using nullptr_t = decltype(nullptr);
}

#if NTD_OWN_STDLIB
// @amalgamate: config-begin NTD_OWN_STDLIB
// Reserved for the embedded STL-primitives implementation.
// @amalgamate: config-end NTD_OWN_STDLIB
#else
#include <type_traits>
namespace ntd
{
using std::conditional;
using std::conditional_t;
using std::decay;
using std::decay_t;
using std::declval;
using std::enable_if;
using std::enable_if_t;
using std::false_type;
using std::integral_constant;
using std::invoke_result_t;
using std::is_integral;
using std::is_integral_v;
using std::is_pointer;
using std::is_pointer_v;
using std::is_same;
using std::is_same_v;
using std::is_signed;
using std::is_signed_v;
using std::is_trivially_copyable_v;
using std::is_unsigned;
using std::is_unsigned_v;
using std::remove_const;
using std::remove_cv;
using std::remove_cv_t;
using std::remove_pointer;
using std::remove_pointer_t;
using std::remove_reference;
using std::remove_reference_t;
using std::true_type;
using std::underlying_type;
using std::underlying_type_t;
using std::void_t;
} // namespace ntd
#endif
