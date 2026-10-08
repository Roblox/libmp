// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Include the Win32 API without leaving NTD-introduced configuration macros set.

#if !defined(_WIN32)
    #error "windows_api.h requires Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #define NTD_WINDOWS_API_UNDEF_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
    #define NOMINMAX
    #define NTD_WINDOWS_API_UNDEF_NOMINMAX
#endif

#include <windows.h>

#ifdef NTD_WINDOWS_API_UNDEF_NOMINMAX
    #undef NOMINMAX
    #undef NTD_WINDOWS_API_UNDEF_NOMINMAX
#endif

#ifdef NTD_WINDOWS_API_UNDEF_LEAN_AND_MEAN
    #undef WIN32_LEAN_AND_MEAN
    #undef NTD_WINDOWS_API_UNDEF_LEAN_AND_MEAN
#endif
