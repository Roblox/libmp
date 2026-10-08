// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Verifies one-shot runtime disabling of the embedded allocator's large-region cache.

#include <cstdio>

#include "ntd_stdlib/allocator.h"
#include "test_host_allocator.h"

int main()
{
    test_install_host_allocator();

    ntd::mem::LargeCacheConfig config;
    config.minimum_bytes = 0;
    config.maximum_bytes = 0;
    ntd::mem::SetLargeCacheConfig(config);

    const size_t reserved_before = ntd::mem::ReservedBytes();
    void* allocation = ntd::mem::Alloc(100000);
    if (!allocation || ntd::mem::ReservedBytes() <= reserved_before)
        return 1;
    if (ntd::mem::ActiveCapacityBytes() < 100000 || ntd::mem::CachedBytes() != 0)
        return 1;

    ntd::mem::Free(allocation);
    if (ntd::mem::BytesInUse() != 0 || ntd::mem::CachedBytes() != 0 || ntd::mem::ReservedBytes() != reserved_before)
        return 1;

    std::printf("NTD DISABLED LARGE CACHE TEST OK\n");
    return 0;
}
