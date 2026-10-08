// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "stream_processor.h"

#include "data_context.h"

#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"

namespace GPRX
{

inline static bool GetMemoryWriteOffset(
    mpdatasz_t bytes_written,
    mpsize_t destination_capacity,
    mpsize_t requested_size,
    mpsize_t& out_offset)
{
    if (bytes_written > destination_capacity)
        return false;
    out_offset = to_mpsize(bytes_written);
    return requested_size <= destination_capacity - out_offset;
}

#if !CFG_EXCLUDE_LZ4_ENCODER
inline static bool GetLz4ResultSize(size_t result, mpsize_t& out_size)
{
    if (lz4::LZ4F_isError(result) || result > max_mpsize_t)
        return false;
    out_size = to_mpsize(result);
    return true;
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
void PassThroughProcessor::SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity)
{
    m_dest_buffer = destination_buffer;
    m_dest_capacity = destination_capacity;
}

uint8_t* PassThroughProcessor::ProcessChunkManually(mpsize_t input_chunk_size)
{
    mpsize_t write_offset = 0;
    if (!m_dest_buffer || !GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, input_chunk_size, write_offset))
        return nullptr;

    uint8_t* chunkStartPtr = m_dest_buffer + write_offset;
    m_bytes_written += input_chunk_size;

    return chunkStartPtr;
}

bool PassThroughProcessor::ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    if (!input_chunk && input_chunk_size > 0)
        return false;
    if (input_chunk_size == 0)
        return true;
    if (m_next_stage)
    {
        m_bytes_written += input_chunk_size;
        return m_next_stage->ProcessChunk(input_chunk, input_chunk_size);
    }

    uint8_t* chunkStartPtr = ProcessChunkManually(input_chunk_size);
    if (!chunkStartPtr)
        return false;
    ntd::memcpy(chunkStartPtr, input_chunk, input_chunk_size);
    return true;
}

ntd::vector<uint8_t> PassThroughProcessor::ComposeChunks(const MemChunkList& chunks, mpsize_t size)
{
    if (!chunks.empty() && size == 0)
    {
        for (const auto& chunk : chunks)
        {
            if (chunk.size() > max_mpsize_t - size)
                return {};
            size += chunk.size();
        }
    }

    if (size == 0)
        return {};

    ntd::vector<uint8_t> buffer(size);
    auto proc = ntd::make_unique<PassThroughProcessor>();
    proc->SetMemoryOutput(buffer.data(), size);
    proc->BeginStream();

    if (!ProcessChunkSequence(chunks, size,
        [&](const uint8_t* chunk_data, mpsize_t size_from_chunk) -> bool
        {
            return proc->ProcessChunk(chunk_data, size_from_chunk);
        }
    ))
    {
        return {};
    }

    if (!proc->EndStream())
        return {};

    return buffer;
}    

////////////////////////////////////////////////////////////////////////////////////////////////////
#ifndef EXCLUDE_FILE_STREAM_PROCESSOR

bool StreamProcessorFile::Open(const ntd::string& filepath)
{
    if (m_file.is_open())
        m_file.close();

    m_file.open(filepath.c_str(), ntd::ios::binary | ntd::ios::in | ntd::ios::out | ntd::ios::trunc);
    m_write_active = m_file.is_open();
    m_retain_file_on_end = false;
    return m_write_active;
}

StreamProcessorFile::~StreamProcessorFile()
{
    if (m_file.is_open())
        m_file.close();
}

bool StreamProcessorFile::ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    if (!m_write_active || (!input_chunk && input_chunk_size > 0))
        return false;
    if (input_chunk_size == 0)
        return true;

    mpsize_t total_written = 0;
    while (total_written < input_chunk_size)
    {
        const ntd::streamsize request_size = static_cast<ntd::streamsize>(ntd::min<uint64_t>(
            input_chunk_size - total_written,
            static_cast<uint64_t>(ntd::numeric_limits<ntd::streamsize>::max())));
        m_file.write(reinterpret_cast<const char*>(input_chunk + total_written), request_size);
        if (!m_file.good())
            return false;

        total_written += static_cast<mpsize_t>(request_size);
        m_bytes_written += static_cast<mpdatasz_t>(request_size);
    }
    return true;
}

bool StreamProcessorFile::EndStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return true;
    if (!m_write_active)
        return true;

    m_file.flush();
    m_write_active = false;
    if (!m_retain_file_on_end)
        m_file.close();
    return !m_file.fail();
}

