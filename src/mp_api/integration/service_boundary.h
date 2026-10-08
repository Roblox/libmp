// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "mp_api/control_channel.h"
#include "data_range.h"

// Validation helpers for the values accepted by MicroProfilerService.  They are kept
// free of lua_State and engine types so the engine bridge, native regression tests, and
// fuzz targets can share exactly the same narrowing, capacity, and overlap policy.

namespace LibMP
{

struct ValidatedBufferWindow
{
    mpsize_t offset = 0;
    mpsize_t size = 0;
};

inline bool ValidateBufferWindow(
    int64_t offset, int64_t size, uint64_t capacity, ValidatedBufferWindow& out)
{
    if (offset < 0 || size < 0 ||
        static_cast<uint64_t>(offset) > GPRX::max_mpsize_t ||
        static_cast<uint64_t>(size) > GPRX::max_mpsize_t ||
        !GPRX::range_within(
            static_cast<uint64_t>(offset), static_cast<uint64_t>(size), capacity))
        return false;

    out.offset = static_cast<mpsize_t>(offset);
    out.size = static_cast<mpsize_t>(size);
    return true;
}

inline bool BufferWindowsOverlap(
    const ValidatedBufferWindow& first, const ValidatedBufferWindow& second)
{
    if (first.size == 0 || second.size == 0)
        return false;
    const uint64_t first_end = static_cast<uint64_t>(first.offset) + first.size;
    const uint64_t second_end = static_cast<uint64_t>(second.offset) + second.size;
    return first.offset < second_end && second.offset < first_end;
}

struct ValidatedControlCommandArguments
{
    ValidatedBufferWindow command;
    ValidatedBufferWindow response;
};

inline bool ValidateControlCommandArguments(
    int64_t command_offset,
    int64_t command_size,
    uint64_t command_capacity,
    int64_t response_offset,
    int64_t response_size,
    uint64_t response_capacity,
    bool same_buffer,
    ValidatedControlCommandArguments& out)
{
    if (!ValidateBufferWindow(
            command_offset, command_size, command_capacity, out.command) ||
        !ValidateBufferWindow(
            response_offset, response_size, response_capacity, out.response))
        return false;
    return !same_buffer || !BufferWindowsOverlap(out.command, out.response);
}

inline uint32_t ProcessControlCommand(
    ControlChannel& channel,
    const uint8_t* command_buffer,
    uint64_t command_capacity,
    int64_t command_offset,
    int64_t command_size,
    uint8_t* response_buffer,
    uint64_t response_capacity,
    int64_t response_offset,
    int64_t response_size)
{
    if (!command_buffer || !response_buffer)
        return 0;

    ValidatedControlCommandArguments args;
    if (!ValidateControlCommandArguments(
            command_offset, command_size, command_capacity,
            response_offset, response_size, response_capacity,
            command_buffer == response_buffer, args))
        return 0;

    return channel.OnCommand(
        command_buffer + args.command.offset, args.command.size,
        response_buffer + args.response.offset, args.response.size);
}

inline bool ValidateDataSizeArgument(int64_t slot_id, uint32_t& out_slot_id)
{
    return TryConvertSlotId(slot_id, out_slot_id);
}

inline bool TryConvertDataSizeToServiceResult(uint64_t size, int32_t& out_size)
{
    constexpr uint64_t max_service_result = 0x7FFFFFFFull;
    if (size > max_service_result)
        return false;
    out_size = static_cast<int32_t>(size);
    return true;
}

} // namespace LibMP
