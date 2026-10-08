// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/types.h"
#include "version.h"
#include "flatbuffer_types/mp_types_ntd.h"
#include "flatbuffer_types/control_commands_generated.inc"
#include "ntd_stdlib/functional.h"
#include "ntd_stdlib/vector.h"

#ifndef ENABLE_EXTERNAL_CONTROL_IO
#ifdef LIBMP_MODULE_BUILD
    #define ENABLE_EXTERNAL_CONTROL_IO 1
#else
    #define ENABLE_EXTERNAL_CONTROL_IO 0
#endif
#endif

namespace LibMP
{

namespace Slots
{
static constexpr uint32_t Count = TypesNtd::DataSlot_MAX + 1;

inline bool IsValid(uint32_t slot_id)
{
    return slot_id < Count;
}

inline bool IsWritable(uint32_t slot_id)
{
    return slot_id != TypesNtd::DataSlot_Live && IsValid(slot_id);
}
} // namespace Slots

////////////////////////////////////////////////////////////////////////////////////////////////////
// Schema annotations generate LIBMP_CONTROL_COMMANDS. The C++ templates below expand it into
// command IDs, traits, receiver slots, dispatch, handlers, and senders, keeping protocol mechanics
// beside their compiler-checked implementations.
enum class ControlCommandId : uint16_t
{
    Invalid = 0,
#define LIBMP_CONTROL_COMMAND_ID(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    NAME = ID,
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_COMMAND_ID)
#undef LIBMP_CONTROL_COMMAND_ID
};

namespace ControlDetail
{
constexpr uint32_t MessageBlockId = 1;
constexpr uint8_t RequestTypeMeta = 0;
constexpr uint8_t ResponseTypeMeta = 1;

enum class PayloadKind : uint8_t
{
    Struct,
    Table,
};

template <ControlCommandId Command>
struct CommandTraits;

#define LIBMP_CONTROL_COMMAND_TRAITS(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    template <>                                                                                \
    struct CommandTraits<ControlCommandId::NAME>                                               \
    {                                                                                          \
        using Request = TypesNtd::REQUEST;                                                     \
        using Response = TypesNtd::RESPONSE;                                                   \
        static constexpr PayloadKind RequestKind = PayloadKind::REQUEST_KIND;                  \
        static constexpr PayloadKind ResponseKind = PayloadKind::RESPONSE_KIND;                \
    };
LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_COMMAND_TRAITS)
#undef LIBMP_CONTROL_COMMAND_TRAITS

template <typename Response, PayloadKind Kind>
struct ResponseBuildTypeTraits;

template <typename Response>
struct ResponseBuildTypeTraits<Response, PayloadKind::Struct>
{
    using Type = Response;
};

template <typename Response>
struct ResponseBuildTypeTraits<Response, PayloadKind::Table>
{
    using Type = typename Response::NativeTableType;
};

template <ControlCommandId Command>
using ResponseBuildType = typename ResponseBuildTypeTraits<
    typename CommandTraits<Command>::Response,
    CommandTraits<Command>::ResponseKind>::Type;

template <ControlCommandId Command>
using ResponseView = const typename CommandTraits<Command>::Response*;

template <ControlCommandId Command>
using ResponseCallback = bool (*)(
    const typename CommandTraits<Command>::Request&,
    ResponseBuildType<Command>&);

template <typename Table>
const Table* GetVerifiedTable(const uint8_t* payload, mpsize_t size)
{
    if (!payload)
        return nullptr;
#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-input-verification !LIBMP_TRUSTED_INPUTS_ONLY
    flatbuffers::Verifier verifier(payload, size);
    if (!verifier.VerifyBuffer<Table>(nullptr))
        return nullptr;
// @amalgamate: config-end untrusted-input-verification
#else
    (void)size;
#endif
    return flatbuffers::GetRoot<Table>(payload);
}

constexpr size_t ResponseStorageSize()
{
    size_t result = 1;
#define LIBMP_CONTROL_RESPONSE_STORAGE_SIZE(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    if (sizeof(TypesNtd::RESPONSE) > result)                                                          \
        result = sizeof(TypesNtd::RESPONSE);
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_RESPONSE_STORAGE_SIZE)
#undef LIBMP_CONTROL_RESPONSE_STORAGE_SIZE
    return result;
}

