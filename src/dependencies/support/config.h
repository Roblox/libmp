// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Third-party allocation routing policy.
#ifndef CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #define CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD 1
#endif

// Use the host project's <flatbuffers/flatbuffers.h> and ::flatbuffers namespace
// instead of LibMP's bundled, namespace-isolated copy.
#ifndef CFG_USE_EXTERNAL_FLATBUFFERS
    #define CFG_USE_EXTERNAL_FLATBUFFERS 0
#endif

#ifndef CFG_USE_EXTERNAL_LZ4
    #define CFG_USE_EXTERNAL_LZ4 0
#endif

#ifndef CFG_EXCLUDE_LZ4_ENCODER
    #define CFG_EXCLUDE_LZ4_ENCODER 0
#endif
