// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// control_channel_tests.cpp -- black-box tests for the redesigned control channel.
//
// The protocol is: a raw GPRX BlockHeader (32 bytes, raw encoding) followed by a
// schema-declared FlatBuffers struct or table. Generated dispatch applies an exact-size
// check to structs and flatbuffers::Verifier to tables before use. These tests drive a
// client<->server loopback (mirroring the engine's MicroProfilerService bridge) and cover
// compatibility, validation, snapshotting, response capacity, and returned-byte counts.

#include "test_harness.h"

#include "gprx_container/block.h"
#include "gprx_container/utils.h"
#include "mp_api/control_channel.h"

#include <cstring>
#include <string>
#include <type_traits>

namespace
{
// ---- Mock host state driven by the server channel's callbacks --------------------
struct CallEntry { int fn; long long arg; };
CallEntry g_calls[64];
int g_call_count = 0;
bool g_callback_success = true;
bool g_response_value = true;
uint32_t g_capture_slot = 0;
uint32_t g_capture_max_frame_count = 0;
uint32_t g_capture_container_format = 0;
std::string g_stream_filename_postfix;
uint32_t g_stream_target_slot = 0;
uint32_t g_stream_marker = 0;
bool g_stream_marker_enabled = false;

int record(int fn, long long arg)
{
    if (g_call_count < 64)
        g_calls[g_call_count] = { fn, arg };
    ++g_call_count;
    return 0;
}

bool on_enable_profiler(const LibMP::TypesNtd::MsgBool& request, LibMP::TypesNtd::MsgBool& response)
{
    record(1, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_enable_capture(const LibMP::TypesNtd::MsgBool& request, LibMP::TypesNtd::MsgBool& response)
{
    record(2, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_show_ui(const LibMP::TypesNtd::MsgBool& request, LibMP::TypesNtd::MsgBool& response)
{
    record(3, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_set_frame_limit(const LibMP::TypesNtd::MsgI32& request, LibMP::TypesNtd::MsgBool& response)
{
    record(4, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_set_mem_target_mb(const LibMP::TypesNtd::MsgI32& request, LibMP::TypesNtd::MsgBool& response)
{
    record(10, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_capture_to_slot(const LibMP::TypesNtd::CaptureToSlotRequest& request, LibMP::TypesNtd::MsgBool& response)
{
    record(5, request.target_slot_id());
    g_capture_slot = request.target_slot_id();
    g_capture_max_frame_count = request.max_frame_count();
    g_capture_container_format = request.container_format();
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_reset_slot(const LibMP::TypesNtd::MsgU32& request, LibMP::TypesNtd::MsgBool& response)
{
    record(6, request.value());
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_experimental_start_file_stream(
    const LibMP::TypesNtd::ExperimentalFileStreamStartRequest& request,
    LibMP::TypesNtd::MsgBool& response)
{
    record(7, request.filename_postfix() ? request.filename_postfix()->size() : -1);
    g_stream_filename_postfix = request.filename_postfix() ? request.filename_postfix()->str() : std::string();
    g_stream_target_slot = request.target_slot_id();
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_experimental_start_memory_stream(
    const LibMP::TypesNtd::ExperimentalMemoryStreamStartRequest& request,
    LibMP::TypesNtd::MsgBool& response)
{
    record(12, request.target_slot_id());
    g_stream_target_slot = request.target_slot_id();
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_experimental_stop_stream(const LibMP::TypesNtd::MsgBool&, LibMP::TypesNtd::MsgBool& response)
{
    record(8, 0);
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_experimental_set_marker(
    const LibMP::TypesNtd::ExperimentalStreamMarkerRequest& request,
    LibMP::TypesNtd::MsgBool& response)
{
    record(9, request.marker());
    g_stream_marker = request.marker();
    g_stream_marker_enabled = request.enabled();
    response.mutate_value(g_response_value);
    return g_callback_success;
}
bool on_experimental_get_stream_status(
    const LibMP::TypesNtd::MsgBool&,
    LibMP::TypesNtd::ExperimentalStreamStatus& response)
{
    record(11, 0);
    response = LibMP::TypesNtd::ExperimentalStreamStatus(true, 123, 456, 0x15, 789, 262144, 3210);
    return g_callback_success;
}
bool on_experimental_set_content_mode(
    const LibMP::TypesNtd::ExperimentalContentModeRequest& request,
    LibMP::TypesNtd::MsgBool& response)
{
    record(13, static_cast<uint32_t>(request.content_mode()));
    response.mutate_value(g_response_value);
    return g_callback_success;
}

LibMP::ControlChannel* g_server = nullptr;

// Records the exact command bytes the client produced so negative tests can mutate a
// known-good frame, and asserts the transport invariant that the server never writes
// more than the offered response capacity.
alignas(8) uint8_t g_last_cmd[512];
mpsize_t g_last_cmd_size = 0;
uint8_t* g_mutated_input = nullptr;
mpsize_t g_mutated_input_size = 0;

uint32_t loopback_transfer(const uint8_t* cmd, mpsize_t cmd_size, uint8_t* resp, mpsize_t resp_max)
{
    if (cmd && cmd_size > 0 && cmd_size <= sizeof(g_last_cmd))
    {
        std::memcpy(g_last_cmd, cmd, cmd_size);
        g_last_cmd_size = cmd_size;
    }
    const uint32_t n = g_server->OnCommand(cmd, cmd_size, resp, resp_max);
    CHECK(n <= resp_max); // response must never exceed the offered capacity
    return n;
}

void setup(LibMP::ControlChannel& server, LibMP::ControlChannel& client)
{
    LibMP::ExternalControlIO io_s;
    io_s.OnEnableProfiler = on_enable_profiler;
    io_s.OnEnableCapture = on_enable_capture;
    io_s.OnShowUI = on_show_ui;
    io_s.OnSetFrameLimit = on_set_frame_limit;
    io_s.OnSetMemTargetMB = on_set_mem_target_mb;
    io_s.OnCaptureToSlot = on_capture_to_slot;
    io_s.OnResetSlot = on_reset_slot;
    io_s.OnExperimentalV1StartFileStream = on_experimental_start_file_stream;
    io_s.OnExperimentalV1StartMemoryStream = on_experimental_start_memory_stream;
    io_s.OnExperimentalV1StopStream = on_experimental_stop_stream;
    io_s.OnExperimentalV1SetStreamMarker = on_experimental_set_marker;
    io_s.OnExperimentalV1GetStreamStatus = on_experimental_get_stream_status;
    io_s.OnExperimentalV1SetContentMode = on_experimental_set_content_mode;
    server.AssignIO(io_s);
    g_server = &server;

    LibMP::ExternalControlIO io_c;
    io_c.transfer = loopback_transfer;
    client.AssignIO(io_c);

    g_call_count = 0;
    g_callback_success = true;
    g_response_value = true;
    g_capture_slot = 0;
    g_capture_max_frame_count = 0;
    g_capture_container_format = 0;
    g_stream_filename_postfix.clear();
    g_stream_target_slot = 0;
    g_stream_marker = 0;
    g_stream_marker_enabled = false;
    g_last_cmd_size = 0;
    g_mutated_input = nullptr;
    g_mutated_input_size = 0;
}

// A valid EnableProfiler(true) frame is produced by sending once; return its size.
mpsize_t capture_valid_frame(LibMP::ControlChannel& client)
{
    const LibMP::TypesNtd::MsgBool* response = nullptr;
    client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response);
    return g_last_cmd_size;
}

bool on_enable_profiler_and_mutate_input(
    const LibMP::TypesNtd::MsgBool& request,
    LibMP::TypesNtd::MsgBool& response)
{
    record(1, request.value());
    if (g_mutated_input)
        std::memset(g_mutated_input, 0xA5, g_mutated_input_size);
    response.mutate_value(true);
    return true;
}
} // namespace

// ------------------------------------------------------------------------------------
TEST_CASE(control_generated_type_selection)
{
    using Callback = bool (*)(
        const LibMP::TypesNtd::MsgBool&, LibMP::TypesNtd::MsgBool&);
    static_assert(std::is_same<
        LibMP::ControlDetail::ResponseBuildType<LibMP::ControlCommandId::EnableProfiler>,
        LibMP::TypesNtd::MsgBool>::value,
        "struct callbacks fill the wire type");
    static_assert(std::is_same<
        LibMP::ControlDetail::ResponseView<LibMP::ControlCommandId::EnableProfiler>,
        const LibMP::TypesNtd::MsgBool*>::value,
        "senders output a wire view");
    static_assert(std::is_same<
        LibMP::ControlDetail::ResponseCallback<LibMP::ControlCommandId::EnableProfiler>,
        Callback>::value,
        "callbacks receive complete schema types");

    using StreamStatusCallback = bool (*)(
        const LibMP::TypesNtd::MsgBool&, LibMP::TypesNtd::ExperimentalStreamStatus&);
    static_assert(std::is_same<
        LibMP::ControlDetail::ResponseCallback<LibMP::ControlCommandId::ExperimentalV1GetStreamStatus>,
        StreamStatusCallback>::value,
        "status callbacks fill the status wire type");

    using TableBuildType = LibMP::ControlDetail::ResponseBuildTypeTraits<
        LibMP::TypesNtd::CounterIteratorConfig,
        LibMP::ControlDetail::PayloadKind::Table>::Type;
    static_assert(std::is_same<
        TableBuildType, LibMP::TypesNtd::CounterIteratorConfigT>::value,
        "table callbacks fill the native type");
}

TEST_CASE(control_table_verifier_rejects_corrupted_table)
{
    LibMP::TypesNtd::CounterIteratorConfigT config;
    config.root_counter_ids.push_back(7);
    config.root_counter_ids.push_back(11);

    LibMP::FbBuilder builder;
    builder.Finish(LibMP::TypesNtd::CounterIteratorConfig::Pack(builder, &config));

    uint8_t* payload = builder.GetBufferPointer();
    const mpsize_t payload_size = GPRX::to_mpsize(builder.GetSize());
    const auto* verified = LibMP::ControlDetail::GetVerifiedTable<
        LibMP::TypesNtd::CounterIteratorConfig>(payload, payload_size);
    CHECK(verified != nullptr);
    if (!verified || !verified->root_counter_ids())
        return;
    CHECK_EQ(verified->root_counter_ids()->size(), 2u);

    uint8_t* vector_data = const_cast<uint8_t*>(
        reinterpret_cast<const uint8_t*>(verified->root_counter_ids()));
    LibMP::flatbuffers::WriteScalar<LibMP::flatbuffers::uoffset_t>(
        vector_data, GPRX::max_u32);
    CHECK(LibMP::ControlDetail::GetVerifiedTable<
        LibMP::TypesNtd::CounterIteratorConfig>(payload, payload_size) == nullptr);
}

// ------------------------------------------------------------------------------------
TEST_CASE(control_command_roundtrips)
{
    constexpr uint32_t slot_id = LibMP::TypesNtd::DataSlot_LastUserManaged;
    LibMP::ControlChannel server, client;
    setup(server, client);

    const LibMP::TypesNtd::MsgBool* response = nullptr;
    CHECK(client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response && response->value());
    CHECK(client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(false), response));
    CHECK(response && response->value());
    CHECK(client.SendEnableCapture(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response && response->value());
    CHECK(client.SendShowUI(LibMP::TypesNtd::MsgBool(false), response));
    CHECK(response && response->value());
    CHECK(client.SendSetFrameLimit(LibMP::TypesNtd::MsgI32(256), response));
    CHECK(response && response->value());
    CHECK(client.SendSetMemTargetMB(LibMP::TypesNtd::MsgI32(64), response));
    CHECK(response && response->value());
    const LibMP::TypesNtd::CaptureToSlotRequest capture(
        slot_id,
        512,
        LibMP::TypesNtd::ContainerFormat_CompressedHtml);
    CHECK(client.SendCaptureToSlot(capture, response));
    CHECK(response && response->value());
    CHECK(client.SendResetSlot(LibMP::TypesNtd::MsgU32(slot_id), response));
    CHECK(response && response->value());
    LibMP::TypesNtd::ExperimentalFileStreamStartRequestT start_stream;
    start_stream.filename_postfix = "ExperimentA";
    start_stream.target_slot_id = slot_id;
    CHECK(client.SendExperimentalV1StartFileStream(start_stream, response));
    CHECK(response && response->value());
    CHECK(client.SendExperimentalV1StartMemoryStream(
        LibMP::TypesNtd::ExperimentalMemoryStreamStartRequest(slot_id + 1), response));
    CHECK(response && response->value());
    CHECK(client.SendExperimentalV1SetStreamMarker(
        LibMP::TypesNtd::ExperimentalStreamMarkerRequest(3, true), response));
    CHECK(response && response->value());
    const LibMP::TypesNtd::ExperimentalStreamStatus* status = nullptr;
    CHECK(client.SendExperimentalV1GetStreamStatus(LibMP::TypesNtd::MsgBool(false), status));
    CHECK(status && status->active());
    CHECK_EQ(status->stream_frame_id(), 123u);
    CHECK_EQ(status->absolute_frame_id(), 456u);
    CHECK_EQ(status->marker_mask(), 0x15u);
    CHECK_EQ(status->recorded_size_kb(), 789u);
    CHECK_EQ(status->max_size_kb(), 262144u);
    CHECK_EQ(status->elapsed_ms(), 3210u);
    CHECK(client.SendExperimentalV1SetContentMode(
        LibMP::TypesNtd::ExperimentalContentModeRequest(LibMP::TypesNtd::ContentMode_CountersOnly), response));
    CHECK(response && response->value());
    CHECK(client.SendExperimentalV1StopStream(LibMP::TypesNtd::MsgBool(false), response));
    CHECK(response && response->value());

    CHECK_EQ(g_call_count, 14);
    CHECK_EQ(g_calls[0].fn, 1); CHECK_EQ(g_calls[0].arg, 1);
    CHECK_EQ(g_calls[1].fn, 1); CHECK_EQ(g_calls[1].arg, 0);
    CHECK_EQ(g_calls[2].fn, 2); CHECK_EQ(g_calls[2].arg, 1);
    CHECK_EQ(g_calls[3].fn, 3); CHECK_EQ(g_calls[3].arg, 0);
    CHECK_EQ(g_calls[4].fn, 4); CHECK_EQ(g_calls[4].arg, 256);
    CHECK_EQ(g_calls[5].fn, 10); CHECK_EQ(g_calls[5].arg, 64);
    CHECK_EQ(g_calls[6].fn, 5); CHECK_EQ(g_calls[6].arg, slot_id);
    CHECK_EQ(g_capture_slot, slot_id);
    CHECK_EQ(g_capture_max_frame_count, 512);
    CHECK_EQ(g_capture_container_format, LibMP::TypesNtd::ContainerFormat_CompressedHtml);
    CHECK_EQ(g_calls[7].fn, 6); CHECK_EQ(g_calls[7].arg, slot_id);
    CHECK_EQ(g_calls[8].fn, 7); CHECK(g_stream_filename_postfix == "ExperimentA");
    CHECK_EQ(g_calls[8].arg, 11);
    CHECK_EQ(g_calls[9].fn, 12); CHECK_EQ(g_calls[9].arg, slot_id + 1);
    CHECK_EQ(g_stream_target_slot, slot_id + 1);
    CHECK_EQ(g_calls[10].fn, 9); CHECK_EQ(g_stream_marker, 3u); CHECK(g_stream_marker_enabled);
    CHECK_EQ(g_calls[11].fn, 11);
    CHECK_EQ(g_calls[12].fn, 13); CHECK_EQ(g_calls[12].arg, 1);
    CHECK_EQ(g_calls[13].fn, 8);
}

// Preserve the complete int32 argument through the MsgI32 struct path.
TEST_CASE(control_i32_value_preserved)
{
    LibMP::ControlChannel server, client;
    setup(server, client);

    const LibMP::TypesNtd::MsgBool* response = nullptr;
    CHECK(client.SendSetFrameLimit(LibMP::TypesNtd::MsgI32(0), response));
    CHECK(response && response->value());
    CHECK(client.SendSetFrameLimit(LibMP::TypesNtd::MsgI32(-1), response));
    CHECK(response && response->value());
    CHECK(client.SendSetFrameLimit(LibMP::TypesNtd::MsgI32(2147483647), response));
    CHECK(response && response->value());

    CHECK_EQ(g_call_count, 3);
    CHECK_EQ(g_calls[0].arg, 0);
    CHECK_EQ(g_calls[1].arg, -1);
    CHECK_EQ(g_calls[2].arg, 2147483647);
}

// A negative command response is distinct from a transport or validation failure.
TEST_CASE(control_false_response)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    g_response_value = false;

    const LibMP::TypesNtd::MsgBool* response = nullptr;
    CHECK(client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response && !response->value());
    CHECK_EQ(g_call_count, 1); // the callback still ran
}

TEST_CASE(control_callback_failure)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    g_callback_success = false;

    LibMP::TypesNtd::MsgBool previous_response(true);
    const LibMP::TypesNtd::MsgBool* response = &previous_response;
    CHECK(!client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response == nullptr);
    CHECK_EQ(g_call_count, 1);
}

TEST_CASE(control_missing_callback_failure)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    server.AssignIO({});

    const LibMP::TypesNtd::MsgBool* response = nullptr;
    CHECK(!client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response == nullptr);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_rejects_garbage)
{
    LibMP::ControlChannel server, client;
    setup(server, client);

    alignas(8) uint8_t resp[128];
    alignas(8) uint8_t buf[256];
    for (unsigned i = 0; i < sizeof(buf); ++i)
        buf[i] = static_cast<uint8_t>(0xA5 ^ (i * 7));

    // Various sizes, none of which carry a valid raw magic -> rejected, no callback.
    CHECK_EQ(server.OnCommand(buf, 16, resp, sizeof(resp)), 0u);
    CHECK_EQ(server.OnCommand(buf, 32, resp, sizeof(resp)), 0u);
    CHECK_EQ(server.OnCommand(buf, sizeof(buf), resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_rejects_oversized_message)
{
    LibMP::ControlChannel server, client;
    setup(server, client);

    alignas(8) uint8_t command[4097] = {};
    alignas(8) uint8_t response[128] = {};
    CHECK_EQ(server.OnCommand(command, sizeof(command), response, sizeof(response)), 0u);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_rejects_short_header)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    CHECK(n >= GPRX::BlockHeaderRawSize);
    g_call_count = 0;

    alignas(8) uint8_t resp[128];
    // Anything shorter than a full raw header must be rejected outright.
    for (mpsize_t s = 1; s < GPRX::BlockHeaderRawSize; ++s)
        CHECK_EQ(server.OnCommand(g_last_cmd, s, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

// Reject a non-raw payload even when the rest of the command frame is valid.
TEST_CASE(control_rejects_non_raw_encoding)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t buf[512];
    std::memcpy(buf, g_last_cmd, n);
    auto* h = reinterpret_cast<GPRX::BlockHeader*>(buf);
    h->encoding_method = GPRX::BlockPayloadEncodingMethod::LZ4;

    alignas(8) uint8_t resp[128];
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

// The control path is raw-only: both hex header forms must be rejected.
TEST_CASE(control_rejects_hex_header)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t resp[128];
    alignas(8) uint8_t buf[512];

    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->metadata0 = GPRX::MagicFormatHexFull;
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->metadata0 = GPRX::MagicFormat; // truncated-hex form
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    CHECK_EQ(g_call_count, 0);
}

// A frame flagged as a response (not a request) must not be dispatched as a command.
TEST_CASE(control_rejects_response_flag)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t buf[512];
    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->type_meta = LibMP::ControlDetail::ResponseTypeMeta;

    alignas(8) uint8_t resp[128];
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

// A truncated payload (header claims more bytes than are present) must be rejected.
TEST_CASE(control_rejects_truncated_payload)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    CHECK(n > GPRX::BlockHeaderRawSize); // there is a payload to truncate
    g_call_count = 0;

    alignas(8) uint8_t resp[128];
    // Pass the full header but drop the last payload byte.
    CHECK_EQ(server.OnCommand(g_last_cmd, n - 1, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_rejects_noncanonical_header_and_trailing_bytes)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    const mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t resp[128];
    alignas(8) uint8_t buf[512];

    // Raw framing requires canonical magic, a fixed block id, and matching lengths.
    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->metadata0 |= 1;
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->block_id = LibMP::ControlDetail::MessageBlockId + 1;
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->type_meta = LibMP::ControlDetail::ResponseTypeMeta + 1;
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    std::memcpy(buf, g_last_cmd, n);
    reinterpret_cast<GPRX::BlockHeader*>(buf)->payload_uncompressed_size++;
    CHECK_EQ(server.OnCommand(buf, n, resp, sizeof(resp)), 0u);

    std::memcpy(buf, g_last_cmd, n);
    buf[n] = 0xA5;
    CHECK_EQ(server.OnCommand(buf, n + 1, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_rejects_wrong_struct_size)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    const mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t buf[512];
    std::memcpy(buf, g_last_cmd, n);
    alignas(8) uint8_t resp[128];

    // Keep framing internally consistent but append one byte beyond MsgBool.
    auto* header = reinterpret_cast<GPRX::BlockHeader*>(buf);
    ++header->payload_size;
    ++header->payload_uncompressed_size;
    buf[n] = 0;
    CHECK_EQ(server.OnCommand(buf, n + 1, resp, sizeof(resp)), 0u);

    // A missing struct payload is rejected as well.
    std::memcpy(buf, g_last_cmd, n);
    header = reinterpret_cast<GPRX::BlockHeader*>(buf);
    header->payload_size = 0;
    header->payload_uncompressed_size = 0;
    CHECK_EQ(server.OnCommand(buf, GPRX::BlockHeaderRawSize, resp, sizeof(resp)), 0u);
    CHECK_EQ(g_call_count, 0);
}

TEST_CASE(control_processes_only_the_internal_message_snapshot)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    const mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    // An unaligned external view is accepted because OnCommand copies the complete
    // message into its aligned internal buffer before parsing it.
    alignas(8) uint8_t external[513];
    std::memcpy(external + 1, g_last_cmd, n);
    g_mutated_input = external + 1;
    g_mutated_input_size = n;

    LibMP::ExternalControlIO io;
    io.OnEnableProfiler = on_enable_profiler_and_mutate_input;
    server.AssignIO(io);

    alignas(8) uint8_t response[128];
    const mpsize_t written = server.OnCommand(
        external + 1, n, response, sizeof(response));
    CHECK_EQ(written, GPRX::BlockHeaderRawSize + sizeof(LibMP::TypesNtd::MsgBool));
    CHECK_EQ(g_call_count, 1);

    GPRX::BlockHeader response_header;
    std::memcpy(&response_header, response, sizeof(response_header));
    CHECK_EQ(response_header.type_meta, 1u);
    CHECK_EQ(response_header.payload_size, sizeof(LibMP::TypesNtd::MsgBool));
    LibMP::TypesNtd::MsgBool response_payload;
    std::memcpy(
        &response_payload, response + GPRX::BlockHeaderRawSize, sizeof(response_payload));
    CHECK(response_payload.value());
}

TEST_CASE(control_handshake_is_capacity_checked)
{
    LibMP::ControlChannel server, client;
    setup(server, client);

    alignas(8) uint8_t resp[sizeof(LibMP::HandshakeResp)] = {};
    CHECK_EQ(server.OnCommand(nullptr, 0, resp, sizeof(resp)), sizeof(resp));

    LibMP::HandshakeResp parsed = {};
    std::memcpy(&parsed, resp, sizeof(parsed));
    CHECK_EQ(parsed.remote_versions.control_channel, LibMP::LocalVersion::CONTROL_CHANNEL);
    CHECK_EQ(parsed.error_code, 0u);

    // A null response is rejected even for the special handshake path.
    CHECK_EQ(server.OnCommand(nullptr, 0, nullptr, sizeof(resp)), 0u);
}

// The receiver must not write past a small response buffer; it returns 0 instead.
TEST_CASE(control_small_response_capacity)
{
    LibMP::ControlChannel server, client;
    setup(server, client);
    mpsize_t n = capture_valid_frame(client);
    g_call_count = 0;

    alignas(8) uint8_t resp[64];
    // A response needs a 32-byte header plus its struct; a handful of bytes cannot hold it.
    CHECK_EQ(server.OnCommand(g_last_cmd, n, resp, 8), 0u);
    CHECK_EQ(g_call_count, 0); // no state change if the result cannot be returned
    // Handshake needs sizeof(HandshakeResp); too-small capacity must also yield 0.
    CHECK_EQ(server.OnCommand(nullptr, 0, resp, 1), 0u);
}

// Reject a response with a mismatched ticket even when its frame is otherwise valid.
TEST_CASE(control_client_rejects_tampered_ticket)
{
    static LibMP::ControlChannel server;
    static LibMP::ControlChannel client;
    setup(server, client);

    // A transport that corrupts the response ticket after the server produced it.
    struct Tamper
    {
        static uint32_t transfer(const uint8_t* cmd, mpsize_t cmd_size, uint8_t* resp, mpsize_t resp_max)
        {
            uint32_t nbytes = g_server->OnCommand(cmd, cmd_size, resp, resp_max);
            if (nbytes >= GPRX::BlockHeaderRawSize)
                reinterpret_cast<GPRX::BlockHeader*>(resp)->metadata1 ^= (uint64_t(1) << 32); // flip a ticket bit
            return nbytes;
        }
    };
    LibMP::ExternalControlIO io_c;
    io_c.transfer = Tamper::transfer;
    client.AssignIO(io_c);
    g_call_count = 0;

    // The command still runs on the server, but the client rejects the mismatched reply.
    LibMP::TypesNtd::MsgBool previous_response(true);
    const LibMP::TypesNtd::MsgBool* response = &previous_response;
    CHECK(!client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response));
    CHECK(response == nullptr);
    CHECK_EQ(g_call_count, 1);
}

LIBMP_TEST_MAIN("control_channel")