bool StreamProcessorFile::FlushStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return true;
    if (!m_write_active)
        return true;

    m_file.flush();
    return !m_file.fail();
}

bool StreamProcessorFile::TransferFileTo(DataContextFile& destination)
{
    if (m_write_active || !m_retain_file_on_end || !m_file.is_open())
        return false;

    if (!destination.Adopt(ntd::move(m_file), m_bytes_written))
        return false;
    m_retain_file_on_end = false;
    return true;
}

#endif //ifndef EXCLUDE_FILE_STREAM_PROCESSOR

////////////////////////////////////////////////////////////////////////////////////////////////////
bool StreamProcessorChunkedMemory::AllocateNewChunk()
{
    m_chunks.emplace_back();
    auto& new_chunk = m_chunks.back();
    if (!new_chunk.Allocate(m_chunk_size))
    {
        m_chunks.resize(m_chunks.size() - 1);
        return false;
    }
    new_chunk.usedBytes = 0;
    return true;
}

void StreamProcessorChunkedMemory::BeginStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return;
    IStreamProcessor::BeginStream(propagate_to_next_stage);
    m_chunks.clear();
}

bool StreamProcessorChunkedMemory::ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    if (!input_chunk && input_chunk_size > 0)
        return false;
    const uint8_t* current_input_ptr = input_chunk;
    mpsize_t remaining_input_size = input_chunk_size;

    while (remaining_input_size > 0)
    {
        if (m_chunks.empty() || m_chunks.back().usedBytes == m_chunks.back().totalBytes)
        {
            if (!AllocateNewChunk())
                return false;
        }

        MemChunk& current_chunk = m_chunks.back();
        mpsize_t available_space = current_chunk.totalBytes - current_chunk.usedBytes;
        mpsize_t bytes_to_copy = ntd::min(remaining_input_size, available_space);

        ntd::memcpy(current_chunk.ptr + current_chunk.usedBytes, current_input_ptr, bytes_to_copy);
        
        current_chunk.usedBytes += bytes_to_copy;
        m_bytes_written += bytes_to_copy;
        current_input_ptr += bytes_to_copy;
        remaining_input_size -= bytes_to_copy;
    }
    return true;
}

ntd::vector<uint8_t> StreamProcessorChunkedMemory::GetComposedChunks()
{
    const mpdatasz_t size = GetBytesWritten();
    if (size > max_mpsize_t)
        return {};
    return PassThroughProcessor::ComposeChunks(m_chunks, to_mpsize(size));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void LZ4Processor::SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity)
{
    m_dest_buffer = destination_buffer;
    m_dest_capacity = destination_capacity;
}

void LZ4Processor::BeginStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return;

    IStreamProcessor::BeginStream(propagate_to_next_stage);
    m_stream_valid = false;
    m_decode_complete = false;
#if CFG_EXCLUDE_LZ4_ENCODER
    if (m_direction == Direction::Encode)
        return;
#endif
    if (m_next_stage)
    {
        // Only prepare the scratch buffer if we are in a multi-stage pipeline.
        mpsize_t required_scratch_size = DECODE_BUF_SIZE;
#if !CFG_EXCLUDE_LZ4_ENCODER
        if (m_direction == Direction::Encode)
        {
            const size_t compression_bound_wide =
                lz4::LZ4F_compressBound(ENCODE_INPUT_MAX_SIZE, nullptr);
            if (compression_bound_wide > max_mpsize_t)
                return;
            const mpsize_t compression_bound = to_mpsize(compression_bound_wide);
            required_scratch_size = ntd::max(ENCODE_INPUT_MAX_SIZE, compression_bound);
        }
#endif
        resize_if_needed(m_scratch_buffer, required_scratch_size);
    }

#if !CFG_EXCLUDE_LZ4_ENCODER
    if (m_direction == Direction::Encode)
    {
        if (!m_cctx)
        {
            lz4::LZ4F_cctx* cctx = nullptr;
            const size_t create_result = lz4::LZ4F_createCompressionContext(&cctx, 100);
            if (lz4::LZ4F_isError(create_result) || !cctx)
                return;
            m_cctx.reset(cctx);
        }
        
        const size_t header_capacity_wide = lz4::GetFrameHeaderSizeMax();
        if (header_capacity_wide > max_mpsize_t)
            return;
        resize_if_needed(m_header_buffer, to_mpsize(header_capacity_wide));
        const size_t header_result = lz4::LZ4F_compressBegin(
            m_cctx.get(), m_header_buffer.data(), m_header_buffer.size(), nullptr);
        mpsize_t header_size = 0;
        if (!GetLz4ResultSize(header_result, header_size))
            return;
        
        if (m_next_stage)
        {
            if (!m_next_stage->ProcessChunk(m_header_buffer.data(), header_size))
                return;
        }
        else if (m_dest_buffer)
        {
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, header_size, write_offset))
                return;
            ntd::memcpy(m_dest_buffer + write_offset, m_header_buffer.data(), header_size);
        }
        else
        {
            return;
        }

        m_bytes_written += header_size;
        m_stream_valid = true;
    }
    else // Decode
