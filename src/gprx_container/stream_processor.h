// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "utils_ex.h"
#include "lz4frame_wrapper.h"
#include "ntd_stdlib/memory.h"
#include "ntd_stdlib/string.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/vector.h"

#ifndef EXCLUDE_FILE_STREAM_PROCESSOR
    #include "ntd_stdlib/file_io.h" // ntd::fstream (OS-level or std:: alias per --ntd-fileio)
#endif

namespace GPRX
{

class DataContextFile;

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Abstract interface for a streaming data processor (e.g., encoder, decoder, compressor, decompressor).
 */
class IStreamProcessor : public ntd::mem::RoutedAllocation
{
protected:
    mpdatasz_t m_bytes_written = 0; // Total bytes produced by this processor

    bool m_ignore_propagated_calls = false;
    bool m_owns_next_stage = false;
    IStreamProcessor* m_next_stage = nullptr;
    mpdatasz_t m_next_stage_orig_written_bytes = 0;

public:
    enum class Direction
    {
        Encode,
        Decode
    };

    IStreamProcessor(IStreamProcessor* next_stage = nullptr)
        : m_next_stage(next_stage)
    {
        m_next_stage_orig_written_bytes = next_stage ? next_stage->GetBytesWritten() : 0;
    }

    virtual ~IStreamProcessor()
    {
        if (m_owns_next_stage && m_next_stage)
            ntd::Delete(m_next_stage);
    }

    virtual void SetIgnorePropagatedCalls(bool x)
    {
        m_ignore_propagated_calls = x;
    }

    virtual void SetOwnsNextStage(bool x)
    {
        m_owns_next_stage = x;
    }

    /**
     * @brief Resets the processor's internal state for a new stream.
     */
    virtual void BeginStream(bool propagate_to_next_stage = false)
    {
        if (propagate_to_next_stage && m_ignore_propagated_calls)
            return;
        m_bytes_written = 0;
        if (propagate_to_next_stage && m_next_stage)
            m_next_stage->BeginStream(true);
        m_next_stage_orig_written_bytes = m_next_stage ? m_next_stage->GetBytesWritten() : 0;
    }

    virtual bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) = 0;

    virtual bool EndStream(bool propagate_to_next_stage = false)
    {
        if (propagate_to_next_stage && m_ignore_propagated_calls)
            return true;
        if (propagate_to_next_stage && m_next_stage)
            return m_next_stage->EndStream(true);
        return true;
    }
    
    virtual bool FlushStream(bool propagate_to_next_stage = false)
    {
        if (propagate_to_next_stage && m_ignore_propagated_calls)
            return true;
        if (propagate_to_next_stage && m_next_stage)
            return m_next_stage->FlushStream(true);
        return true;
    }
    
    virtual mpdatasz_t GetBytesWritten() const
    {
        return m_next_stage ? m_next_stage->GetBytesWritten() - m_next_stage_orig_written_bytes : m_bytes_written;
    }

    virtual mpdatasz_t GetOwnBytesWritten() const
    {
        return m_bytes_written;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A processor that passes data to a next stage or writes to a memory buffer.
 */
class PassThroughProcessor : public IStreamProcessor
{
protected:
    uint8_t* m_dest_buffer = nullptr;
    mpsize_t m_dest_capacity = 0;

public:
    static ntd::vector<uint8_t> ComposeChunks(const MemChunkList& chunks, mpsize_t size);

    explicit PassThroughProcessor(IStreamProcessor* next_stage = nullptr)
        : IStreamProcessor(next_stage)
    {}

    /**
     * @brief Sets a memory buffer as the final destination (for single-stage use).
     */
    void SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity);
    uint8_t* ProcessChunkManually(mpsize_t input_chunk_size);
    bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) override;
};

#ifndef EXCLUDE_FILE_STREAM_PROCESSOR

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A stream processor that acts as a final destination, writing all data it receives to a file.
 * It uses a two-step initialization and does not throw exceptions.
 */
class StreamProcessorFile : public IStreamProcessor
{
private:
    ntd::fstream m_file;
    bool m_write_active = false;
    bool m_retain_file_on_end = false;

public:
    StreamProcessorFile() = default;
    ~StreamProcessorFile() override;

    /**
     * @brief Opens the output file for writing. This must be called before processing data.
     * @param filepath The path to the output file.
     * @return true if the file was opened successfully.
     */
    bool Open(const ntd::string& filepath);
    bool IsOpen() const { return m_file.is_open(); }
    void SetRetainFileOnEnd(bool retain) { m_retain_file_on_end = retain; }
    bool TransferFileTo(DataContextFile& destination);

    bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) override;
    bool EndStream(bool propagate_to_next_stage = false) override;
    bool FlushStream(bool propagate_to_next_stage = false) override;
};

#endif //ifndef EXCLUDE_FILE_STREAM_PROCESSOR

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A stream processor that writes incoming data into a series of fixed-size memory chunks.
 */
class StreamProcessorChunkedMemory : public IStreamProcessor
{
private:
    static constexpr mpsize_t OUTPUT_CHUNK_SIZE = 64 * 1024;

    MemChunkList m_chunks;
    const mpsize_t m_chunk_size;

    bool AllocateNewChunk();

public:
    explicit StreamProcessorChunkedMemory(mpsize_t chunk_size = OUTPUT_CHUNK_SIZE) 
        : m_chunk_size(chunk_size ? chunk_size : 1) {}

