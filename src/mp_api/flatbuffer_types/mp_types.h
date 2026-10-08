// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "mp_api/api_config.h"
#include "mp_api/flatbuffer_types/flatbuffers_core.h"
#include "ntd_stdlib/ntd_config.h"

// Include both wrappers deliberately; each wrapper selects only the flavor needed
// by this configuration:
//   ntd types equal std                 -> std schema (ntd aliases it)
//   distinct ntd + public std API       -> std and ntd schemas
//   distinct ntd + public ntd API       -> ntd schema
// "Distinct ntd" means allocator-routed standard primitives or the reserved
// embedded mode. The dual case lets the public default-allocator std boundary
// coexist with LibMP's internal ntd type family.
#include "mp_api/flatbuffer_types/mp_types_std.h"
#include "mp_api/flatbuffer_types/mp_types_ntd.h"

#if LIBMP_PUBLIC_NTD_API || defined(LIBMP_TYPES_INTERNAL)
    #ifndef LIBMP_TYPES_INTERNAL
        namespace LibMP { namespace Types = TypesNtd; }
    #endif
#else
    #ifndef LIBMP_TYPES_INTERNAL
        namespace LibMP { namespace Types = ::MpTypesStd; }
    #endif
#endif

#ifndef LIBMP_TYPES_INTERNAL
namespace LibMP
{
// Public enum spellings mirror language wrappers without exposing a second set of numeric values.
namespace Enums
{
namespace ContainerFormat
{
using Type = Types::ContainerFormat;
static constexpr Type Raw = Types::ContainerFormat_Raw;
static constexpr Type Compressed = Types::ContainerFormat_Compressed;
static constexpr Type CompressedAscii = Types::ContainerFormat_CompressedAscii;
static constexpr Type CompressedHtml = Types::ContainerFormat_CompressedHtml;
} // namespace ContainerFormat

namespace ContentMode
{
using Type = Types::ContentMode;
static constexpr Type Full = Types::ContentMode_Full;
static constexpr Type CountersOnly = Types::ContentMode_CountersOnly;
} // namespace ContentMode

namespace DataSlot
{
static constexpr uint32_t Live = Types::DataSlot_Live;
static constexpr uint32_t FirstUserManaged = Types::DataSlot_FirstUserManaged;
static constexpr uint32_t LastUserManaged = Types::DataSlot_LastUserManaged;
static constexpr uint32_t Temporary = Types::DataSlot_Temporary;
} // namespace DataSlot

namespace AppType
{
using Type = Types::AppType;
static constexpr Type Unknown = Types::AppType_Unknown;
static constexpr Type Client = Types::AppType_Client;
static constexpr Type Studio = Types::AppType_Studio;
static constexpr Type Server = Types::AppType_Server;
} // namespace AppType
} // namespace Enums
} // namespace LibMP
#endif