#endif
    {
        lz4::LZ4F_dctx* dctx = nullptr;
        const size_t create_result = lz4::LZ4F_createDecompressionContext(&dctx, 100);
        if (lz4::LZ4F_isError(create_result) || !dctx)
            return;
        m_dctx.reset(dctx);
        m_stream_valid = true;
    }
}

bool LZ4Processor::ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    if (!m_stream_valid || (!input_chunk && input_chunk_size > 0))
        return false;
#if !CFG_EXCLUDE_LZ4_ENCODER
    return (m_direction == Direction::Encode)
        ? ProcessEncodeChunk(input_chunk, input_chunk_size)
        : ProcessDecodeChunk(input_chunk, input_chunk_size);
#else
    return ProcessDecodeChunk(input_chunk, input_chunk_size);
#endif
}

bool LZ4Processor::EndStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return true;
    if (!m_stream_valid)
        return false;

#if !CFG_EXCLUDE_LZ4_ENCODER
    bool res = (m_direction == Direction::Encode)
        ? EndEncodeStream()
        : EndDecodeStream();
#else
    bool res = EndDecodeStream();
#endif
    if (!res)
        return false;
    return (propagate_to_next_stage && m_next_stage) ? m_next_stage->EndStream(true) : true;
}

// --- Encoding (compression) ---
#if !CFG_EXCLUDE_LZ4_ENCODER
// @amalgamate: config-begin lz4-encoder !CFG_EXCLUDE_LZ4_ENCODER
bool LZ4Processor::ProcessEncodeChunk(const uint8_t* chunk, mpsize_t size)
{
    const uint8_t* current_ptr = chunk;
    mpsize_t remaining_size = size;

    // Loop through the large input chunk, feeding it to the compressor in smaller, manageable slices.
    while (remaining_size > 0)
    {
        // Determine the size of the input slice for this iteration.
        mpsize_t input_slice_size = ntd::min(remaining_size, ENCODE_INPUT_MAX_SIZE);
        size_t compressed_result;

        if (m_next_stage)
        {
            // Multi-stage: compress into scratch buffer, then pass to next stage.
            compressed_result = lz4::LZ4F_compressUpdate(
                m_cctx.get(), m_scratch_buffer.data(), m_scratch_buffer.size(),
                current_ptr, input_slice_size, nullptr);
        }
        else
        {
            // Single-stage: compress DIRECTLY into destination buffer.
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, 0, write_offset))
                return false;
            mpsize_t out_capacity = m_dest_capacity - write_offset;
            compressed_result = lz4::LZ4F_compressUpdate(
                m_cctx.get(), m_dest_buffer + write_offset, out_capacity,
                current_ptr, input_slice_size, nullptr);
        }

        mpsize_t compressed_size = 0;
        if (!GetLz4ResultSize(compressed_result, compressed_size))
            return false;
        if (m_next_stage && compressed_size > 0 &&
            !m_next_stage->ProcessChunk(m_scratch_buffer.data(), compressed_size))
            return false;
        
        m_bytes_written += compressed_size;
        current_ptr += input_slice_size;
        remaining_size -= input_slice_size;
    }
    return true;
}

bool LZ4Processor::EndEncodeStream()
{
    size_t end_result;
    if (m_next_stage)
    {
        end_result = lz4::LZ4F_compressEnd(
            m_cctx.get(), m_scratch_buffer.data(), m_scratch_buffer.size(), nullptr);
    }
    else
    {
        mpsize_t write_offset = 0;
        if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, 0, write_offset))
            return false;
        mpsize_t out_capacity = m_dest_capacity - write_offset;
        end_result = lz4::LZ4F_compressEnd(
            m_cctx.get(), m_dest_buffer + write_offset, out_capacity, nullptr);
    }
    mpsize_t end_size = 0;
    if (!GetLz4ResultSize(end_result, end_size))
        return false;
    if (m_next_stage && end_size > 0 &&
        !m_next_stage->ProcessChunk(m_scratch_buffer.data(), end_size))
        return false;
    m_bytes_written += end_size;
    return true;
}
// @amalgamate: config-end lz4-encoder
#endif

