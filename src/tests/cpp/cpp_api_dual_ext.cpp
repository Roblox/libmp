// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Dual-flavor coexistence test -- EXTERNAL half: cpp_api via the public header.
// Compiled into the same binary as cpp_api_dual_int.cpp (the internal/ntd half). The genuine
// "Two distinct flavors in one binary" occurs whenever ntd types differ from
// default-allocator std and the public API remains std: the internal half uses
// ntd while this public half uses std. Otherwise both halves collapse to one flavor.
#include "libmp_test_api.h"
#include <type_traits>
#include <utility>

#if CFG_USE_EXTERNAL_FLATBUFFERS
static_assert(std::is_same<LibMP::flatbuffers::String, ::flatbuffers::String>::value,
              "external mode must reuse the host's global FlatBuffers namespace");
#endif

#if LIBMP_PUBLIC_NTD_API
// This build publishes the ntd flavor at the boundary (there is no std consumer to coexist with).
static_assert(std::is_same<LibMP::DefaultFlavor, LibMP::NtdFlavor>::value,
              "LIBMP_PUBLIC_NTD_API: the public boundary is the ntd flavor");
#else
// The public boundary is the std flavor -- the half that must coexist with the internal ntd one.
static_assert(std::is_same<LibMP::DefaultFlavor, LibMP::StdFlavor>::value, "external cpp_api must be the std flavor");
static_assert(std::is_same<decltype(std::declval<const LibMP::Session&>().FetchGroupDesc(0u)),
                           std::optional<MpTypesStd::GroupDescT>>::value,
              "external Fetch* must return std types");
#endif

static_assert(std::is_same<LibMP::Enums::ContainerFormat::Type, LibMP::Types::ContainerFormat>::value,
              "the public enum facade must preserve the generated enum type");
static_assert(std::is_same<LibMP::Enums::ContentMode::Type, LibMP::Types::ContentMode>::value,
              "the public enum facade must preserve the generated enum type");

bool cpp_api_probe_std()
{
    LibMP::Enums::ContainerFormat::Type format = LibMP::Enums::ContainerFormat::Raw;
    auto s = LibMP::Session::Create(nullptr);  // odr-uses SessionT<DefaultFlavor>::Create (safe: null in, null out)
    auto fp = &LibMP::Session::FetchGroupDesc; // odr-uses SessionT<DefaultFlavor>::FetchGroupDesc -> must link
    auto tracks = &LibMP::Session::FetchTracks;
    auto tool = &LibMP::Tools::ConvertToBuffer<LibMP::DefaultFlavor>;
    auto capture = static_cast<bool (LibMP::Control::*)(uint32_t, uint32_t, LibMP::Types::ContainerFormat)>(
        &LibMP::Control::CaptureToSlot);
    auto startFileStream = &LibMP::Experimental::V1::StartFileStream;
    auto startMemoryStream = &LibMP::Experimental::V1::StartMemoryStream;
    auto stopStream = &LibMP::Experimental::V1::StopStream;
    auto setStreamMarker = &LibMP::Experimental::V1::SetStreamMarker;
    auto getStreamStatus = &LibMP::Experimental::V1::GetStreamStatus;
    auto setContentMode = static_cast<bool (*)(LibMP::Types::ContentMode)>(
        &LibMP::Experimental::V1::SetContentMode);
    return format == LibMP::Enums::ContainerFormat::Raw &&
        !s && fp != nullptr && tracks != nullptr && tool != nullptr && capture != nullptr &&
        startFileStream != nullptr && startMemoryStream != nullptr && stopStream != nullptr && setStreamMarker != nullptr &&
        getStreamStatus != nullptr && setContentMode != nullptr;
}
