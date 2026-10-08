# Dependency patches

## LZ4

[`lz4_use_extern_c.patch`](lz4_use_extern_c.patch) records the modifications
made when LibMP vendors the pinned LZ4 sources. The
[`update_lz4_src.py`](../../scripts/update_lz4_src.py) update script makes the
same changes and regenerates the patch.

LibMP compiles the bundled LZ4 C sources as C++ inside the
`GPRX::lz4_impl` namespace. The patch makes LZ4's `extern "C"` guards
conditional on `LZ4PATCH_USE_EXTERN_C`, so the embedded implementation uses
C++ linkage and its symbols remain isolated from another LZ4 implementation
linked by the host. Defining `LZ4PATCH_USE_EXTERN_C` restores the upstream C
linkage behavior.
