// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "flatbuffer_types/mp_types.h"
#include "gprx_container/types.h"

struct MpSession;

namespace GPRX
{
class ContainerContext;
class IncrementalContainerWriter;
class IStreamProcessor;
struct StreamFinalizerOptions;
}

namespace LibMP
{

// Shared implementation used by the native C++ conveniences and the WASM-facing C API.
bool ConvertSessionToStream(
    MpSession* session,
    TypesNtd::ContainerFormat format,
    GPRX::IStreamProcessor& output);

// Serializes a native container using the same encodings and HTML finalizer as
// ConvertSessionToStream. This is intended for capture producers that already
// own a ContainerContext and do not need to parse their data back into a
// session first.
bool ConvertContainerToStream(
    GPRX::ContainerContext& container,
    TypesNtd::ContainerFormat format,
    GPRX::IStreamProcessor& output);

// Starts/finishes the incremental writer with the same HTML envelope used by
// ConvertContainerToStream. Append containers through the GPRX writer between
// these calls.
bool BeginIncrementalHtmlStream(
    GPRX::IncrementalContainerWriter& writer,
    GPRX::IStreamProcessor& output);
bool FinishIncrementalHtmlStream(GPRX::IncrementalContainerWriter& writer);

} // namespace LibMP

#ifndef ENABLE_EXTERNAL_TOOLS_OUTPUT
#ifdef LIBMP_MODULE_BUILD
    #define ENABLE_EXTERNAL_TOOLS_OUTPUT 1
#else
    #define ENABLE_EXTERNAL_TOOLS_OUTPUT 0
#endif
#endif

extern "C"
{

// Called synchronously with each converted-output chunk. The data is valid only
// for the duration of the call. Return nonzero to accept it or zero to abort.
uint32_t LibMpExtToolOutput(const uint8_t* data, mpsize_t size);

}