    void BeginStream(bool propagate_to_next_stage = false) override;
    bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) override;

    /**
     * @brief Retrieves the list of all populated chunks and transfers ownership to the caller.
     * After this call, the processor's internal chunk list is cleared.
     */
    MemChunkList GetChunks() { return ntd::move(m_chunks); }

    /**
     * @brief Combines all chunks into a newly allocated solid buffer.
     */
    ntd::vector<uint8_t> GetComposedChunks();
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A streaming processor for LZ4 Frame compression and decompression.
 * It writes directly to a final destination or uses a scratch buffer when chained.
 */
class LZ4Processor : public IStreamProcessor
{
private:
    static constexpr mpsize_t DECODE_BUF_SIZE = 16 * 1024;
#if !CFG_EXCLUDE_LZ4_ENCODER
    static constexpr mpsize_t LZ4_BLOCK_SIZE = 64 * 1024;
    static constexpr mpsize_t ENCODE_INPUT_MAX_SIZE = LZ4_BLOCK_SIZE;
#endif

    Direction m_direction;

    // --- Destination for single-stage operation ---
    uint8_t* m_dest_buffer = nullptr;
    mpsize_t m_dest_capacity = 0;

    // --- LZ4 Contexts ---
#if !CFG_EXCLUDE_LZ4_ENCODER
    ntd::unique_ptr<lz4::LZ4F_cctx, lz4::CctxDeleter> m_cctx;
#endif
    ntd::unique_ptr<lz4::LZ4F_dctx, lz4::DctxDeleter> m_dctx;
    
    // Scratch buffer is ONLY used for multi-stage (chaining) operations.
    ntd::vector<uint8_t> m_scratch_buffer;
#if !CFG_EXCLUDE_LZ4_ENCODER
    ntd::vector<uint8_t> m_header_buffer;
#endif

    bool m_decode_complete = false;
    bool m_stream_valid = false;

    // Private methods for each direction
#if !CFG_EXCLUDE_LZ4_ENCODER
    bool ProcessEncodeChunk(const uint8_t* chunk, mpsize_t size);
    bool EndEncodeStream();
#endif
    bool ProcessDecodeChunk(const uint8_t* chunk, mpsize_t size);
    bool EndDecodeStream();

public:
    explicit LZ4Processor(Direction dir, IStreamProcessor* next_stage = nullptr)
        : IStreamProcessor(next_stage), m_direction(dir) {}

    /**
     * @brief Sets a memory buffer as the final destination (for single-stage use).
     */
    void SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity);

    void BeginStream(bool propagate_to_next_stage = false) override;
    bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) override;
    bool EndStream(bool propagate_to_next_stage = false) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A streaming processor for Base64 encoding and decoding.
 * It writes directly to a final destination or uses a fixed size buffer when chained.
 */
class Base64Processor : public IStreamProcessor
{
protected:
    uint8_t* m_dest_buffer = nullptr;
    mpsize_t m_dest_capacity = 0;

private:
    static constexpr mpsize_t OUTPUT_BUFFER_SIZE = 8 * 1024;

    Direction m_direction;
    ntd::string m_leftover_chars;
    ntd::vector<uint8_t> m_output_buffer;
    bool m_decode_padding_seen = false;
    
    static const uint8_t DECODE_TABLE[256];
    static const char* ENCODE_TABLE;

    // --- Reusable Decode Helpers ---
    static inline uint8_t DecodeByte1(const uint32_t b[4]) { return to_uint8((b[0] << 2) | (b[1] >> 4)); }
    static inline uint8_t DecodeByte2(const uint32_t b[4]) { return to_uint8(((b[1] & 0xF) << 4) | (b[2] >> 2)); }
    static inline uint8_t DecodeByte3(const uint32_t b[4]) { return to_uint8(((b[2] & 0x3) << 6) | b[3]); }
    
    // --- Reusable Encode Helpers ---
    static inline char EncodeChar1(const uint8_t b[3]) { return ENCODE_TABLE[ b[0] >> 2 ]; }
    static inline char EncodeChar2(const uint8_t b[3]) { return ENCODE_TABLE[ ((b[0] & 0x03) << 4) | (b[1] >> 4) ]; }
    static inline char EncodeChar3(const uint8_t b[3]) { return ENCODE_TABLE[ ((b[1] & 0x0f) << 2) | (b[2] >> 6) ]; }
    static inline char EncodeChar4(const uint8_t b[3]) { return ENCODE_TABLE[ b[2] & 0x3f ]; }

    inline const uint8_t* GetBytePtr(const uint8_t* input_chunk, mpsize_t cursor);
    inline void MoveToLeftoverBytes(mpsize_t needed_len, const uint8_t* &input_chunk, mpsize_t &input_chunk_size);

    // --- Private Dispatch Methods ---
    inline bool ProcessDecodeChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size);
    inline bool EndDecodeStream();
    inline bool ProcessEncodeChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size);
    inline bool EndEncodeStream();
    inline bool FlushOutputBuffer();

public:
    explicit Base64Processor(Direction dir, IStreamProcessor* next_stage = nullptr)
        : IStreamProcessor(next_stage), m_direction(dir) {}
    
    void SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity);
    void BeginStream(bool propagate_to_next_stage = false) override;
    bool ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size) override;
    bool EndStream(bool propagate_to_next_stage = false) override;
    bool FlushStream(bool propagate_to_next_stage = false) override;
};

} // GPRX
