// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Select the container flavor exposed at the LibMP C++ API boundary. Internal
// API plumbing follows ntd independently of this setting.
#ifndef LIBMP_PUBLIC_NTD_API
    #define LIBMP_PUBLIC_NTD_API 0
#endif

// Omit FlatBuffers graph verification for trusted-only builds.
#ifndef LIBMP_TRUSTED_INPUTS_ONLY
    #define LIBMP_TRUSTED_INPUTS_ONLY 0
#endif