constexpr size_t ResponseStorageAlignment()
{
    size_t result = 1;
#define LIBMP_CONTROL_RESPONSE_STORAGE_ALIGNMENT(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    if (alignof(TypesNtd::RESPONSE) > result)                                                              \
        result = alignof(TypesNtd::RESPONSE);
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_RESPONSE_STORAGE_ALIGNMENT)
#undef LIBMP_CONTROL_RESPONSE_STORAGE_ALIGNMENT
    return result;
}
} // namespace ControlDetail

////////////////////////////////////////////////////////////////////////////////////////////////////
static constexpr uint64_t DATA_FORMAT_SIGNATURE = 0x00c5a83f01030308; // 56 bit

////////////////////////////////////////////////////////////////////////////////////////////////////
struct LocalVersion
{
    static constexpr uint32_t DATA_FORMAT     = Version::BuildFrom(1, 1); // 8+8 bit
    static constexpr uint32_t CONTROL_CHANNEL = Version::BuildFrom(1, 1);
    static constexpr uint32_t LIBRARY         = Version::BuildFrom(1, 1);
    static constexpr uint32_t LIBRARY_WARNING = Version::BuildFrom(1, 0);

    static void Fill(Version& v)
    {
        v.data_format = DATA_FORMAT;
        v.control_channel = CONTROL_CHANNEL;
        v.library = LIBRARY;
        v.library_warning = LIBRARY_WARNING;
    }
};

#pragma pack(push, 1) // Disable padding
struct HandshakeResp
{
    Version remote_versions;
    uint32_t error_code;
};
#pragma pack(pop)

enum class ControlLogLevel : uint32_t
{
    Log = 0,
    Warning = 1,
    Error = 2
};

// Externally provided implementation for the control channel I/O and methods
struct ExternalControlIO
{
    // I/O function for the client
    using TransferFunc = uint32_t(*)(const uint8_t*, mpsize_t, uint8_t*, mpsize_t);
    TransferFunc transfer = nullptr;

    // Getting error code during handshake
    uint32_t (*get_error_code)() = nullptr;

    // Logging callback
    void (*ConsoleLog)(uint32_t, const char*, mpsize_t) = nullptr;

    // Receiver slots; callbacks fill the response and report operation success.
#define LIBMP_CONTROL_RECEIVER_SLOT(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    ControlDetail::ResponseCallback<ControlCommandId::NAME> On##NAME = nullptr;
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_RECEIVER_SLOT)
#undef LIBMP_CONTROL_RECEIVER_SLOT
};

struct ParsedControlMessage
{
    uint16_t session_id = 0;
    uint32_t ticket_number = 0;
    bool is_response = false;
    ControlCommandId cmd_id = ControlCommandId::Invalid;
    const uint8_t* payload_ptr = nullptr;
    mpsize_t payload_size = 0;
    bool is_valid = false;
};

struct CommandResult
{
    const uint8_t* payload = nullptr;
    mpsize_t size = 0;
    bool success = false;
};

class ControlChannel
{
private:
    static constexpr mpsize_t MESSAGE_BUFFER_SIZE_MAX = 4 * 1024;
    static constexpr mpsize_t LOG_BUFFER_STACK_SIZE_MAX = 512;

    static uint32_t s_error_code;
    static Version s_remote_versions;
    static Version s_local_versions;
    static bool s_versions_compatible;

    ExternalControlIO m_io;
    
    // Client-side storage used by ExecuteCommand and FetchRemoteVersions.
    ntd::vector<uint8_t> m_outgoing_request_buffer;
    ntd::vector<uint8_t> m_incoming_response_buffer;

    // Server-side immutable copy of the external bytes passed to OnCommand.
    ntd::vector<uint8_t> m_incoming_request_snapshot;
#if LIBMP_CONTROL_HAS_TABLE_PAYLOADS
    FbBuilder m_control_builder;
#endif
    alignas(ControlDetail::ResponseStorageAlignment())
        uint8_t m_response_storage[ControlDetail::ResponseStorageSize()];

