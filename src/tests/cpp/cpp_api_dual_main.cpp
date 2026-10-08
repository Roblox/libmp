// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Dual-flavor coexistence test -- driver. Links the std half (cpp_api_dual_ext.cpp) and the ntd
// half (cpp_api_dual_int.cpp) into one binary; if it links and runs, std + ntd cpp_api coexist.
#include "test_host_allocator.h"

#include <cstdio>

bool cpp_api_probe_std();
bool cpp_api_probe_ntd();

int main()
{
    test_install_host_allocator();
    const bool ok = cpp_api_probe_std() && cpp_api_probe_ntd();
    std::printf("cpp_api_dual_flavor: std + ntd cpp_api in one binary -> %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
