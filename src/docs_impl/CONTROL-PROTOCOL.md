# LibMP Control Protocol

The control channel is how a Luau script drives the engine's profiler backend
(enable/disable profiling and capture, show the UI, set the frame limit). It uses a
documented binary protocol with explicit framing and schema-defined payloads that are
validated before access. The implementation is `src/mp_api/control_channel.{h,cpp}`.

## Framing

Every message is a **raw GPRX block header** immediately followed by a payload declared in
the FlatBuffers schema. Payloads may be fixed-size structs or variable-size tables. There is
exactly one encoding — no ASCII/hex header variant and no compressed or Base64 payload are
accepted.

```
+-----------------------------+------------------------------+
| raw BlockHeader (32 bytes)  | schema payload (N bytes)     |
+-----------------------------+------------------------------+
```

Header fields used by the protocol (`GPRX::BlockHeader`, all little-endian):

| Field | Meaning on this channel |
|-------|-------------------------|
| `metadata0` | `MagicFormatRawFull` (raw magic). A hex/ASCII magic is rejected. |
| `metadata1` | Packed routing: bits 0–15 = command id, bits 16–31 = session id, bits 32–63 = ticket. |
| `type_id` | `MagicTmp`; any other value is rejected. |
| `type_meta` | `0` = request, `1` = response. |
| `encoding_method` | `Raw`; any other value is rejected. |
| `payload_size` | Exact length in bytes of the schema payload that follows. |
| `payload_uncompressed_size` | Must equal `payload_size`. |

The receiver first rejects oversized messages and copies the complete input into an
internal aligned buffer. Header validation, payload validation, and callback execution
therefore use one immutable snapshot. It then requires at least a 32-byte header, canonical
raw magic, block id `1`, a request/response flag of exactly `0` or `1`, and
`payload_size == payload_uncompressed_size == size - 32` (no truncation or trailing-byte
ambiguity). Fixed structs require their exact generated size and alignment before their
accessors view the immutable snapshot; builds that accept untrusted input pass tables through
`flatbuffers::Verifier::VerifyBuffer<ExactType>()` before field access.

The surrounding engine-facing buffer offsets, capacities, and overlap rules are defined
by reusable validators in `mp_api/integration/service_boundary.h`. This lets
the engine bridge and the standalone tests/fuzzer execute the same checks.

## Commands

The command set is declared next to its payload types in
`mp_api/flatbuffer_types/mp_types.fbs`:

```fbs
/* @control_command id=10 name=EnableProfiler request=MsgBool response=MsgBool */
```

`scripts/gen_flatbuffers.py --cpp` validates these annotations and generates a compact
command registry. Compiler-checked templates beside `ControlChannel` expand that registry
into command ids, request/response type traits, public send methods, host receiver slots,
handlers, and receive-side dispatch. Adding a command therefore does not require
repeating its protocol metadata in C++. The generated host slot is named
`ExternalControlIO::On<Name>`. Malformed annotations, duplicate names or ids,
and unknown types fail generation. The generated C++ API always uses complete schema
types: callbacks receive wire requests and fill a wire struct or generated native
table response. A callback returns `true` only when the operation succeeds; `false` aborts
the response and makes the originating sender fail. Senders accept wire structs or native
table objects, return whether a valid response was received, and output a wire view valid
until the next send or version fetch on that channel.

Example commands:

| Command | Id | Request | Response |
|---------|----|---------|----------|
| EnableProfiler | 10 | `MsgBool` struct | `MsgBool` struct |
| EnableCapture | 20 | `MsgBool` struct | `MsgBool` struct |
| ShowUI | 30 | `MsgBool` struct | `MsgBool` struct |
| SetFrameLimit | 40 | `MsgI32` struct | `MsgBool` struct |

Most responses are a `MsgBool` whose `value` is the command's success flag. The generated
processor applies the validation appropriate to each annotated type and passes only a
validated typed request to the corresponding generated receiver slot. The slot's assigned
callback remains the host's business logic.

## Handshake

A **zero-length request** is the handshake / "get versions" trigger. The receiver replies
with a fixed `HandshakeResp { Version remote_versions; uint32 error_code; }`. This is the
only message that is not a header+table frame; it is a fixed-size struct written into a
capacity-checked buffer, so it is safe.

## Versioning and compatibility

`Version` carries four `major.minor` numbers: `data_format`, `control_channel`, `library`,
and `library_warning`. On handshake the client compares its local versions against the
host's:

- **Major mismatch** on the library version → incompatible; the client refuses to operate
  and logs that the module must be replaced.
- **Local library older than the host's `library_warning`** → compatible, but the client
  logs that an upgrade is recommended.
- A non-zero `error_code` from the host means the backend is unavailable.

Command ids, payload layouts, and routing metadata are part of the wire contract. An
incompatible control-channel change requires bumping its major version so the handshake
rejects incompatible peers. A capture-container framing change separately requires a
`data_format` version bump. Adding a backward-compatible command bumps the control-channel
minor version when deployed.

## Validation summary (receiver)

`ControlChannel::OnCommand` rejects (returns 0, invokes no callback) when any of the
following do not hold, in order: response buffer non-null and 4-byte aligned; for a
non-handshake frame — input size at most 4 KiB, complete snapshot copy, at least a 32-byte
header, raw magic, raw header serialization, `type_id == MagicTmp`, `encoding_method ==
Raw`, exact payload framing, a known command id, and schema-generated payload validation.
The response is validated as its annotated type and only written if it fits the caller's
buffer. The client accepts a response only if its payload type, session id, ticket, and
command id match the request, and it parses exactly the number of bytes the transport
reported.
