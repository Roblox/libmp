// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "gprx_container/container_context.h"
#include "gprx_container/lz4frame_wrapper.h"
#include "mp_api/control_channel.h"
#include "mp_api/c_api_internal.h"

#ifdef LIBMP_MODULE_BUILD
#include <malloc.h>
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
void MpInit()
{
    static bool is_init = false;
    if (is_init)
        return;

    is_init = true;

    GPRX::ContainerContext::SetDataFormatMeta(
        LibMP::DATA_FORMAT_SIGNATURE,
        static_cast<uint8_t>(LibMP::Version::GetMajor(LibMP::LocalVersion::DATA_FORMAT)),
        static_cast<uint8_t>(LibMP::Version::GetMinor(LibMP::LocalVersion::DATA_FORMAT))
    );

    GPRX::lz4::InitEmbeddedLz4();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t MpGetMemUsed()
{
#if NTD_EMBEDDED_ALLOC
    return static_cast<uint32_t>(ntd::mem::ActiveCapacityBytes());
#elif defined(LIBMP_MODULE_BUILD)
    struct mallinfo info = mallinfo();
    return static_cast<uint32_t>(info.uordblks);
#else
    return 0;
#endif
}

uint32_t MpGetMemReserved()
{
#if NTD_EMBEDDED_ALLOC
    return static_cast<uint32_t>(ntd::mem::ReservedBytes());
#elif defined(LIBMP_MODULE_BUILD)
    struct mallinfo info = mallinfo();
    return static_cast<uint32_t>(info.arena + info.hblkhd); // total obtained from the system
#else
    return 0;
#endif
}
