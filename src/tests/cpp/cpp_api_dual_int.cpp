// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Dual-flavor coexistence test -- INTERNAL half: the ntd flavor via the internal header.
// Linked into the same binary as cpp_api_dual_ext.cpp (std flavor).
#include "mp_api/cpp_api_internal.h"
#include <type_traits>
#include <utility>

#if NTD_TYPES_DISTINCT_FROM_STD
// ntd is a distinct family here; the internal boundary is the ntd flavor.
static_assert(std::is_same<LibMP::DefaultFlavor, LibMP::NtdFlavor>::value, "internal cpp_api must be the ntd flavor");
static_assert(std::is_same<decltype(std::declval<const LibMP::Session&>().FetchGroupDesc(0u)),
                           ntd::optional<MpTypesNtd::GroupDescT>>::value,
              "internal Fetch* must return ntd types");
    #if !LIBMP_PUBLIC_NTD_API
// External boundary is std here (the genuine coexistence case), so StdFlavor also exists and must
// be distinct. Under LIBMP_PUBLIC_NTD_API the boundary is ntd and StdFlavor isn't defined at all.
static_assert(!std::is_same<LibMP::NtdFlavor, LibMP::StdFlavor>::value,
              "the allocator-distinct cpp_api flavors must not collapse");
    #endif
#else
// ntd types equal std types: the flavors collapse to one family.
static_assert(std::is_same<LibMP::DefaultFlavor, LibMP::StdFlavor>::value,
              "equivalent ntd/std types must collapse to one cpp_api flavor");
#endif

bool cpp_api_probe_ntd()
{
    auto s = LibMP::Session::Create(nullptr);  // odr-uses SessionT<NtdFlavor>::Create
    auto fp = &LibMP::Session::FetchGroupDesc; // odr-uses SessionT<NtdFlavor>::FetchGroupDesc -> must link
    auto tracks = &LibMP::Session::FetchTracks;
    auto tool = &LibMP::Tools::ConvertToBuffer<LibMP::DefaultFlavor>;
    auto capture = static_cast<bool (LibMP::Control::*)(uint32_t, uint32_t, LibMP::TypesNtd::ContainerFormat)>(
        &LibMP::Control::CaptureToSlot);
    auto startFileStream = &LibMP::Experimental::V1::StartFileStream;
    auto startMemoryStream = &LibMP::Experimental::V1::StartMemoryStream;
    auto getStreamStatus = &LibMP::Experimental::V1::GetStreamStatus;
    auto setContentMode = static_cast<bool (*)(LibMP::TypesNtd::ContentMode)>(
        &LibMP::Experimental::V1::SetContentMode);
    return !s && fp != nullptr && tracks != nullptr && tool != nullptr && capture != nullptr &&
        startFileStream != nullptr && startMemoryStream != nullptr && getStreamStatus != nullptr && setContentMode != nullptr;
}
