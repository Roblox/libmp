// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "test_harness.h"

#include "mp_api/integration/service_boundary.h"

#include <cstdint>
#include <limits>

TEST_CASE(buffer_window_validation)
{
    LibMP::ValidatedBufferWindow window;
    CHECK(LibMP::ValidateBufferWindow(0, 0, 0, window));
    CHECK(LibMP::ValidateBufferWindow(4, 12, 16, window));
    CHECK_EQ(window.offset, 4u);
    CHECK_EQ(window.size, 12u);
    CHECK(!LibMP::ValidateBufferWindow(-1, 1, 16, window));
    CHECK(!LibMP::ValidateBufferWindow(0, -1, 16, window));
    CHECK(!LibMP::ValidateBufferWindow(9, 8, 16, window));
    CHECK(!LibMP::ValidateBufferWindow(
        static_cast<int64_t>(GPRX::max_mpsize_t) + 1, 0,
        std::numeric_limits<uint64_t>::max(), window));
}

TEST_CASE(control_windows_reject_overlap_without_wrapping)
{
    LibMP::ValidatedControlCommandArguments args;
    CHECK(LibMP::ValidateControlCommandArguments(0, 32, 64, 32, 32, 64, true, args));
    CHECK(!LibMP::ValidateControlCommandArguments(0, 33, 64, 32, 32, 64, true, args));
    CHECK(LibMP::ValidateControlCommandArguments(0, 33, 64, 32, 32, 64, false, args));
    CHECK(LibMP::ValidateControlCommandArguments(7, 0, 8, 7, 1, 8, true, args));
    CHECK(!LibMP::ValidateControlCommandArguments(0, 1, 0, 0, 0, 0, false, args));
    CHECK(!LibMP::ValidateControlCommandArguments(
        GPRX::max_mpsize_t, 2, GPRX::max_mpsize_t,
        0, 0, 0, false, args));
}

TEST_CASE(control_wrapper_rejects_before_dispatch)
{
    LibMP::ControlChannel channel;
    alignas(8) uint8_t shared[128] = {};

    // Same-buffer overlap, null bases, and out-of-capacity windows are rejected by
    // the reusable boundary before ControlChannel sees any pointer arithmetic.
    CHECK_EQ(LibMP::ProcessControlCommand(
        channel, shared, sizeof(shared), 0, 64,
        shared, sizeof(shared), 32, 64), 0u);
    CHECK_EQ(LibMP::ProcessControlCommand(
        channel, nullptr, 0, 0, 0,
        shared, sizeof(shared), 0, 32), 0u);
    CHECK_EQ(LibMP::ProcessControlCommand(
        channel, shared, sizeof(shared), 120, 16,
        shared, sizeof(shared), 0, 32), 0u);
}

TEST_CASE(data_size_slot_validation)
{
    uint32_t slot = 99;
    CHECK(LibMP::ValidateDataSizeArgument(0, slot));
    CHECK_EQ(slot, 0u);
    CHECK(LibMP::ValidateDataSizeArgument(GPRX::max_u32, slot));
    CHECK_EQ(slot, GPRX::max_u32);
    CHECK(!LibMP::ValidateDataSizeArgument(-1, slot));
    CHECK(!LibMP::ValidateDataSizeArgument(
        static_cast<int64_t>(GPRX::max_u32) + 1, slot));

    int32_t result = -1;
    CHECK(LibMP::TryConvertDataSizeToServiceResult(0x7FFFFFFFull, result));
    CHECK_EQ(result, 0x7FFFFFFF);
    CHECK(!LibMP::TryConvertDataSizeToServiceResult(
        0x80000000ull, result));
}

LIBMP_TEST_MAIN("service_boundary")
