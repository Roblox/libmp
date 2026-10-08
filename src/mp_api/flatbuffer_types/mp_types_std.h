// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "dependencies/support/config.h"
#include "mp_api/api_config.h"
#include "mp_api/flatbuffer_types/flatbuffers_core.h"
#include "ntd_stdlib/ntd_config.h"

// The std schema is needed when ntd types equal std, or as the public half of a
// distinct-ntd/public-std dual-flavor build.
#if !NTD_TYPES_DISTINCT_FROM_STD || !LIBMP_PUBLIC_NTD_API
// Amalgamation expands the derived NTD_TYPES_DISTINCT_FROM_STD predicate below.
// @amalgamate: config-begin std-api-flavor (!NTD_OWN_STDLIB && !NTD_EMBEDDED_ALLOC && !NTD_EXTERNAL_MALLOC) || !LIBMP_PUBLIC_NTD_API

#include "dependencies/support/warnings_push.h"
#if !CFG_USE_EXTERNAL_FLATBUFFERS
    #define flatbuffers LibMP_flatbuffers
#endif
#define FBSTD std
#define FBTYPES MpTypesStd
    #include "mp_api/flatbuffer_types/mp_types_generated.inc"
#undef FBTYPES
#undef FBSTD
#if !CFG_USE_EXTERNAL_FLATBUFFERS
    #undef flatbuffers
#endif
#include "dependencies/support/warnings_pop.h"

namespace LibMP
{
#if CFG_USE_EXTERNAL_FLATBUFFERS
    namespace flatbuffers = ::flatbuffers;
#else
    namespace flatbuffers = ::LibMP_flatbuffers;
#endif
}

// @amalgamate: config-end std-api-flavor
#endif // !NTD_TYPES_DISTINCT_FROM_STD || !LIBMP_PUBLIC_NTD_API
