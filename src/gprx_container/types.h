// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "ntd_stdlib/limits.h"
#include <cstdint>

// Global namespace

using mpsize_t = uint32_t;
using mpssize_t = int32_t;
using mpdatasz_t = uint64_t;
using mphash_t = uint32_t;

namespace GPRX
{

constexpr mpsize_t max_mpsize_t = ntd::numeric_limits<mpsize_t>::max();
constexpr mpssize_t max_mpssize_t = ntd::numeric_limits<mpssize_t>::max();
constexpr mpdatasz_t max_mpdatasz_t = ntd::numeric_limits<mpdatasz_t>::max();
constexpr uint16_t max_u16 = ntd::numeric_limits<uint16_t>::max();
constexpr uint32_t max_u24 = 0x00FFFFFF;
constexpr uint32_t max_u32 = ntd::numeric_limits<uint32_t>::max();
constexpr uint64_t max_u64 = ntd::numeric_limits<uint64_t>::max();

constexpr mpsize_t kDataAlignment = sizeof(uint64_t);

union U64Split
{
    uint64_t entry;
    struct
    {
        uint32_t low;
        uint32_t high;
    }
    parts;
};

} // GPRX
