// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Generated from @control_command annotations in mp_types.fbs.
// Do not edit directly; run scripts/gen_flatbuffers.py --cpp.
// Contains schema facts; expansion logic lives in control_channel.h/.cpp.
// Command APIs use the complete generated struct or table types.

#define LIBMP_CONTROL_HAS_TABLE_PAYLOADS 1

#define LIBMP_CONTROL_COMMANDS(X)                                                                            \
    X(10, EnableProfiler, MsgBool, Struct, MsgBool, Struct)                                                  \
    X(20, EnableCapture, MsgBool, Struct, MsgBool, Struct)                                                   \
    X(30, ShowUI, MsgBool, Struct, MsgBool, Struct)                                                          \
    X(40, SetFrameLimit, MsgI32, Struct, MsgBool, Struct)                                                    \
    X(50, CaptureToSlot, CaptureToSlotRequest, Struct, MsgBool, Struct)                                      \
    X(60, ResetSlot, MsgU32, Struct, MsgBool, Struct)                                                        \
    X(70, SetMemTargetMB, MsgI32, Struct, MsgBool, Struct)                                                   \
    X(10001, ExperimentalV1StartFileStream, ExperimentalFileStreamStartRequest, Table, MsgBool, Struct)      \
    X(10002, ExperimentalV1StopStream, MsgBool, Struct, MsgBool, Struct)                                     \
    X(10003, ExperimentalV1SetStreamMarker, ExperimentalStreamMarkerRequest, Struct, MsgBool, Struct)        \
    X(10004, ExperimentalV1GetStreamStatus, MsgBool, Struct, ExperimentalStreamStatus, Struct)               \
    X(10006, ExperimentalV1StartMemoryStream, ExperimentalMemoryStreamStartRequest, Struct, MsgBool, Struct) \
    X(10007, ExperimentalV1SetContentMode, ExperimentalContentModeRequest, Struct, MsgBool, Struct)
