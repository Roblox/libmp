// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "c_api_private.h"
#include "control_channel.h"

namespace
{
// Namespace scope gives the singleton automatic teardown without the
// __cxa_guard_* dependency produced by a non-trivial function-local static.
ntd::unique_ptr<LibMP::ControlChannel> g_controlChannel;
LibMP::ControlChannel* GetControlChannel()
{
    return g_controlChannel.get();
}

template <typename Request>
uint32_t SendCommandWithBoolResponse(
    LibMP::ControlChannel& control,
    bool (LibMP::ControlChannel::*send)(
        const Request&, const LibMP::TypesNtd::MsgBool*&),
    const Request& request)
{
    const LibMP::TypesNtd::MsgBool* response = nullptr;
    return GPRX::bool_to_int(
        (control.*send)(request, response) && response && response->value());
}
} // namespace

LibMP::ControlChannel* MpGetControlChannel()
{
    return GetControlChannel();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
inline static void SessionSetFullSyncRequired(MpSession* session)
{
    if (session)
    {
        MpSetClearCacheOnSyncRequired(session);
        MpSetSyncRequired(session);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t MpUseControlChannel(uint32_t use)
{
    if (use && !GetControlChannel())
    {
        g_controlChannel = ntd::make_unique<LibMP::ControlChannel>();
        g_controlChannel->FetchRemoteVersions();
    }
    else if (!use && GetControlChannel())
    {
        g_controlChannel.reset();
    }
    return 1;
}

uint32_t MpIsBackendVersionCompatible()
{
    if (auto* control = GetControlChannel())
        return GPRX::bool_to_int(control->AreVersionsCompatible());
    return 1;
}

uint32_t MpIsBackendAccessible()
{
    if (auto* control = GetControlChannel())
        return GPRX::bool_to_int(control->IsBackendAccessible());
    return 1;
}

uint32_t MpIsBackendReady()
{
    return MpIsBackendVersionCompatible() && MpIsBackendAccessible();
}

uint32_t MpControlEnableProfiler(MpSession* session, uint32_t enable)
{
    if (auto* control = GetControlChannel())
    {
        SessionSetFullSyncRequired(session);
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendEnableProfiler,
            LibMP::TypesNtd::MsgBool(!!enable));
    }
    return 0;
}

uint32_t MpControlEnableCapture(MpSession* /*session*/, uint32_t enable)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendEnableCapture,
            LibMP::TypesNtd::MsgBool(!!enable));
    return 0;
}

uint32_t MpControlShowUI(MpSession* /*session*/, uint32_t show)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendShowUI,
            LibMP::TypesNtd::MsgBool(!!show));
    return 0;
}

uint32_t MpControlSetFrameLimit(MpSession* session, int32_t num)
{
    if (auto* control = GetControlChannel())
    {
        SessionSetFullSyncRequired(session);
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendSetFrameLimit,
            LibMP::TypesNtd::MsgI32(num));
    }
    return 0;
}

uint32_t MpControlSetMemTargetMB(MpSession* session, int32_t targetMB)
{
    if (auto* control = GetControlChannel())
    {
        SessionSetFullSyncRequired(session);
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendSetMemTargetMB,
            LibMP::TypesNtd::MsgI32(targetMB));
    }
    return 0;
}

uint32_t MpControlCaptureToSlot(
    MpSession* /*session*/,
    uint32_t targetSlotId,
    uint32_t maxFrameCount,
    uint32_t containerFormat)
{
    if (auto* control = GetControlChannel())
    {
        const LibMP::TypesNtd::CaptureToSlotRequest request(
            targetSlotId,
            maxFrameCount,
            static_cast<LibMP::TypesNtd::ContainerFormat>(containerFormat));
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendCaptureToSlot, request);
    }
    return 0;
}

uint32_t MpControlResetSlot(MpSession* /*session*/, uint32_t slotId)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendResetSlot,
            LibMP::TypesNtd::MsgU32(slotId));
    return 0;
}

uint32_t MpControlExperimentalV1StartFileStream(
    const char* filenamePostfix, mpsize_t filenamePostfixSize, uint32_t targetSlotId)
{
    if (!filenamePostfix && filenamePostfixSize != 0)
        return 0;

    if (auto* control = GetControlChannel())
    {
        LibMP::TypesNtd::ExperimentalFileStreamStartRequestT request;
        if (filenamePostfixSize != 0)
            request.filename_postfix.assign(filenamePostfix, filenamePostfixSize);
        request.target_slot_id = targetSlotId;
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendExperimentalV1StartFileStream, request);
    }
    return 0;
}

uint32_t MpControlExperimentalV1StartMemoryStream(uint32_t targetSlotId)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendExperimentalV1StartMemoryStream,
            LibMP::TypesNtd::ExperimentalMemoryStreamStartRequest(targetSlotId));
    return 0;
}

uint32_t MpControlExperimentalV1StopStream()
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendExperimentalV1StopStream,
            LibMP::TypesNtd::MsgBool(false));
    return 0;
}

uint32_t MpControlExperimentalV1SetStreamMarker(uint32_t marker, uint32_t enabled)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendExperimentalV1SetStreamMarker,
            LibMP::TypesNtd::ExperimentalStreamMarkerRequest(marker, !!enabled));
    return 0;
}

const uint8_t* MpControlExperimentalV1GetStreamStatus()
{
    if (auto* control = GetControlChannel())
    {
        const LibMP::TypesNtd::ExperimentalStreamStatus* response = nullptr;
        if (control->SendExperimentalV1GetStreamStatus(LibMP::TypesNtd::MsgBool(false), response))
            return reinterpret_cast<const uint8_t*>(response);
    }
    return nullptr;
}

uint32_t MpControlExperimentalV1SetContentMode(uint32_t contentMode)
{
    if (auto* control = GetControlChannel())
        return SendCommandWithBoolResponse(
            *control, &LibMP::ControlChannel::SendExperimentalV1SetContentMode,
            LibMP::TypesNtd::ExperimentalContentModeRequest(
                static_cast<LibMP::TypesNtd::ContentMode>(contentMode)));
    return 0;
}
