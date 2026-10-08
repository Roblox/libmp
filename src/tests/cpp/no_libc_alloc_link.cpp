// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Whole-archive link test for direct libc allocation references.
#include "libmp_test_api.h"

// CMake force-loads the complete archive and leaves the direct libc allocation
// API wrappers undefined. Merely producing this executable is the test.
int main()
{
    MpInit();
    return 0;
}