// --- Decoding (decompression) ---
bool LZ4Processor::ProcessDecodeChunk(const uint8_t* chunk, mpsize_t size)
{
    const uint8_t* current_ptr = chunk;
    mpsize_t remaining_size = size;

    while (remaining_size > 0)
    {
        size_t src_consumed_size = static_cast<size_t>(remaining_size);
        size_t dst_produced_size;
        size_t decode_result;

        if (m_next_stage)
        {
            dst_produced_size = m_scratch_buffer.size();
            decode_result = lz4::LZ4F_decompress(m_dctx.get(), m_scratch_buffer.data(), &dst_produced_size, current_ptr, &src_consumed_size, nullptr);
        }
        else
        {
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, 0, write_offset))
                return false;
            dst_produced_size = m_dest_capacity - write_offset;
            decode_result = lz4::LZ4F_decompress(m_dctx.get(), m_dest_buffer + write_offset, &dst_produced_size, current_ptr, &src_consumed_size, nullptr);
        }

        if (lz4::LZ4F_isError(decode_result))
            return false;
        if (dst_produced_size > max_mpsize_t || src_consumed_size > remaining_size)
            return false;
        const mpsize_t produced_size = to_mpsize(dst_produced_size);
        const mpsize_t consumed_size = to_mpsize(src_consumed_size);
        m_decode_complete = decode_result == 0;
        if (m_next_stage && produced_size > 0 &&
            !m_next_stage->ProcessChunk(m_scratch_buffer.data(), produced_size))
            return false;

        m_bytes_written += produced_size;
        current_ptr += consumed_size;
        remaining_size -= consumed_size;

        // No forward progress (nothing consumed and nothing produced) means the
        // decompressor is stuck -- corrupt input, or a decompression bomb whose output
        // exceeds the destination. Fail instead of spinning forever.
        if (src_consumed_size == 0 && dst_produced_size == 0)
            return false;
    }
    return true;
}

bool LZ4Processor::EndDecodeStream()
{
    return m_decode_complete;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void Base64Processor::SetMemoryOutput(uint8_t* destination_buffer, mpsize_t destination_capacity)
{
    m_dest_buffer = destination_buffer;
    m_dest_capacity = destination_capacity;
}

inline const uint8_t* Base64Processor::GetBytePtr(const uint8_t* input_chunk, mpsize_t cursor)
{
    return (cursor >= m_leftover_chars.size())
        ? &input_chunk[cursor - m_leftover_chars.size()]
        : reinterpret_cast<uint8_t*>(&m_leftover_chars.data()[cursor]);
}

inline void Base64Processor::MoveToLeftoverBytes(mpsize_t needed_len, const uint8_t* &input_chunk, mpsize_t &input_chunk_size)
{
    if (m_leftover_chars.length() < needed_len)
    {
        // Append available input even if the group remains incomplete.
        mpsize_t bytesToMove = ntd::min(
            needed_len - to_mpsize(m_leftover_chars.length()), input_chunk_size);
        if (bytesToMove > 0)
        {
            m_leftover_chars.append(reinterpret_cast<const char*>(input_chunk), bytesToMove);
            input_chunk += bytesToMove;
            input_chunk_size -= bytesToMove;
        }
    }
}

bool Base64Processor::ProcessChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    if (!input_chunk && input_chunk_size > 0)
        return false;
    if (m_direction == Direction::Decode && m_decode_padding_seen && input_chunk_size > 0)
        return false;
    return (m_direction == Direction::Decode)
        ? ProcessDecodeChunk(input_chunk, input_chunk_size)
        : ProcessEncodeChunk(input_chunk, input_chunk_size);
}

bool Base64Processor::EndStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return true;

    bool res = (m_direction == Direction::Decode) ? EndDecodeStream() : EndEncodeStream();
    if (!res)
        return false;
    return (propagate_to_next_stage && m_next_stage) ? m_next_stage->EndStream(true) : true;
}

void Base64Processor::BeginStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return;

    IStreamProcessor::BeginStream(propagate_to_next_stage);
    if (m_next_stage)
    {
        m_output_buffer.clear();
        m_output_buffer.reserve(OUTPUT_BUFFER_SIZE);
    }
    m_leftover_chars.clear();
    m_decode_padding_seen = false;
}

