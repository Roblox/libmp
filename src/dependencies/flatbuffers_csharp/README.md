# FlatBuffers C# runtime subset

These files are the C# runtime sources from the FlatBuffers release pinned by
[`../flatbuffers_version.txt`](../flatbuffers_version.txt). They are compiled
directly into the C# source project and concatenated into the generated
single-file library. The upstream Apache 2.0 license is in
[`../flatbuffers/LICENSE`](../flatbuffers/LICENSE).

Run `scripts/update_flatbuffers_core_src.py` when the pinned FlatBuffers core
version changes; it refreshes this directory together with the C++ headers.
