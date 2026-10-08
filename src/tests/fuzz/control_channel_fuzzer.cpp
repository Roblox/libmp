// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Fuzzes the complete MicroProfilerService-style command boundary: buffer offsets,
// capacities, aliasing/overlap, ControlChannel framing, schema-derived payload validation,
// and response placement. ASan/UBSan cover the parser while explicit guards cover writes.

#include "mp_api/integration/service_boundary.h"
#include "mp_api/flatbuffer_types/mp_types_ntd.h"
#include "gprx_container/block.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
bool g_callback_fired = false;
bool cb_bool(const LibMP::TypesNtd::MsgBool&, LibMP::TypesNtd::MsgBool& response)
{
    g_callback_fired = true;
    response.mutate_value(true);
    return true;
}
bool cb_i32(const LibMP::TypesNtd::MsgI32&, LibMP::TypesNtd::MsgBool& response)
{
    g_callback_fired = true;
    response.mutate_value(true);
    return true;
}

constexpr uint32_t kGuard = 32;
constexpr uint32_t kArea = 8192;
constexpr uint32_t kMaxCommand = 4096;
alignas(8) uint8_t g_first[kGuard + kArea + kGuard];
alignas(8) uint8_t g_second[kGuard + kArea + kGuard];
uint8_t g_before[kArea];
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 4)
        return 0;

    const bool same_buffer = (data[0] & 1) != 0;
    const uint32_t command_offset = data[1] & 15;
    const uint32_t response_offset = data[2] & 63;
    static const uint32_t response_sizes[] = {0, 1, 8, 31, 32, 33, 40, 64, 128, 512};
    const uint32_t response_size =
        response_sizes[data[3] % (sizeof(response_sizes) / sizeof(response_sizes[0]))];
    const size_t available = size - 4;
    const uint32_t command_size = static_cast<uint32_t>(
        available < kMaxCommand ? available : kMaxCommand);

    // Exercise the same table verifier used by schema-declared table commands.
    (void)LibMP::ControlDetail::GetVerifiedTable<
        LibMP::TypesNtd::CounterIteratorConfig>(data + 4, command_size);

    std::memset(g_first, 0xAB, sizeof(g_first));
    std::memset(g_second, 0xAB, sizeof(g_second));
    uint8_t* command_base = g_first + kGuard;
    uint8_t* response_base = same_buffer ? command_base : g_second + kGuard;
    std::memcpy(command_base + command_offset, data + 4, command_size);
    std::memcpy(g_before, response_base, kArea);

    LibMP::ControlChannel server;
    LibMP::ExternalControlIO io;
    io.OnEnableProfiler = cb_bool;
    io.OnEnableCapture = cb_bool;
    io.OnShowUI = cb_bool;
    io.OnSetFrameLimit = cb_i32;
    io.OnSetMemTargetMB = cb_i32;
    server.AssignIO(io);

    g_callback_fired = false;
    const uint32_t written = LibMP::ProcessControlCommand(
        server,
        command_base, kArea, command_offset, command_size,
        response_base, kArea, response_offset, response_size);

    if (written > response_size)
        __builtin_trap();
    if (written > 0)
    {
        const uint8_t* response = response_base + response_offset;
        if (command_size == 0)
        {
            if (written != sizeof(LibMP::HandshakeResp) || g_callback_fired)
                __builtin_trap();
        }
        else
        {
            if (written < sizeof(GPRX::BlockHeader))
                __builtin_trap();
            GPRX::BlockHeader request_header{};
            GPRX::BlockHeader response_header{};
            std::memcpy(&request_header, command_base + command_offset, sizeof(request_header));
            std::memcpy(&response_header, response, sizeof(response_header));
            if (response_header.metadata0 != GPRX::MagicFormatRawFull ||
                response_header.metadata1 != request_header.metadata1 ||
                response_header.type_id != GPRX::BlockType::MagicTmp ||
                response_header.type_meta != 1 ||
                response_header.encoding_method != GPRX::BlockPayloadEncodingMethod::Raw ||
                response_header.payload_size != written - sizeof(response_header) ||
                response_header.payload_uncompressed_size != response_header.payload_size)
                __builtin_trap();
            if (response_header.payload_size != sizeof(LibMP::TypesNtd::MsgBool))
                __builtin_trap();
            LibMP::TypesNtd::MsgBool payload;
            std::memcpy(&payload, response + sizeof(response_header), sizeof(payload));
            (void)payload.value();
        }
    }
    for (uint32_t i = 0; i < kGuard; ++i)
    {
        if (g_first[i] != 0xAB || g_first[kGuard + kArea + i] != 0xAB ||
            g_second[i] != 0xAB || g_second[kGuard + kArea + i] != 0xAB)
            __builtin_trap();
    }
    for (uint32_t i = 0; i < kArea; ++i)
    {
        const bool in_response = i >= response_offset &&
            static_cast<uint64_t>(i) < static_cast<uint64_t>(response_offset) + response_size;
        if (!in_response && response_base[i] != g_before[i])
            __builtin_trap();
    }
    if (g_callback_fired && written == 0)
        __builtin_trap();

    return 0;
}