inline bool Base64Processor::FlushOutputBuffer()
{
    if (!m_next_stage || m_output_buffer.empty())
        return true;
    bool success = m_next_stage->ProcessChunk(
        m_output_buffer.data(),
        to_mpsize(m_output_buffer.size()));
    m_output_buffer.clear();
    return success;
}

// --- Decoding logic ---

inline bool Base64Processor::ProcessDecodeChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    const mpsize_t leftover_size = to_mpsize(m_leftover_chars.length());
    if (input_chunk_size > max_mpsize_t - leftover_size)
        return false;
    mpsize_t remaining_len = leftover_size + input_chunk_size;
    MoveToLeftoverBytes(4, input_chunk, input_chunk_size);
    mpsize_t cursor_pos = 0;
    
    uint32_t b[4];
    while (remaining_len >= 4)
    {
        const char* current = reinterpret_cast<const char*>(GetBytePtr(input_chunk, cursor_pos));
        const bool pad2 = (current[2] == '=');
        const bool pad3 = (current[3] == '=');
        for (mpsize_t i = 0; i < 4; ++i)
        {
            b[i] = DECODE_TABLE[static_cast<unsigned char>(current[i])];
        }

        mpsize_t out_len = 3;
        if (pad2)
            out_len--;
        if (pad3)
            out_len--;

        // Reject invalid input rather than folding the 0xFF sentinel into a decoded byte:
        // the two leading characters must be alphabet, a non-'=' third/fourth character
        // must be alphabet, and '=' is legal only as trailing padding (no "XX=X").
        if (b[0] == 0xFF || b[1] == 0xFF ||
            (!pad2 && b[2] == 0xFF) || (!pad3 && b[3] == 0xFF) || (pad2 && !pad3))
            return false;
        if ((pad2 && (b[1] & 0x0F) != 0) ||
            (pad3 && !pad2 && (b[2] & 0x03) != 0) ||
            ((pad2 || pad3) && remaining_len != 4))
            return false;

        if (m_next_stage)
        {
            if (m_output_buffer.size() + out_len > OUTPUT_BUFFER_SIZE)
                if (!FlushOutputBuffer())
                    return false;
            m_output_buffer.push_back(DecodeByte1(b));
            if (out_len > 1)
                m_output_buffer.push_back(DecodeByte2(b));
            if (out_len > 2)
                m_output_buffer.push_back(DecodeByte3(b));
        }
        else
        {
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, out_len, write_offset))
                return false;
            m_dest_buffer[write_offset] = DecodeByte1(b);
            if (out_len > 1)
                m_dest_buffer[write_offset + 1] = DecodeByte2(b);
            if (out_len > 2)
                m_dest_buffer[write_offset + 2] = DecodeByte3(b);
            m_bytes_written += out_len;
        }

        cursor_pos += 4;
        remaining_len -= 4;
        if (pad2 || pad3)
            m_decode_padding_seen = true;
    }
    
    if (cursor_pos > 0)
    {
        if (remaining_len > 0)
        {
            const char* current = reinterpret_cast<const char*>(GetBytePtr(input_chunk, cursor_pos));
            m_leftover_chars.assign(current, remaining_len);
        }
        else
            m_leftover_chars.clear();
    }
    return true;
}

inline bool Base64Processor::EndDecodeStream()
{
    // A well-formed base64 stream ends on a 4-char boundary; any leftover here is a
    // partial group. Reject invalid trailing characters and impossible lengths rather
    // than silently skipping them, so truncated/corrupt input fails cleanly.
    uint32_t b[4];
    mpsize_t i = 0;
    for (char c : m_leftover_chars)
    {
        const uint8_t d = DECODE_TABLE[static_cast<unsigned char>(c)];
        if (d == 0xFF)
            return false;
        b[i++] = d;
    }
    if (i == 1)
        return false; // a single base64 character cannot form a byte
    if ((i == 2 && (b[1] & 0x0F) != 0) ||
        (i == 3 && (b[2] & 0x03) != 0))
        return false; // non-zero unused bits are a non-canonical encoding


    if (m_next_stage)
    {
        if (m_output_buffer.size() + 2 > OUTPUT_BUFFER_SIZE)
            if (!FlushOutputBuffer())
                return false;
        if (i >= 2)
            m_output_buffer.push_back(DecodeByte1(b));
        if (i >= 3)
            m_output_buffer.push_back(DecodeByte2(b));
        if (!FlushOutputBuffer())
            return false;
        return true;
    }
    else
    {
        const mpsize_t out_len = (i >= 2 ? 1u : 0u) + (i >= 3 ? 1u : 0u);
        mpsize_t write_offset = 0;
        if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, out_len, write_offset))
            return false;
        if (i >= 2)
            m_dest_buffer[write_offset] = DecodeByte1(b);
        if (i >= 3)
            m_dest_buffer[write_offset + 1] = DecodeByte2(b);
        m_bytes_written += out_len;
        return true;
    }
}

