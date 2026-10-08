// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include <cstddef>
#include "dependencies/support/config.h"

namespace GPRX::lz4
{

// Forward declarations for LZ4 opaque structs and options
struct LZ4F_dctx_s;
typedef LZ4F_dctx_s LZ4F_dctx;

struct LZ4F_decompressOptions_t;

#if !CFG_EXCLUDE_LZ4_ENCODER
struct LZ4F_cctx_s;
typedef LZ4F_cctx_s LZ4F_cctx;
struct LZ4F_preferences_t;
struct LZ4F_compressOptions_t;
#endif

typedef size_t LZ4F_errorCode_t;

// API Structure
struct Lz4Api
{
#if !CFG_EXCLUDE_LZ4_ENCODER
    size_t headerSizeMax;
    size_t (*compressBound)(size_t srcSize, const LZ4F_preferences_t* prefs);
    LZ4F_errorCode_t (*createCompressionContext)(LZ4F_cctx** cctxPtr, unsigned version);
    LZ4F_errorCode_t (*freeCompressionContext)(LZ4F_cctx* cctx);
    size_t (*compressBegin)(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const LZ4F_preferences_t* prefs);
    size_t (*compressUpdate)(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const void* srcBuffer, size_t srcSize, const LZ4F_compressOptions_t* cOpt);
    size_t (*compressEnd)(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const LZ4F_compressOptions_t* cOpt);
#endif
    LZ4F_errorCode_t (*createDecompressionContext)(LZ4F_dctx** dctxPtr, unsigned version);
    LZ4F_errorCode_t (*freeDecompressionContext)(LZ4F_dctx* dctx);
    size_t (*decompress)(LZ4F_dctx* dctx, void* dstBuffer, size_t* dstSizePtr, const void* srcBuffer, size_t* srcSizePtr, const LZ4F_decompressOptions_t* dOpt);
    unsigned (*isError)(size_t code);
};

extern Lz4Api g_Lz4Api;

// Sets up external pointers and constants
void InitializeLz4Api(const Lz4Api& api);

// Bind embedded LZ4 to the API struct
void InitEmbeddedLz4();

// Transparently forward calls through the selected LZ4 implementation.
#if !CFG_EXCLUDE_LZ4_ENCODER
inline size_t GetFrameHeaderSizeMax() { return g_Lz4Api.headerSizeMax; }
inline size_t LZ4F_compressBound(size_t srcSize, const LZ4F_preferences_t* prefs) { return g_Lz4Api.compressBound(srcSize, prefs); }
inline LZ4F_errorCode_t LZ4F_createCompressionContext(LZ4F_cctx** cctxPtr, unsigned version) { return g_Lz4Api.createCompressionContext(cctxPtr, version); }
inline LZ4F_errorCode_t LZ4F_freeCompressionContext(LZ4F_cctx* cctx) { return g_Lz4Api.freeCompressionContext(cctx); }
inline size_t LZ4F_compressBegin(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const LZ4F_preferences_t* prefs) { return g_Lz4Api.compressBegin(cctx, dstBuffer, dstCapacity, prefs); }
inline size_t LZ4F_compressUpdate(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const void* srcBuffer, size_t srcSize, const LZ4F_compressOptions_t* cOpt) { return g_Lz4Api.compressUpdate(cctx, dstBuffer, dstCapacity, srcBuffer, srcSize, cOpt); }
inline size_t LZ4F_compressEnd(LZ4F_cctx* cctx, void* dstBuffer, size_t dstCapacity, const LZ4F_compressOptions_t* cOpt) { return g_Lz4Api.compressEnd(cctx, dstBuffer, dstCapacity, cOpt); }
#endif
inline LZ4F_errorCode_t LZ4F_createDecompressionContext(LZ4F_dctx** dctxPtr, unsigned version) { return g_Lz4Api.createDecompressionContext(dctxPtr, version); }
inline LZ4F_errorCode_t LZ4F_freeDecompressionContext(LZ4F_dctx* dctx) { return g_Lz4Api.freeDecompressionContext(dctx); }
inline size_t LZ4F_decompress(LZ4F_dctx* dctx, void* dstBuffer, size_t* dstSizePtr, const void* srcBuffer, size_t* srcSizePtr, const LZ4F_decompressOptions_t* dOpt) { return g_Lz4Api.decompress(dctx, dstBuffer, dstSizePtr, srcBuffer, srcSizePtr, dOpt); }
inline unsigned LZ4F_isError(size_t code) { return g_Lz4Api.isError(code); }

#if !CFG_EXCLUDE_LZ4_ENCODER
struct CctxDeleter
{
    void operator()(LZ4F_cctx* ctx) const
    {
        if (ctx)
            g_Lz4Api.freeCompressionContext(ctx);
    }
};
#endif

struct DctxDeleter
{
    void operator()(LZ4F_dctx* ctx) const
    {
        if (ctx)
            g_Lz4Api.freeDecompressionContext(ctx);
    }
};

} // namespace GPRX::lz4