    uint16_t m_session_id = 1;
    uint32_t m_next_ticket = 1;

    void LogVersionMsg(ControlLogLevel log_level, const char* str);

    // Reusable core execution logic
    CommandResult ExecuteCommand(ControlCommandId cmd, const uint8_t* req_payload, mpsize_t req_size);

    // Lightweight GPRX formatting and parsing
    using ResizeCallback = ntd::function<uint8_t*(mpsize_t)>;
    mpsize_t FormatMessage(
        uint16_t session_id, uint32_t ticket_id,
        bool is_response, ControlCommandId cmd,
        const uint8_t* fb_data, mpsize_t fb_size,
        uint8_t* outbuf, mpsize_t outbuf_max_size,
        ResizeCallback outbuf_resize_callback = nullptr);

    ParsedControlMessage ParseMessage(const uint8_t* buffer, mpsize_t size);

    template <ControlCommandId Command>
    bool SendCommand(
        const uint8_t* request_payload, mpsize_t request_size,
        ControlDetail::ResponseView<Command>& response);

    template <ControlCommandId Command>
    mpsize_t ReceiveCommand(
        const ParsedControlMessage& message,
        uint8_t* response_buffer,
        mpsize_t response_capacity,
        CommandResult (ControlChannel::*handler)(
            const typename ControlDetail::CommandTraits<Command>::Request&));

#define LIBMP_CONTROL_HANDLER_DECL(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    CommandResult Handle##NAME(const TypesNtd::REQUEST& request);
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_HANDLER_DECL)
#undef LIBMP_CONTROL_HANDLER_DECL

public:
    ControlChannel();
    ~ControlChannel() = default;

    void AssignIO(const ExternalControlIO& io);
    void SetSessionId(uint16_t session_id) { m_session_id = session_id; }

    // Command Receiver
    mpsize_t OnCommand(const uint8_t* cmdBuffer, mpsize_t cmdSize, uint8_t* respBuffer, mpsize_t respMaxSize);

    bool FetchRemoteVersions();
    static bool AreVersionsCompatible();
    static bool IsBackendAccessible() { return !s_error_code; }
    static uint32_t GetErrorCode() { return s_error_code; }

    // Send methods clear the response on failure. Response pointers remain valid until
    // the next send or version fetch.
#define LIBMP_CONTROL_SENDER_DECL_Struct(NAME, REQUEST)                \
    bool Send##NAME(                                                   \
        const TypesNtd::REQUEST& request,                              \
        ControlDetail::ResponseView<ControlCommandId::NAME>& response);
#define LIBMP_CONTROL_SENDER_DECL_Table(NAME, REQUEST)                 \
    bool Send##NAME(                                                   \
        const TypesNtd::REQUEST##T& request,                           \
        ControlDetail::ResponseView<ControlCommandId::NAME>& response);
#define LIBMP_CONTROL_SENDER_DECL(ID, NAME, REQUEST, REQUEST_KIND, RESPONSE, RESPONSE_KIND) \
    LIBMP_CONTROL_SENDER_DECL_##REQUEST_KIND(NAME, REQUEST)
    LIBMP_CONTROL_COMMANDS(LIBMP_CONTROL_SENDER_DECL)
#undef LIBMP_CONTROL_SENDER_DECL
#undef LIBMP_CONTROL_SENDER_DECL_Table
#undef LIBMP_CONTROL_SENDER_DECL_Struct
};

} // LibMP

#undef LIBMP_CONTROL_COMMANDS
#undef LIBMP_CONTROL_HAS_TABLE_PAYLOADS

////////////////////////////////////////////////////////////////////////////////////////////////////
// Externally provided control channel I/O
extern "C"
{

uint32_t LibMpExtControlMsg(const uint8_t* msg_buf, mpsize_t msg_sz, uint8_t* resp_buf, mpsize_t resp_max_sz);
void LibMpExtConsoleLog(uint32_t log_level, const char* str, mpsize_t len);

}