// --- Encoding logic ---

inline bool Base64Processor::ProcessEncodeChunk(const uint8_t* input_chunk, mpsize_t input_chunk_size)
{
    const mpsize_t leftover_size = to_mpsize(m_leftover_chars.length());
    if (input_chunk_size > max_mpsize_t - leftover_size)
        return false;
    mpsize_t remaining_size = leftover_size + input_chunk_size;
    MoveToLeftoverBytes(3, input_chunk, input_chunk_size);
    mpsize_t cursor_pos = 0;

    while (remaining_size >= 3)
    {
        const uint8_t* current_ptr = GetBytePtr(input_chunk, cursor_pos);
        if (m_next_stage)
        {
            if (m_output_buffer.size() + 4 > OUTPUT_BUFFER_SIZE)
                if (!FlushOutputBuffer())
                    return false;
            m_output_buffer.push_back(to_uint8(EncodeChar1(current_ptr)));
            m_output_buffer.push_back(to_uint8(EncodeChar2(current_ptr)));
            m_output_buffer.push_back(to_uint8(EncodeChar3(current_ptr)));
            m_output_buffer.push_back(to_uint8(EncodeChar4(current_ptr)));
        }
        else
        {
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, 4, write_offset))
                return false;
            m_dest_buffer[write_offset + 0] = to_uint8(EncodeChar1(current_ptr));
            m_dest_buffer[write_offset + 1] = to_uint8(EncodeChar2(current_ptr));
            m_dest_buffer[write_offset + 2] = to_uint8(EncodeChar3(current_ptr));
            m_dest_buffer[write_offset + 3] = to_uint8(EncodeChar4(current_ptr));
            m_bytes_written += 4;
        }
        cursor_pos += 3;
        remaining_size -= 3;
    }

    if (cursor_pos > 0)
    {
        if (remaining_size > 0)
        {
            const uint8_t* current_ptr = GetBytePtr(input_chunk, cursor_pos);
            m_leftover_chars.assign(reinterpret_cast<const char*>(current_ptr), remaining_size);
        }
        else
            m_leftover_chars.clear();
    }
    return true;
}

inline bool Base64Processor::EndEncodeStream()
{
    if (!m_leftover_chars.empty())
    {
        uint8_t in[3] = {0, 0, 0};
        for (mpsize_t i = 0; i < m_leftover_chars.length(); ++i)
        {
            in[i] = static_cast<uint8_t>(m_leftover_chars[i]);
        }

        char out[4];
        out[0] = EncodeChar1(in);
        out[1] = EncodeChar2(in);
        out[2] = (m_leftover_chars.length() > 1) ? EncodeChar3(in) : '=';
        out[3] = '=';

        if (m_next_stage)
        {
            if (m_output_buffer.size() + 4 > OUTPUT_BUFFER_SIZE)
                if (!FlushOutputBuffer())
                    return false;
            m_output_buffer.insert(m_output_buffer.end(), reinterpret_cast<uint8_t*>(out), reinterpret_cast<uint8_t*>(out) + 4);
        }
        else
        {
            mpsize_t write_offset = 0;
            if (!GetMemoryWriteOffset(m_bytes_written, m_dest_capacity, 4, write_offset))
                return false;
            ntd::memcpy(m_dest_buffer + write_offset, out, 4);
            m_bytes_written += 4;
        }
    }

    if (m_next_stage)
    {
        if (!FlushOutputBuffer())
            return false;
        return true;
    }
    return true;
}

bool Base64Processor::FlushStream(bool propagate_to_next_stage)
{
    if (propagate_to_next_stage && m_ignore_propagated_calls)
        return true;

    if (!FlushOutputBuffer())
        return false;
    if (propagate_to_next_stage && m_next_stage)
        return m_next_stage->FlushStream(true);
    return true;
}

// --- Static lookup tables ---
const char* Base64Processor::ENCODE_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const uint8_t Base64Processor::DECODE_TABLE[256] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  62,0xFF,0xFF,0xFF,  63,
      52,  53,  54,  55,  56,  57,  58,  59,  60,  61,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
      15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
      41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};
    
} // GPRX
