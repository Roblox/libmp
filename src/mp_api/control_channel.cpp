// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "control_channel.h"
#include "flatbuffer_types/control_commands_generated.inc"
#include "gprx_container/utils_ex.h"
#include "gprx_container/block.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/type_traits.h"
#include <cstdio>

namespace LibMP
{

ControlChannel::ControlChannel()
{
    ExternalControlIO io;
    io.transfer = LibMpExtControlMsg;
    io.ConsoleLog = LibMpExtConsoleLog;
    AssignIO(io);

    m_incoming_response_buffer.resize(MESSAGE_BUFFER_SIZE_MAX);
    m_incoming_request_snapshot.resize(MESSAGE_BUFFER_SIZE_MAX);
    LocalVersion::Fill(s_local_versions);
}

void ControlChannel::AssignIO(const ExternalControlIO& io)
{
    m_io = io;
}

// Fixed FlatBuffers structs have no graph to verify. Their generated accessors can read the
// serialized bytes directly once the exact wire size and alignment have been established.
template <typename T>
static const T* GetControlStruct(const uint8_t* payload, mpsize_t size)
{
    static_assert(ntd::is_trivially_copyable_v<T>, "control structs must be trivially copyable");
    if (!payload || size != sizeof(T) || !GPRX::is_aligned<alignof(T)>(payload))
        return nullptr;
    return reinterpret_cast<const T*>(payload);
}

mpsize_t ControlChannel::FormatMessage(
    uint16_t session_id, uint32_t ticket_id,
    bool is_response, ControlCommandId cmd,
    const uint8_t* fb_data, mpsize_t fb_size,
    uint8_t* outbuf, mpsize_t outbuf_max_size,
    ResizeCallback outbuf_resize_callback)
{
    // The control channel is raw-header only, so the framing is a fixed 32-byte header.
    const mpsize_t header_disk_size = GPRX::BlockHeaderRawSize;

    // Prevent the framed size from wrapping past the output capacity check.
    if (fb_size > GPRX::max_mpsize_t - header_disk_size)
        return 0;
    mpsize_t total_required_size = header_disk_size + fb_size;
    if (fb_size > 0 && !fb_data)
        return 0;

    if (total_required_size > outbuf_max_size)
    {
        if (!outbuf_resize_callback)
            return 0;
        outbuf = outbuf_resize_callback(total_required_size);
        if (!outbuf)
            return 0;
        outbuf_max_size = total_required_size;
    }
    if (!outbuf)
        return 0;

    // 1. Prepare the raw binary header (raw magic + Raw encoding).
    GPRX::BlockHeader header = {};
    header.metadata0 = GPRX::MagicFormatRawFull;
    header.block_id = ControlDetail::MessageBlockId;
    header.type_id = GPRX::BlockType::MagicTmp;
    header.encoding_method = GPRX::BlockPayloadEncodingMethod::Raw;
    header.payload_size = fb_size;
    header.payload_uncompressed_size = fb_size;

    // Pack metadata
    header.type_meta = is_response
        ? ControlDetail::ResponseTypeMeta
        : ControlDetail::RequestTypeMeta;
    header.metadata1 = (
        (static_cast<uint64_t>(ticket_id) << 32) |
        (static_cast<uint64_t>(session_id) << 16) |
        static_cast<uint64_t>(cmd)
    );

    // 2. Write header, then the schema-declared payload.
    ntd::memcpy(outbuf, &header, sizeof(GPRX::BlockHeader));
    if (fb_size > 0 && fb_data != nullptr)
        ntd::memcpy(outbuf + header_disk_size, fb_data, fb_size);

    return total_required_size;
}

ParsedControlMessage ControlChannel::ParseMessage(const uint8_t* buffer, mpsize_t size)
{
    ParsedControlMessage result = {};
    if (!buffer || !GPRX::is_aligned<sizeof(uint32_t)>(buffer) || size < GPRX::BlockHeaderRawSize)
        return result;

    GPRX::BlockHeader header;
    ntd::memcpy(&header, buffer, sizeof(GPRX::BlockHeader));

    // Generic GPRX magic variants are intentionally excluded from this fixed raw protocol.
    if (header.metadata0 != GPRX::MagicFormatRawFull ||
        header.type_id != GPRX::BlockType::MagicTmp ||
        (header.type_meta != ControlDetail::RequestTypeMeta &&
            header.type_meta != ControlDetail::ResponseTypeMeta) ||
        header.encoding_method != GPRX::BlockPayloadEncodingMethod::Raw ||
        header.block_id != ControlDetail::MessageBlockId ||
        header.payload_uncompressed_size != header.payload_size)
        return result;

    const uint64_t meta = header.metadata1;
    result.is_response = header.type_meta == ControlDetail::ResponseTypeMeta;
    result.cmd_id = static_cast<ControlCommandId>(meta & GPRX::max_u16);
    result.session_id = static_cast<uint16_t>((meta >> 16) & GPRX::max_u16);
    result.ticket_number = static_cast<uint32_t>((meta >> 32) & GPRX::max_u32);

    if (header.payload_size != size - GPRX::BlockHeaderRawSize)
        return result; // Truncated payload or ambiguous trailing bytes

    result.payload_ptr = buffer + GPRX::BlockHeaderRawSize;
    result.payload_size = header.payload_size;
    result.is_valid = true;
    return result;
}

void ControlChannel::LogVersionMsg(ControlLogLevel log_level, const char* str)
{
    if (!m_io.ConsoleLog)
        return;
    char buf[LOG_BUFFER_STACK_SIZE_MAX];
    int len = snprintf(buf, sizeof(buf),
        "%s Local version: %u.%u, available version: %u.%u", str,
        Version::GetMajor(s_local_versions.library), Version::GetMinor(s_local_versions.library),
        Version::GetMajor(s_remote_versions.library), Version::GetMinor(s_remote_versions.library));
    if (len < 0)
        return;
    len = ntd::min(len, static_cast<int>(sizeof(buf) - 1));
    m_io.ConsoleLog(
        static_cast<uint32_t>(log_level),
        buf,
        GPRX::to_mpsize(len));
}

CommandResult ControlChannel::ExecuteCommand(ControlCommandId cmd, const uint8_t* req_payload, mpsize_t req_size)
{
    if (!m_io.transfer)
        return {};

    uint32_t ticket = m_next_ticket++;

    // 1. Format the outgoing GPRX message
    mpsize_t written_tx_size = FormatMessage(m_session_id, ticket, false, cmd,
        req_payload, req_size,
        m_outgoing_request_buffer.data(), GPRX::to_mpsize(m_outgoing_request_buffer.size()),
        [this](mpsize_t required_size) -> uint8_t*
        {
            m_outgoing_request_buffer.resize(required_size);
            return m_outgoing_request_buffer.data();
        }
    );

    if (!written_tx_size)
        return {};

    // 2. Parse only bytes returned by this transfer, excluding stale buffer contents.
    uint32_t rx_size = m_io.transfer(m_outgoing_request_buffer.data(), written_tx_size,
        m_incoming_response_buffer.data(),
        GPRX::to_mpsize(m_incoming_response_buffer.size()));
    if (rx_size == 0 || rx_size > m_incoming_response_buffer.size())
        return {};

    // 3. Parse the incoming response.
    ParsedControlMessage msg = ParseMessage(m_incoming_response_buffer.data(), rx_size);

    if (!msg.is_valid || !msg.is_response || !msg.payload_ptr || msg.session_id != m_session_id ||
        msg.ticket_number != ticket || msg.cmd_id != cmd)
        return {};

    return {msg.payload_ptr, msg.payload_size, true};
}

template <ControlCommandId Command>
bool ControlChannel::SendCommand(
    const uint8_t* request_payload, mpsize_t request_size,
    ControlDetail::ResponseView<Command>& response)
{
    using Traits = ControlDetail::CommandTraits<Command>;
    using Response = typename Traits::Response;

    response = nullptr;

    CommandResult result = ExecuteCommand(Command, request_payload, request_size);
    if (!result.success)
        return false;

    const Response* response_view = nullptr;
    if constexpr (Traits::ResponseKind == ControlDetail::PayloadKind::Struct)
        response_view = GetControlStruct<Response>(result.payload, result.size);
    else
        response_view = ControlDetail::GetVerifiedTable<Response>(result.payload, result.size);

    if (!response_view)
        return false;
    response = response_view;
    return true;
}

template <ControlCommandId Command>
mpsize_t ControlChannel::ReceiveCommand(
    const ParsedControlMessage& message,
    uint8_t* response_buffer,
    mpsize_t response_capacity,
    CommandResult (ControlChannel::*handler)(
        const typename ControlDetail::CommandTraits<Command>::Request&))
{
    using Traits = ControlDetail::CommandTraits<Command>;
    using Request = typename Traits::Request;
    using Response = typename Traits::Response;

    CommandResult response;
    auto invoke_handler = [&](const Request& request) -> bool
    {
        if constexpr (Traits::ResponseKind == ControlDetail::PayloadKind::Struct)
        {
            static_assert(sizeof(Response) <= GPRX::max_mpsize_t,
                "control response is too large");
            constexpr mpsize_t payload_size = static_cast<mpsize_t>(sizeof(Response));
            if (payload_size > GPRX::max_mpsize_t - GPRX::BlockHeaderRawSize ||
                GPRX::BlockHeaderRawSize + payload_size > response_capacity)
                return false;
        }
        else if (response_capacity < GPRX::BlockHeaderRawSize)
        {
            return false;
        }

        response = (this->*handler)(request);
        return true;
    };

    if constexpr (Traits::RequestKind == ControlDetail::PayloadKind::Struct)
    {
        const Request* request = GetControlStruct<Request>(
            message.payload_ptr, message.payload_size);
        if (!request)
            return 0;
        if (!invoke_handler(*request))
            return 0;
    }
    else
    {
        const Request* request = ControlDetail::GetVerifiedTable<Request>(
            message.payload_ptr, message.payload_size);
        if (!request)
            return 0;
        if (!invoke_handler(*request))
            return 0;
    }

    if (!response.success)
        return 0;

    if constexpr (Traits::ResponseKind == ControlDetail::PayloadKind::Struct)
    {
        static_assert(ntd::is_trivially_copyable_v<Response>,
            "control response structs must be trivially copyable");
        if (!response.payload || response.size != sizeof(Response))
            return 0;
    }
    else if (!ControlDetail::GetVerifiedTable<Response>(response.payload, response.size))
    {
        return 0;
    }

    return FormatMessage(
        message.session_id, message.ticket_number, true, message.cmd_id,
        response.payload, response.size, response_buffer, response_capacity);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool ControlChannel::AreVersionsCompatible()
{
    return s_versions_compatible;
}

bool ControlChannel::FetchRemoteVersions()
{
    if (s_remote_versions.IsFilled())
        return true;

    if (!m_io.transfer)
        return false;

    // Send a 0-size message as the "GetVersions" trigger
    if (m_io.transfer(
            nullptr,
            0,
            m_incoming_response_buffer.data(),
            GPRX::to_mpsize(m_incoming_response_buffer.size())) != sizeof(HandshakeResp))
    {
        s_error_code = 1;
        return false;
    }

    HandshakeResp resp;
    ntd::memcpy(&resp, m_incoming_response_buffer.data(), sizeof(HandshakeResp));
    s_remote_versions = resp.remote_versions;
    s_error_code = resp.error_code;

    if (s_error_code != 0)
    {
        LogVersionMsg(ControlLogLevel::Warning, "WARNING: The engine reported that LibMP functionality is unavailable.");
        return false;
    }

    if (!s_local_versions.MajorMatch(s_remote_versions))
    {
        LogVersionMsg(ControlLogLevel::Error, "ERROR: The version of LibMP used in this experience is incompatible with the host. Please replace the module.");
        return false;
    }

    s_versions_compatible = true;

    if (s_local_versions.library < s_remote_versions.library_warning)
    {
        LogVersionMsg(ControlLogLevel::Warning, "WARNING: A new version of LibMP is available, and an upgrade is recommended.");
    }
    
    return true;
}

// Response encoders and handlers.
#define LIBMP_CONTROL_ENCODE_RESPONSE_Struct(TYPE, OBJECT)                  \
    static_assert(sizeof(OBJECT) <= ControlDetail::ResponseStorageSize(),   \
        "control response storage is too small");                           \
    ntd::memcpy(m_response_storage, &OBJECT, sizeof(OBJECT));               \
    return {m_response_storage, GPRX::to_mpsize(sizeof(OBJECT)), true}

#define LIBMP_CONTROL_ENCODE_RESPONSE_Table(TYPE, OBJECT)                          \
    m_control_builder.Clear();                                                     \
    m_control_builder.Finish(TypesNtd::Create##TYPE(m_control_builder, &OBJECT));  \
    if (m_control_builder.GetSize() > GPRX::max_mpsize_t)                          \
        return {};                                                                 \
    return {                                                                       \
        m_control_builder.GetBufferPointer(),                                      \
        GPRX::to_mpsize(m_control_builder.GetSize()),                              \
        true,                                                                      \
    }

#define LIBMP_CONTROL_HANDLER_IMPL(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    CommandResult ControlChannel::Handle##NAME(const TypesNtd::REQUEST& request)             \
    {                                                                                        \
        using BuildType = ControlDetail::ResponseBuildType<ControlCommandId::NAME>;          \
        BuildType response{};                                                                \
        if (!m_io.On##NAME || !m_io.On##NAME(request, response))                             \
            return {};                                                                       \
        LIBMP_CONTROL_ENCODE_RESPONSE_##RESPONSE_KIND(RESPONSE, response);                   \
    }
LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_HANDLER_IMPL)
#undef LIBMP_CONTROL_HANDLER_IMPL
#undef LIBMP_CONTROL_ENCODE_RESPONSE_Table
#undef LIBMP_CONTROL_ENCODE_RESPONSE_Struct

////////////////////////////////////////////////////////////////////////////////////////////////////
mpsize_t ControlChannel::OnCommand(
    const uint8_t* cmdBuffer, mpsize_t cmdSize,
    uint8_t* respBuffer, mpsize_t respMaxSize)
{
    if (!respBuffer || !GPRX::is_aligned<sizeof(uint32_t)>(respBuffer))
        return 0;

    // --- SPECIAL CASE: Handshake ---
    if (cmdSize == 0)
    {
        if (respMaxSize < sizeof(HandshakeResp))
            return 0;

        HandshakeResp resp;
        resp.remote_versions = s_local_versions;
        resp.error_code = m_io.get_error_code ? m_io.get_error_code() : 0;
        ntd::memcpy(respBuffer, &resp, sizeof(HandshakeResp));
        return sizeof(HandshakeResp);
    }

    // Snapshot the complete untrusted message before parsing. Header validation,
    // payload validation, and callback dispatch therefore all observe the same bytes.
    if (!cmdBuffer || cmdSize > m_incoming_request_snapshot.size())
        return 0;
    ntd::memcpy(m_incoming_request_snapshot.data(), cmdBuffer, cmdSize);

    // --- STANDARD CASE: raw header + schema-declared payload ---
    ParsedControlMessage msg = ParseMessage(m_incoming_request_snapshot.data(), cmdSize);
    if (!msg.is_valid || msg.is_response)
        return 0;

    // Command dispatch.
    switch (msg.cmd_id)
    {
#define LIBMP_CONTROL_DISPATCH(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
        case ControlCommandId::NAME:                                                     \
            return ReceiveCommand<ControlCommandId::NAME>(                               \
                msg, respBuffer, respMaxSize, &ControlChannel::Handle##NAME);
        LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_DISPATCH)
#undef LIBMP_CONTROL_DISPATCH
        default:
            return 0;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
#define LIBMP_CONTROL_SENDER_IMPL_Struct(NAME, REQUEST)                                  \
    bool ControlChannel::Send##NAME(                                                     \
        const TypesNtd::REQUEST& request,                                                \
        ControlDetail::ResponseView<ControlCommandId::NAME>& response)                   \
    {                                                                                    \
        return SendCommand<ControlCommandId::NAME>(                                      \
            reinterpret_cast<const uint8_t*>(&request),                                  \
            GPRX::to_mpsize(sizeof(request)), response);                                 \
    }

#define LIBMP_CONTROL_SENDER_IMPL_Table(NAME, REQUEST)                                        \
    bool ControlChannel::Send##NAME(                                                          \
        const TypesNtd::REQUEST##T& request,                                                  \
        ControlDetail::ResponseView<ControlCommandId::NAME>& response)                        \
    {                                                                                         \
        m_control_builder.Clear();                                                            \
        m_control_builder.Finish(TypesNtd::Create##REQUEST(m_control_builder, &request));     \
        if (m_control_builder.GetSize() > GPRX::max_mpsize_t)                                 \
            return false;                                                                     \
        return SendCommand<ControlCommandId::NAME>(                                           \
            m_control_builder.GetBufferPointer(),                                             \
            GPRX::to_mpsize(m_control_builder.GetSize()), response);                          \
    }

#define LIBMP_CONTROL_SENDER_IMPL(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    LIBMP_CONTROL_SENDER_IMPL_##REQUEST_KIND(NAME, REQUEST)
LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_SENDER_IMPL)
#undef LIBMP_CONTROL_SENDER_IMPL
#undef LIBMP_CONTROL_SENDER_IMPL_Table
#undef LIBMP_CONTROL_SENDER_IMPL_Struct
#undef LIBMP_CONTROL_COMMANDS
#undef LIBMP_CONTROL_HAS_TABLE_PAYLOADS

uint32_t ControlChannel::s_error_code = 0;
Version ControlChannel::s_remote_versions = {};
Version ControlChannel::s_local_versions = {};
bool ControlChannel::s_versions_compatible = false;

} // namespace LibMP

////////////////////////////////////////////////////////////////////////////////////////////////////
#if !ENABLE_EXTERNAL_CONTROL_IO

uint32_t LibMpExtControlMsg(
    const uint8_t* /*msg_buf*/,
    mpsize_t /*msg_sz*/,
    uint8_t* /*resp_buf*/,
    mpsize_t /*resp_max_sz*/)
{
    return 0;
}

void LibMpExtConsoleLog(
    uint32_t /*log_level*/,
    const char* /*str*/,
    mpsize_t /*len*/)
{
}

#endif // !ENABLE_EXTERNAL_CONTROL_IO
