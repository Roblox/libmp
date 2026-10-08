// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "gprx_container/stream_processor.h"
#include "gprx_container/block.h"
#include "mp_api/c_api.h"
#include "mp_api/control_channel.h"
#include "roundtrip_capture_builder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::filesystem::path g_control_output;

void write_file(const std::filesystem::path& path, const uint8_t* data, size_t size)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream)
        throw std::runtime_error("failed to write " + path.string());
}

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    write_file(path, data.data(), data.size());
}

uint32_t capture_control(
    const uint8_t* command, mpsize_t command_size, uint8_t*, mpsize_t)
{
    // Prefix consumed by control_channel_fuzzer: separate buffers, aligned command and
    // response offsets, and a 64-byte response window.
    std::vector<uint8_t> seed(4 + command_size);
    seed[0] = 0;
    seed[1] = 0;
    seed[2] = 0;
    seed[3] = 7;
    if (command_size)
        std::memcpy(seed.data() + 4, command, command_size);
    write_file(g_control_output, seed);
    return 0;
}

void make_control_seeds(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    LibMP::ControlChannel client;
    LibMP::ExternalControlIO io;
    io.transfer = capture_control;
    client.AssignIO(io);

    const LibMP::TypesNtd::MsgBool* response = nullptr;
    g_control_output = directory / "enable-profiler.seed";
    client.SendEnableProfiler(LibMP::TypesNtd::MsgBool(true), response);
    g_control_output = directory / "enable-capture.seed";
    client.SendEnableCapture(LibMP::TypesNtd::MsgBool(false), response);
    g_control_output = directory / "show-ui.seed";
    client.SendShowUI(LibMP::TypesNtd::MsgBool(true), response);
    g_control_output = directory / "frame-limit.seed";
    client.SendSetFrameLimit(LibMP::TypesNtd::MsgI32(256), response);

    const uint8_t handshake[] = {0, 0, 0, 7};
    write_file(directory / "handshake.seed", handshake, sizeof(handshake));

    std::ifstream valid(directory / "enable-profiler.seed", std::ios::binary);
    std::vector<uint8_t> canonical(
        (std::istreambuf_iterator<char>(valid)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> truncated = canonical;
    if (!truncated.empty())
        truncated.pop_back();
    write_file(directory / "truncated.seed", truncated);

    if (canonical.size() >= 4 + sizeof(GPRX::BlockHeader))
    {
        GPRX::BlockHeader header{};
        std::memcpy(&header, canonical.data() + 4, sizeof(header));

        std::vector<uint8_t> non_raw = canonical;
        header.encoding_method = GPRX::BlockPayloadEncodingMethod::LZ4;
        std::memcpy(non_raw.data() + 4, &header, sizeof(header));
        write_file(directory / "non-raw-encoding-rejected.seed", non_raw);

        std::vector<uint8_t> hex_header(
            4 + GPRX::BlockHeaderHexSize + header.payload_size);
        std::memcpy(hex_header.data(), canonical.data(), 4);
        header.encoding_method = GPRX::BlockPayloadEncodingMethod::Raw;
        GPRX::HeaderToHex(header, hex_header.data() + 4);
        std::memcpy(
            hex_header.data() + 4 + GPRX::BlockHeaderHexSize,
            canonical.data() + 4 + sizeof(header), header.payload_size);
        write_file(directory / "hex-header-rejected.seed", hex_header);
    }

    LibMP::TypesNtd::CounterIteratorConfigT table;
    table.root_counter_ids.push_back(7);
    table.root_counter_ids.push_back(11);
    LibMP::FbBuilder builder;
    builder.Finish(LibMP::TypesNtd::CounterIteratorConfig::Pack(builder, &table));
    std::vector<uint8_t> table_seed(4 + builder.GetSize());
    table_seed[3] = 7;
    std::memcpy(table_seed.data() + 4, builder.GetBufferPointer(), builder.GetSize());
    write_file(directory / "table-verification.seed", table_seed);
}

void make_range_seed(
    const std::filesystem::path& path,
    double packed_offset,
    int64_t slot,
    int64_t size,
    int64_t destination_offset)
{
    uint8_t bytes[32];
    std::memcpy(bytes, &packed_offset, 8);
    std::memcpy(bytes + 8, &slot, 8);
    std::memcpy(bytes + 16, &size, 8);
    std::memcpy(bytes + 24, &destination_offset, 8);
    write_file(path, bytes, sizeof(bytes));
}

void make_range_seeds(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    make_range_seed(directory / "absolute.seed", 32.0, 0, 16, 0);
    const uint64_t packed = (uint64_t{1} << 52) | (uint64_t{5} << 32) | 7;
    make_range_seed(directory / "block-relative.seed", static_cast<double>(packed), 0, 32, 5);
    make_range_seed(directory / "fractional.seed", 1.5, 0, 1, 0);
    make_range_seed(directory / "negative-size.seed", 0.0, 0, -1, 0);
    make_range_seed(directory / "nan.seed", std::nan(""), 0, 1, 0);
}

void make_codec_seeds(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    const uint8_t base64[] = {0, 'T', 'W', 'F', 'u'};
    write_file(directory / "base64-valid.seed", base64, sizeof(base64));
    const uint8_t bad_base64[] = {0, '!', '=', '=', '='};
    write_file(directory / "base64-invalid.seed", bad_base64, sizeof(bad_base64));

    static const uint8_t text[] = "valid lz4 seed payload";
    GPRX::StreamProcessorChunkedMemory sink;
    GPRX::LZ4Processor encoder(GPRX::IStreamProcessor::Direction::Encode, &sink);
    encoder.BeginStream(true);
    if (!encoder.ProcessChunk(text, sizeof(text)) || !encoder.EndStream(true))
        throw std::runtime_error("failed to encode LZ4 seed");
    ntd::vector<uint8_t> encoded = sink.GetComposedChunks();
    std::vector<uint8_t> seed(1 + encoded.size());
    seed[0] = 1;
    std::memcpy(seed.data() + 1, encoded.data(), encoded.size());
    write_file(directory / "lz4-valid.seed", seed);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    MpInit();
    const std::filesystem::path root(argv[1]);

    make_control_seeds(root / "control_channel");
    make_range_seeds(root / "getdatainrange");
    make_codec_seeds(root / "codec");

    std::filesystem::create_directories(root / "gprx_reader");
    ntd::vector<uint8_t> capture = BuildCaptureBuffer(256, true);
    write_file(root / "gprx_reader" / "real-16-frame-capture.seed",
        capture.data(), capture.size());
    static const uint8_t truncated[] = {'A', '0', 'B', 'F'};
    write_file(root / "gprx_reader" / "truncated-magic.seed",
        truncated, sizeof(truncated));
    return 0;
}
