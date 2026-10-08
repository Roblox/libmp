// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "dependencies/support/config.h"
#include "gprx_container/xassert.h"
#include "mp_api/api_config.h"
#include "ntd_stdlib/ntd_config.h"

#ifndef FLATBUFFERS_ASSERT
    // FlatBuffers uses this for internal/precondition diagnostics. Buffer verification
    // reports malformed input through its return value and does not depend on assertions.
    #define FLATBUFFERS_ASSERT(x) XASSERT_DIAG(x, "")
#endif

#include "dependencies/support/warnings_push.h"
#if !CFG_USE_EXTERNAL_FLATBUFFERS
    #define flatbuffers LibMP_flatbuffers
#endif
#if NTD_TYPES_DISTINCT_FROM_STD && LIBMP_PUBLIC_NTD_API && CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
// Amalgamation expands the derived NTD_TYPES_DISTINCT_FROM_STD predicate below;
// the default-on routing policy remains for the surrounding #if to evaluate.
// @amalgamate: config-begin flatbuffers-ntd-compat (NTD_OWN_STDLIB || NTD_EMBEDDED_ALLOC || NTD_EXTERNAL_MALLOC) && LIBMP_PUBLIC_NTD_API
    #include "dependencies/support/flatbuffers_ntd_compat.h"
// @amalgamate: config-end flatbuffers-ntd-compat
#endif
// @amalgamate: dependency-begin flatbuffers CFG_USE_EXTERNAL_FLATBUFFERS dependencies/flatbuffers
#include <flatbuffers/base.h>
#include <flatbuffers/flatbuffers.h>
// @amalgamate: dependency-end flatbuffers
#if !CFG_USE_EXTERNAL_FLATBUFFERS
    #undef flatbuffers
#endif
#include "dependencies/support/warnings_pop.h"
