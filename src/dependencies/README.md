# Dependencies

LibMP vendors or bundles the following third-party components.

## FlatBuffers

[FlatBuffers](https://github.com/google/flatbuffers) provides serialization
runtimes and generated types. The vendored C++ headers are under
[`flatbuffers/`](flatbuffers/), and the C# runtime subset is described in
[`flatbuffers_csharp/README.md`](flatbuffers_csharp/README.md). The pinned
versions are recorded in
[`flatbuffers_version.txt`](flatbuffers_version.txt).

FlatBuffers is distributed under the
[Apache License 2.0](flatbuffers/LICENSE).

## LZ4

[LZ4](https://github.com/lz4/lz4) provides block compression. Its vendored
sources are under [`lz4/`](lz4/), and the pinned revision is recorded in
[`lz4_version.txt`](lz4_version.txt).

LZ4 is distributed under the
[BSD 2-Clause License](lz4/LICENSE).

## Spider Luau runtime

[Spider](https://github.com/SovereignSatellite/Spider) translates LibMP's
WebAssembly core to Luau and embeds parts of its runtime into the generated
Luau library. See [`spider_runtime/README.md`](spider_runtime/README.md) for
source information. The pinned Spider revision is recorded in
[`../scripts/build-config.json`](../scripts/build-config.json).

The Spider Luau runtime is distributed under the
[Mozilla Public License 2.0](spider_runtime/LICENSE).

## Local support code and patches

Local dependency patches are documented in
[`patches/README.md`](patches/README.md).

The [`support/`](support/) directory contains LibMP-owned integration helpers
covered by LibMP's root [MIT License](../../LICENSE).
