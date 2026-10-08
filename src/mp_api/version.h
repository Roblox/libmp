// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/types.h"

namespace LibMP
{

#pragma pack(push, 1) // Disable padding

struct Version
{
    uint32_t data_format = 0;
    uint32_t control_channel = 0;
    uint32_t library = 0;
    uint32_t library_warning = 0;

    bool IsFilled()
    {
        return (data_format != 0);
    }

    bool MajorMatch(const Version& other)
    {
        return (
            GetMajor(data_format) == GetMajor(other.data_format) &&
            GetMajor(control_channel) == GetMajor(other.control_channel) &&
            GetMajor(library) == GetMajor(other.library)
        );
    }
    
    inline static constexpr uint16_t GetMajor(uint32_t v)
    {
        return static_cast<uint16_t>(v >> 16);
    }

    inline static constexpr uint16_t GetMinor(uint32_t v)
    {
        return static_cast<uint16_t>(v & GPRX::max_u16);
    }

    inline static constexpr uint32_t BuildFrom(uint16_t major, uint16_t minor)
    {
        return (static_cast<uint32_t>(major) << 16) | minor;
    }
};

#pragma pack(pop)

} // LibMP
