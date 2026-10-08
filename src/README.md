# LibMP

LibMP is a high-performance library for handling profiling data in real time and offline. It supports multiple languages and platforms and uses a zero-copy approach.

* **`gprx_container/`** A universal block-based, streamable data container (codename **GPRX**). It holds arbitrary blobs and maintains links to them via a persistent key-value store.

* **`mp_api/`** A specialized API that adapts the generic container for MicroProfiler-specific data structures and workflows. The C API is the core, and other languages, including C++, wrap it.

* **`ntd_stdlib/`** A thin STL compatibility layer supporting standard containers with either their default or replacement allocators, plus reserved extension points for standalone implementations.

* **`scripts/`** Build scripts. Start with `python3 scripts/build.py --help`; see
  `scripts/BUILDING.txt` for the full guide.

LibMP embeds LZ4 for block compression and uses generated FlatBuffers structures for cross-language compatibility. Builds can select platform, embedded, or host-provided memory allocation backends.
