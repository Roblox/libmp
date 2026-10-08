// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// gprx_container_tests.cpp -- black-box tests for the GPRX container: build-from-scratch,
// save/reload round-trips across header and payload encodings, the global block index,
// and rejection of malformed input. Complements the gprx_reader libFuzzer target with
// deterministic, labelled cases.

#include "test_harness.h"
#include "diagnostic_assert_test.h"

#include "gprx_container/block.h"
#include "gprx_container/container_context.h"
#include "gprx_container/data_context.h"
#include "gprx_container/stream_processor.h"
#include "mp_api/c_api.h" // MpInit() for the LZ4 backend used by compressed payloads

#include <cstdio>
#include <cstring>

using GPRX::BlockPayloadEncodingMethod;
using GPRX::BlockType;
using GPRX::ContainerContext;

namespace
{
constexpr uint64_t kSig = 0x00c5a83f01030308ull; // 56-bit data-format signature
constexpr uint8_t kMajor = 1;
constexpr uint8_t kMinor = 0;

void fill(uint8_t* p, mpsize_t n, uint32_t seed)
{
    for (mpsize_t i = 0; i < n; ++i)
        p[i] = static_cast<uint8_t>((i * 37 + seed * 11 + (i >> 4)) & 0xFF);
}

// Builds a container: magic block (with the given container encoding) + one Data block
// carrying `payload` under `data_encoding`. Returns the serialized bytes.
ntd::vector<uint8_t> build_container(
    const uint8_t* payload, mpsize_t payload_size,
    BlockPayloadEncodingMethod container_encoding,
    BlockPayloadEncodingMethod data_encoding,
    bool raw_headers,
    bool with_global_index)
{
    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext c;

    c.AddBlock(1, true);
    c.InitMagicBlock(c.GetBlocks().back(), container_encoding);

    c.AddBlock(2, true);
    {
        GPRX::BlockContext& data = c.GetBlocks().back();
        data.headerPtr->type_id = BlockType::Data;
        data.headerPtr->encoding_method = data_encoding;
        c.AppendPayloadData(data, payload, payload_size);
    }

    if (with_global_index)
    {
        c.AddBlock(3, true);
        c.BuildGlobalIndex(c.GetBlocks().back());
    }

    return c.SaveToMemory(raw_headers);
}

// Reloads `buf` and checks block 2's payload equals [expected, expected+size).
void check_reload(const ntd::vector<uint8_t>& buf, const uint8_t* expected, mpsize_t size,
                  bool expect_global_index,
                  BlockPayloadEncodingMethod expected_index_encoding =
                      BlockPayloadEncodingMethod::Raw)
{
    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    GPRX::DataContextSingleBuffer ctx;
    ctx.AssignBuffer(buf.data(), buf.size());
    ContainerContext r(ctx, expect_global_index
        ? GPRX::InputTrust::Trusted
        : GPRX::InputTrust::Untrusted);

    CHECK(r.GetValidationStatus().is_valid);
    CHECK_EQ(r.HasGlobalIndex(), expect_global_index);
    if (expect_global_index)
    {
        GPRX::BlockContext* index = r.GetBlock(r.GetGlobalIndexBlockId());
        CHECK(index != nullptr);
        if (index)
            CHECK_EQ(index->headerPtr->encoding_method, expected_index_encoding);
    }

    GPRX::BlockContext* blk = r.GetBlock(2);
    CHECK(blk != nullptr);
    if (!blk)
        return;

    ntd::vector<uint8_t> out(size ? size : 1);
    bool ok = r.ReadPayloadDataToBuffer(*blk, 0, size, out.data(), GPRX::to_mpsize(out.size()));
    CHECK(ok);
    CHECK(std::memcmp(out.data(), expected, size) == 0);
}

void check_ascii_indexed_reload(
    const ntd::vector<uint8_t>& saved,
    const uint8_t* expected,
    mpsize_t expected_size,
    uint32_t expected_index_id,
    BlockPayloadEncodingMethod expected_encoding)
{
    CHECK(saved.size() > GPRX::MagicPartSize);
    if (saved.size() <= GPRX::MagicPartSize)
        return;

    bool is_printable_ascii = true;
    for (uint8_t byte : saved)
        is_printable_ascii = is_printable_ascii && byte >= 0x20 && byte <= 0x7e;
    CHECK(is_printable_ascii);
    CHECK(saved.size() >= GPRX::PAYLOAD_TRAILER_GUID_HEX.size());
    CHECK(std::memcmp(
        saved.data() + saved.size() - GPRX::PAYLOAD_TRAILER_GUID_HEX.size(),
        GPRX::PAYLOAD_TRAILER_GUID_HEX.data(),
        GPRX::PAYLOAD_TRAILER_GUID_HEX.size()) == 0);

    GPRX::DataContextSingleBuffer untrusted_context;
    untrusted_context.AssignBuffer(
        saved.data() + GPRX::MagicPartSize,
        saved.size() - GPRX::MagicPartSize);
    ContainerContext untrusted(untrusted_context);
    CHECK(untrusted.GetValidationStatus().is_valid);
    CHECK(!untrusted.HasGlobalIndex());

    GPRX::DataContextSingleBuffer trusted_context;
    trusted_context.AssignBuffer(
        saved.data() + GPRX::MagicPartSize,
        saved.size() - GPRX::MagicPartSize);
    ContainerContext loaded(trusted_context, GPRX::InputTrust::Trusted);

    CHECK(loaded.GetValidationStatus().is_valid);
    CHECK(loaded.GetMagic().isFirstHeaderTruncated);
    CHECK(loaded.HasGlobalIndex());
    CHECK_EQ(loaded.GetGlobalIndexBlockId(), expected_index_id);

    GPRX::BlockContext* index = loaded.GetBlock(expected_index_id);
    CHECK(index != nullptr);
    if (index)
        CHECK_EQ(index->headerPtr->encoding_method, expected_encoding);

    GPRX::BlockContext* data = loaded.GetBlock(2);
    CHECK(data != nullptr);
    if (!data)
        return;
    CHECK_EQ(data->headerPtr->encoding_method, expected_encoding);

    ntd::vector<uint8_t> actual(expected_size ? expected_size : 1);
    CHECK(loaded.ReadPayloadDataToBuffer(
        *data, 0, expected_size, actual.data(), GPRX::to_mpsize(actual.size())));
    CHECK(std::memcmp(actual.data(), expected, expected_size) == 0);
}
} // namespace

TEST_CASE(utils_scope_exit)
{
    int calls = 0;
    {
        GPRX::ScopeExit cleanup([&]() { ++calls; });
        CHECK_EQ(calls, 0);
    }
    CHECK_EQ(calls, 1);

    {
        GPRX::ScopeExit cleanup([&]() { ++calls; });
        auto moved_cleanup = ntd::move(cleanup);
    }
    CHECK_EQ(calls, 2);

    {
        GPRX::ScopeExit cleanup([&]() { ++calls; });
        cleanup.Release();
    }
    CHECK_EQ(calls, 2);
}

TEST_CASE(container_roundtrip_raw_headers)
{
    uint8_t payload[300];
    fill(payload, sizeof(payload), 1);
    ntd::vector<uint8_t> buf = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, /*raw_headers=*/true, /*global_index=*/false);
    CHECK(buf.size() > 0);
    check_reload(buf, payload, sizeof(payload), false);
}

TEST_CASE(container_roundtrip_hex_headers)
{
    uint8_t payload[192];
    fill(payload, sizeof(payload), 2);
    // ASCII/hex container: base64 payload encoding forces the hex header form.
    ntd::vector<uint8_t> buf = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Base64,
        BlockPayloadEncodingMethod::Base64, /*raw_headers=*/false, /*global_index=*/false);
    CHECK(buf.size() > 0);
    check_reload(buf, payload, sizeof(payload), false);
}

TEST_CASE(container_roundtrip_payload_encodings)
{
    MpInit();
    const BlockPayloadEncodingMethod encs[] = {
        BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Base64,
        BlockPayloadEncodingMethod::LZ4,
        BlockPayloadEncodingMethod::LZ4_Base64,
    };
    for (BlockPayloadEncodingMethod enc : encs)
    {
        uint8_t payload[1024];
        fill(payload, sizeof(payload), static_cast<uint32_t>(enc) + 5);
        const bool raw_headers = !GPRX::IsPayloadAsciiEncoded(enc);
        ntd::vector<uint8_t> buf = build_container(
            payload, sizeof(payload), enc, enc, raw_headers, false);
        CHECK(buf.size() > 0);
        check_reload(buf, payload, sizeof(payload), false);
    }
}

TEST_CASE(container_ascii_global_index_truncated_magic_roundtrip)
{
    MpInit();
    uint8_t payload[1024];
    fill(payload, sizeof(payload), 24);

    const BlockPayloadEncodingMethod encodings[] = {
        BlockPayloadEncodingMethod::Base64,
        BlockPayloadEncodingMethod::LZ4_Base64,
    };
    for (BlockPayloadEncodingMethod encoding : encodings)
    {
        ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
        ContainerContext container;
        container.AddBlock(1, true);
        container.InitMagicBlock(container.GetBlocks().back());

        container.AddBlock(2, true);
        GPRX::BlockContext& data = container.GetBlocks().back();
        data.headerPtr->type_id = BlockType::Data;
        data.headerPtr->encoding_method = BlockPayloadEncodingMethod::Raw;
        CHECK(container.AppendPayloadData(data, payload, sizeof(payload)));

        container.AddBlock(3, true);
        CHECK(container.BuildGlobalIndex(container.GetBlocks().back()));
        const mpsize_t live_index_size =
            container.GetBlocks().back().headerPtr->payload_uncompressed_size;
        container.RebuildDirectLookupTable();
        container.SetContainerEncodingMethod(encoding);

        GPRX::StreamProcessorChunkedMemory output(37);
        CHECK(container.SaveToStream(output, false, false, true));
        ntd::vector<uint8_t> saved = output.GetComposedChunks();
        CHECK(!saved.empty());
        GPRX::BlockHeader data_header{};
        CHECK(saved.size() >= GPRX::BlockHeaderHexSize * 2);
        if (saved.size() >= GPRX::BlockHeaderHexSize * 2)
        {
            CHECK(GPRX::HeaderFromHex(
                data_header, saved.data() + GPRX::BlockHeaderHexSize));
            CHECK(data_header.payload_size != GPRX::BlockPayloadSizeDeferredMarker);
        }
        CHECK_EQ(container.GetBlocks().back().headerPtr->encoding_method,
            BlockPayloadEncodingMethod::Raw);
        CHECK(!container.GetBlocks().back().bDontSave);
        CHECK_EQ(container.GetBlocks().back().headerPtr->payload_uncompressed_size,
            live_index_size);
        check_ascii_indexed_reload(saved, payload, sizeof(payload), 3, encoding);
    }
}

TEST_CASE(container_ascii_global_index_streams_data_with_trailer)
{
    MpInit();
    uint8_t payload[4097];
    fill(payload, sizeof(payload), 26);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext container;
    container.AddBlock(1, true);
    container.InitMagicBlock(container.GetBlocks().back());

    container.AddBlock(2, true);
    GPRX::BlockContext& data = container.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(container.AppendPayloadData(data, payload, sizeof(payload)));
    const mpsize_t original_payload_size = data.headerPtr->payload_size;

    container.SetContainerEncodingMethod(BlockPayloadEncodingMethod::LZ4_Base64);

    GPRX::StreamProcessorChunkedMemory output(37);
    CHECK(container.SaveToStream(output, true, false, true));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(!saved.empty());

    GPRX::BlockHeader data_header{};
    CHECK(saved.size() >= GPRX::BlockHeaderHexSize * 2);
    if (saved.size() >= GPRX::BlockHeaderHexSize * 2)
    {
        CHECK(GPRX::HeaderFromHex(
            data_header, saved.data() + GPRX::BlockHeaderHexSize));
        CHECK_EQ(data_header.payload_size, GPRX::BlockPayloadSizeDeferredMarker);
    }
    CHECK_EQ(data.headerPtr->payload_size, original_payload_size);

    check_ascii_indexed_reload(saved, payload, sizeof(payload), 3,
        BlockPayloadEncodingMethod::LZ4_Base64);
}

TEST_CASE(container_stream_appends_finalizer_after_global_index)
{
    MpInit();
    uint8_t payload[1024];
    fill(payload, sizeof(payload), 30);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext source;
    source.AddBlock(1, true);
    source.InitMagicBlock(source.GetBlocks().back());
    source.AddBlock(2, true);
    GPRX::BlockContext& data = source.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(source.AppendPayloadData(data, payload, sizeof(payload)));
    source.SetContainerEncodingMethod(BlockPayloadEncodingMethod::LZ4_Base64);

    static constexpr uint8_t finalizer[] =
        "<main>profile loaded</main></body></html>";
    GPRX::StreamFinalizerOptions finalizer_options;
    finalizer_options.finalizer_data = finalizer;
    finalizer_options.finalizer_size = GPRX::to_mpsize(sizeof(finalizer) - 1);

    GPRX::StreamProcessorChunkedMemory output(37);
    CHECK(source.SaveToStream(
        output, false, false, true, &finalizer_options));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(!saved.empty());

    size_t comment_close_offset = saved.size();
    for (size_t i = GPRX::MagicPartSize;
        i + GPRX::HtmlCommentClose.size() <= saved.size(); ++i)
    {
        if (std::memcmp(saved.data() + i,
                GPRX::HtmlCommentClose.data(),
                GPRX::HtmlCommentClose.size()) == 0)
        {
            comment_close_offset = i;
            break;
        }
    }
    CHECK(comment_close_offset < saved.size());
    if (comment_close_offset == saved.size())
        return;

    CHECK(comment_close_offset >= GPRX::PAYLOAD_TRAILER_GUID_HEX.size());
    if (comment_close_offset >= GPRX::PAYLOAD_TRAILER_GUID_HEX.size())
    {
        CHECK(std::memcmp(
            saved.data() + comment_close_offset - GPRX::PAYLOAD_TRAILER_GUID_HEX.size(),
            GPRX::PAYLOAD_TRAILER_GUID_HEX.data(),
            GPRX::PAYLOAD_TRAILER_GUID_HEX.size()) == 0);
    }

    CHECK_EQ(saved.size(),
        comment_close_offset + GPRX::HtmlCommentClose.size() +
            finalizer_options.finalizer_size);
    if (saved.size() !=
        comment_close_offset + GPRX::HtmlCommentClose.size() +
            finalizer_options.finalizer_size)
        return;
    CHECK(std::memcmp(saved.data() + comment_close_offset,
        GPRX::HtmlCommentClose.data(), GPRX::HtmlCommentClose.size()) == 0);
    CHECK(std::memcmp(
        saved.data() + comment_close_offset + GPRX::HtmlCommentClose.size(),
        finalizer, finalizer_options.finalizer_size) == 0);

    // An HTML parser exposes only the first comment's contents. That slice ends
    // exactly at the global-index trailer, so trusted loading retains its fast path.
    ntd::vector<uint8_t> data_comment(comment_close_offset);
    std::memcpy(data_comment.data(), saved.data(), data_comment.size());
    check_ascii_indexed_reload(data_comment, payload, sizeof(payload), 3,
        BlockPayloadEncodingMethod::LZ4_Base64);

    GPRX::DataContextSingleBuffer comment_context;
    comment_context.AssignBuffer(
        data_comment.data() + GPRX::MagicPartSize,
        data_comment.size() - GPRX::MagicPartSize);
    ContainerContext loaded_comment(comment_context, GPRX::InputTrust::Trusted);
    CHECK(loaded_comment.GetValidationStatus().is_valid);
    CHECK(loaded_comment.HasGlobalIndex());
    CHECK(loaded_comment.GetBlock(4) == nullptr);

    // Passing the whole generated document remains readable through sequential fallback.
    GPRX::DataContextSingleBuffer full_context;
    full_context.AssignBuffer(saved.data(), saved.size());
    ContainerContext loaded_full(full_context, GPRX::InputTrust::Trusted);
    CHECK(loaded_full.GetValidationStatus().is_valid);
    CHECK(loaded_full.GetBlock(2) != nullptr);

    // With raw headers, keep the finalizer inside a real Terminator block so a
    // sequential reader has an explicit boundary.
    GPRX::StreamProcessorChunkedMemory raw_output(37);
    CHECK(source.SaveToStreamWithEncoding(
        raw_output, BlockPayloadEncodingMethod::Raw,
        false, true, false, &finalizer_options));
    ntd::vector<uint8_t> raw_saved = raw_output.GetComposedChunks();
    CHECK(raw_saved.size() >= GPRX::BlockHeaderRawSize + finalizer_options.finalizer_size);
    if (raw_saved.size() >= GPRX::BlockHeaderRawSize + finalizer_options.finalizer_size)
    {
        const size_t terminator_header_offset =
            raw_saved.size() - finalizer_options.finalizer_size -
            GPRX::BlockHeaderRawSize;
        GPRX::BlockHeader terminator_header{};
        std::memcpy(&terminator_header,
            raw_saved.data() + terminator_header_offset, sizeof(terminator_header));
        CHECK_EQ(terminator_header.type_id, BlockType::Terminator);
        CHECK_EQ(terminator_header.block_id, 3u);
        CHECK_EQ(terminator_header.encoding_method, BlockPayloadEncodingMethod::Raw);
        CHECK_EQ(terminator_header.payload_size, finalizer_options.finalizer_size);
        CHECK(std::memcmp(
            raw_saved.data() + terminator_header_offset + GPRX::BlockHeaderRawSize,
            finalizer, finalizer_options.finalizer_size) == 0);
    }

    GPRX::DataContextSingleBuffer raw_context;
    raw_context.AssignBuffer(raw_saved.data(), raw_saved.size());
    ContainerContext loaded_raw(raw_context, GPRX::InputTrust::Trusted);
    CHECK(loaded_raw.GetValidationStatus().is_valid);
    GPRX::BlockContext* loaded_terminator = loaded_raw.GetBlock(3);
    CHECK(loaded_terminator != nullptr);
    if (loaded_terminator)
        CHECK_EQ(loaded_terminator->headerPtr->type_id, BlockType::Terminator);

    // An empty descriptor closes the one data comment without adding padding.
    GPRX::StreamFinalizerOptions close_only;
    GPRX::StreamProcessorChunkedMemory close_only_output(37);
    CHECK(source.SaveToStream(
        close_only_output, false, false, true, &close_only));
    ntd::vector<uint8_t> close_only_saved = close_only_output.GetComposedChunks();
    CHECK(close_only_saved.size() >= GPRX::HtmlCommentClose.size());
    if (close_only_saved.size() >= GPRX::HtmlCommentClose.size())
    {
        CHECK(std::memcmp(
            close_only_saved.data() + close_only_saved.size() -
                GPRX::HtmlCommentClose.size(),
            GPRX::HtmlCommentClose.data(), GPRX::HtmlCommentClose.size()) == 0);
    }
}

TEST_CASE(container_roundtrip_highly_compressible_payload)
{
    MpInit();
    static uint8_t payload[64 * 1024] = {};
    const BlockPayloadEncodingMethod encodings[] = {
        BlockPayloadEncodingMethod::LZ4,
        BlockPayloadEncodingMethod::LZ4_Base64,
    };
    for (BlockPayloadEncodingMethod encoding : encodings)
    {
        ntd::vector<uint8_t> saved = build_container(
            payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
            encoding, /*raw_headers=*/true, /*global_index=*/false);
        CHECK(!saved.empty());
        check_reload(saved, payload, sizeof(payload), false);
    }
}

TEST_CASE(container_rejects_mismatched_decompressed_size)
{
    MpInit();
    uint8_t payload[1024];
    fill(payload, sizeof(payload), 31);
    ntd::vector<uint8_t> saved = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::LZ4, /*raw_headers=*/true, /*global_index=*/false);
    CHECK(saved.size() > GPRX::BlockHeaderRawSize * 2);

    // The magic block has no payload, so the second raw header immediately follows it.
    // Keep a valid LZ4 frame but claim that it expands to more bytes than it actually
    // does. The reader must reject the mismatch rather than expose an unwritten tail.
    GPRX::BlockHeader* data_header = reinterpret_cast<GPRX::BlockHeader*>(
        saved.data() + GPRX::BlockHeaderRawSize);
    ++data_header->payload_uncompressed_size;

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(saved.data(), saved.size());
    ContainerContext reader(context);
    CHECK(reader.GetValidationStatus().is_valid);
    GPRX::BlockContext* block = reader.GetBlock(2);
    CHECK(block != nullptr);
    if (block)
    {
        ntd::vector<uint8_t> result(data_header->payload_uncompressed_size);
        libmp_test::ScopedExpectedDiagnostic expected(
            "ContainerContext::GetPayloadDataPtrImpl - decoded payload size does not match header");
        CHECK(!reader.ReadPayloadDataToBuffer(
            *block, 0, data_header->payload_uncompressed_size,
            result.data(), GPRX::to_mpsize(result.size())));
        CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(1));
    }
}

TEST_CASE(container_global_index_roundtrip)
{
    uint8_t payload[500];
    fill(payload, sizeof(payload), 9);

    struct EncodingCase
    {
        BlockPayloadEncodingMethod encoding;
        bool raw_headers;
    };
    const EncodingCase cases[] = {
        {BlockPayloadEncodingMethod::Raw, true},
        {BlockPayloadEncodingMethod::LZ4, true},
        {BlockPayloadEncodingMethod::Raw, false},
        {BlockPayloadEncodingMethod::Base64, false},
        {BlockPayloadEncodingMethod::LZ4_Base64, false},
    };

    for (const EncodingCase& test : cases)
    {
        ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
        ContainerContext container;
        container.AddBlock(1, true);
        GPRX::BlockContext& magic = container.GetBlocks().back();
        container.InitMagicBlock(magic);

        container.AddBlock(2, true);
        GPRX::BlockContext& data = container.GetBlocks().back();
        data.headerPtr->type_id = BlockType::Data;
        CHECK(container.AppendPayloadData(data, payload, sizeof(payload)));

        container.AddBlock(3, true);
        CHECK(container.BuildGlobalIndex(container.GetBlocks().back()));
        if (test.encoding != BlockPayloadEncodingMethod::Raw)
            container.SetContainerEncodingMethod(test.encoding);
        else if (!test.raw_headers)
            container.InitMagicBlock(magic, BlockPayloadEncodingMethod::Base64);

        ntd::vector<uint8_t> saved;
        if (test.encoding == BlockPayloadEncodingMethod::Raw && test.raw_headers)
        {
            GPRX::StreamProcessorChunkedMemory without_index;
            CHECK(container.SaveToStream(without_index, false, true, false));
            check_reload(without_index.GetComposedChunks(),
                payload, sizeof(payload), false);

            saved = container.SaveToMemory(true);
        }
        else
        {
            CHECK(container.SaveToMemory(test.raw_headers).empty());
            GPRX::StreamProcessorChunkedMemory output;
            CHECK(container.SaveToStream(output, false, test.raw_headers, true));
            saved = output.GetComposedChunks();
        }
        CHECK(!saved.empty());
        check_reload(saved, payload, sizeof(payload), true, test.encoding);
    }
}

TEST_CASE(container_global_index_rebuild_switches_active_index)
{
    uint8_t payload[96];
    fill(payload, sizeof(payload), 29);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext container;
    container.AddBlock(1, true);
    container.InitMagicBlock(container.GetBlocks().back());
    container.AddBlock(2, true);
    GPRX::BlockContext& data = container.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(container.AppendPayloadData(data, payload, sizeof(payload)));

    container.AddBlock(3, true);
    GPRX::BlockContext& first_index = container.GetBlocks().back();
    CHECK(container.BuildGlobalIndex(first_index));
    CHECK(!first_index.bDontSave);

    container.AddBlock(4, true);
    GPRX::BlockContext& second_index = container.GetBlocks().back();
    CHECK(container.BuildGlobalIndex(second_index));
    CHECK(first_index.bDontSave);
    CHECK(!second_index.bDontSave);
    CHECK_EQ(container.GetGlobalIndexBlockId(), 4u);

    // Rebuilding a superseded index makes it active again.
    CHECK(container.BuildGlobalIndex(first_index));
    CHECK(!first_index.bDontSave);
    CHECK(second_index.bDontSave);
    CHECK_EQ(container.GetGlobalIndexBlockId(), 3u);

    ntd::vector<uint8_t> saved = container.SaveToMemory(true);
    CHECK(!saved.empty());
    check_reload(saved, payload, sizeof(payload), true);
}

TEST_CASE(container_external_payload_chunk)
{
    // A block that references caller-owned memory without copying.
    static uint8_t external[256];
    fill(external, sizeof(external), 3);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext c;
    c.AddBlock(1, true);
    c.InitMagicBlock(c.GetBlocks().back());
    c.AddBlock(2, true);
    {
        GPRX::BlockContext& data = c.GetBlocks().back();
        data.headerPtr->type_id = BlockType::Data;
        CHECK(data.AddExternalPayloadChunk(external, sizeof(external)));
    }
    c.RebuildDirectLookupTable();
    ntd::vector<uint8_t> buf = c.SaveToMemory(true);
    CHECK(buf.size() > 0);
    check_reload(buf, external, sizeof(external), false);
}

TEST_CASE(container_external_payload_chunks_reject_owned_append)
{
    uint8_t external[] = {1, 2, 3};
    uint8_t suffix[] = {4, 5, 6, 7};
    const uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7};

    ContainerContext c;
    c.AddBlock(1, true);
    c.InitMagicBlock(c.GetBlocks().back());
    c.AddBlock(2, true);
    GPRX::BlockContext& data = c.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(data.AddExternalPayloadChunk(external, sizeof(external)));
    CHECK(data.AddExternalPayloadChunk(suffix, sizeof(suffix)));

    uint8_t direct[sizeof(expected)] = {};
    CHECK(c.ReadPayloadDataToBuffer(data, 0, sizeof(direct), direct, sizeof(direct)));
    CHECK(std::memcmp(direct, expected, sizeof(expected)) == 0);
    uint8_t crossing[4] = {};
    CHECK(c.ReadPayloadDataToBuffer(data, 1, sizeof(crossing), crossing, sizeof(crossing)));
    CHECK(std::memcmp(crossing, expected + 1, sizeof(crossing)) == 0);
    CHECK(c.ReadPayloadDataToBuffer(data, sizeof(expected), 0, nullptr, 0));

    const mpsize_t payload_size = data.headerPtr->payload_uncompressed_size;
    const mpsize_t chunk_count = GPRX::to_mpsize(data.payloadChunks.size());
    uint8_t appended = 8;
    CHECK(!c.AppendPayloadData(data, &appended, sizeof(appended)));
    CHECK_EQ(data.headerPtr->payload_uncompressed_size, payload_size);
    CHECK_EQ(data.payloadChunks.size(), chunk_count);

    c.RebuildDirectLookupTable();
    ntd::vector<uint8_t> saved = c.SaveToMemory(true);
    check_reload(saved, expected, sizeof(expected), false);
}

TEST_CASE(container_owned_payload_rejects_external_chunk)
{
    uint8_t owned[] = {1, 2, 3};
    uint8_t external[] = {4, 5};

    ContainerContext c;
    c.AddBlock(1, true);
    GPRX::BlockContext& block = c.GetBlocks().back();
    CHECK(c.AppendPayloadData(block, owned, sizeof(owned)));

    CHECK(!block.AddExternalPayloadChunk(external, sizeof(external)));
    CHECK_EQ(block.headerPtr->payload_uncompressed_size, sizeof(owned));
    CHECK_EQ(block.payloadChunks.size(), 1u);
}

TEST_CASE(container_append_rejects_size_overflow_without_mutation)
{
    ContainerContext c;
    GPRX::BlockContext block;
    block.AllocateHeader();
    block.bNewWritableBlock = true;
    block.headerPtr->payload_size = GPRX::max_mpsize_t;
    block.headerPtr->payload_uncompressed_size = GPRX::max_mpsize_t;
    const uint8_t byte = 1;
    CHECK(!c.AppendPayloadData(block, &byte, 1));
    CHECK_EQ(block.headerPtr->payload_uncompressed_size, GPRX::max_mpsize_t);
    CHECK(block.payloadChunks.empty());
}

TEST_CASE(container_save_rejects_incomplete_payload_chunks)
{
    ContainerContext c;
    c.AddBlock(1, true);
    c.InitMagicBlock(c.GetBlocks().back());
    c.AddBlock(2, true);
    GPRX::BlockContext& data = c.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    const uint8_t payload[] = {1, 2, 3, 4};
    CHECK(c.AppendPayloadData(data, payload, sizeof(payload)));

    // Simulate inconsistent producer metadata. Serialization must fail instead of
    // returning a buffer whose later headers are shifted into the missing payload.
    ++data.headerPtr->payload_size;
    ++data.headerPtr->payload_uncompressed_size;
    CHECK(c.SaveToMemory(true).empty());
}

// ---- Malformed / corner cases: must report invalid, never crash ---------------------
TEST_CASE(container_rejects_malformed)
{
    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);

    auto is_valid = [](const uint8_t* data, mpsize_t size) -> bool {
        GPRX::DataContextSingleBuffer ctx;
        ctx.AssignBuffer(data, size);
        ContainerContext r(ctx);
        return r.GetValidationStatus().is_valid;
    };

    CHECK(!is_valid(nullptr, 0));
    static const uint8_t empty[1] = { 0 };
    CHECK(!is_valid(empty, 0));

    static uint8_t garbage[128];
    fill(garbage, sizeof(garbage), 123);
    CHECK(!is_valid(garbage, 4));               // shorter than any header
    CHECK(!is_valid(garbage, sizeof(garbage))); // no valid magic
}

TEST_CASE(container_rejects_wrong_dataformat)
{
    uint8_t payload[64];
    fill(payload, sizeof(payload), 4);
    ntd::vector<uint8_t> buf = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, false);

    // Reload with a different data-format signature -> not matched.
    ContainerContext::SetDataFormatMeta(kSig ^ 0x1, kMajor, kMinor);
    GPRX::DataContextSingleBuffer ctx;
    ctx.AssignBuffer(buf.data(), buf.size());
    ContainerContext r(ctx);
    CHECK(!r.GetValidationStatus().is_valid);
    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor); // restore
}

TEST_CASE(container_read_out_of_range)
{
    uint8_t payload[128];
    fill(payload, sizeof(payload), 7);
    ntd::vector<uint8_t> buf = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, false);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    GPRX::DataContextSingleBuffer ctx;
    ctx.AssignBuffer(buf.data(), buf.size());
    ContainerContext r(ctx);
    CHECK(r.GetValidationStatus().is_valid);

    GPRX::BlockContext* blk = r.GetBlock(2);
    CHECK(blk != nullptr);
    if (!blk)
        return;

    uint8_t out[256];
    // Read past the end of the payload -> rejected.
    CHECK(!r.ReadPayloadDataToBuffer(*blk, 0, sizeof(payload) + 1, out, sizeof(out)));
    CHECK(!r.ReadPayloadDataToBuffer(*blk, sizeof(payload), 1, out, sizeof(out)));
    CHECK(r.GetPayloadDataPtr(*blk, sizeof(payload) - 4, 8, nullptr) == nullptr);
    // A valid in-range read still works.
    CHECK(r.ReadPayloadDataToBuffer(*blk, 10, 20, out, sizeof(out)));
    CHECK(std::memcmp(out, payload + 10, 20) == 0);
}

TEST_CASE(container_file_roundtrip)
{
    uint8_t payload[257];
    fill(payload, sizeof(payload), 15);
    ntd::vector<uint8_t> expected = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::LZ4, true, false);
    CHECK(!expected.empty());

    const char* path = "libmp_gprx_container_test.gprx";
    {
        GPRX::StreamProcessorFile output;
        CHECK(output.Open(path));
        CHECK(output.ProcessChunk(expected.data(), GPRX::to_mpsize(expected.size())));
        CHECK(output.EndStream());
    }

    {
        GPRX::DataContextFile file;
        CHECK(file.Open(path));
        ContainerContext loaded(file);
        CHECK(loaded.GetValidationStatus().is_valid);
        GPRX::BlockContext* block = loaded.GetBlock(2);
        CHECK(block != nullptr);
        uint8_t actual[sizeof(payload)] = {};
        if (block)
        {
            CHECK(loaded.ReadPayloadDataToBuffer(
                *block, 0, sizeof(actual), actual, sizeof(actual)));
            CHECK(std::memcmp(actual, payload, sizeof(payload)) == 0);
        }
    }
    // Windows cannot remove a file while it is still open.
    CHECK_EQ(std::remove(path), 0);
}

TEST_CASE(file_stream_transfers_open_handle_to_data_context)
{
    const char* path = "libmp_gprx_file_transfer_test.gprx";
    const uint8_t expected[] = {1, 3, 5, 7, 9, 11};
    {
        GPRX::StreamProcessorFile output;
        CHECK(output.Open(path));
        output.SetRetainFileOnEnd(true);
        CHECK(output.ProcessChunk(expected, sizeof(expected)));
        CHECK(output.EndStream());
        CHECK(output.IsOpen());

        GPRX::DataContextFile input;
        CHECK(output.TransferFileTo(input));
        CHECK(!output.IsOpen());
        CHECK_EQ(input.GetTotalDataSize(), sizeof(expected));

        uint8_t actual[sizeof(expected)] = {};
        CHECK_EQ(input.ReadAbsDataToBuffer(0, sizeof(actual), actual, sizeof(actual)), sizeof(actual));
        CHECK(std::memcmp(actual, expected, sizeof(expected)) == 0);
        CHECK(!output.TransferFileTo(input));
    }
    CHECK_EQ(std::remove(path), 0);
}

TEST_CASE(container_external_memchunks_view)
{
    uint8_t payload[333];
    fill(payload, sizeof(payload), 16);

    ContainerContext source;
    source.AddBlock(1, true);
    source.InitMagicBlock(source.GetBlocks().back());
    source.AddBlock(2, true);
    GPRX::BlockContext& data = source.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(source.AppendPayloadData(data, payload, sizeof(payload)));
    source.RebuildDirectLookupTable();

    uint8_t** chunk_ptrs = nullptr;
    mpsize_t* chunk_sizes = nullptr;
    mpsize_t chunk_count = 0;
    source.GetAllDataChunks(chunk_ptrs, chunk_sizes, chunk_count);
    CHECK(chunk_ptrs != nullptr);
    CHECK(chunk_sizes != nullptr);
    CHECK(chunk_count > 0);

    GPRX::DataContextMemChunks view;
    source.InitExternalDataContextWithDataChunks(view);
    ContainerContext loaded(view);
    CHECK(loaded.GetValidationStatus().is_valid);
    GPRX::BlockContext* block = loaded.GetBlock(2);
    CHECK(block != nullptr);
    uint8_t actual[sizeof(payload)] = {};
    if (block)
    {
        CHECK(loaded.ReadPayloadDataToBuffer(
            *block, 0, sizeof(actual), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, payload, sizeof(payload)) == 0);
    }
}

TEST_CASE(container_loaded_then_appended_roundtrip)
{
    uint8_t first_payload[97];
    uint8_t second_payload[131];
    fill(first_payload, sizeof(first_payload), 17);
    fill(second_payload, sizeof(second_payload), 18);
    ntd::vector<uint8_t> initial = build_container(
        first_payload, sizeof(first_payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, false);

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(initial.data(), initial.size());
    ContainerContext mixed(context);
    CHECK(mixed.GetValidationStatus().is_valid);
    mixed.AddBlock(3, true);
    GPRX::BlockContext& appended = mixed.GetBlocks().back();
    appended.headerPtr->type_id = BlockType::Data;
    appended.headerPtr->encoding_method = BlockPayloadEncodingMethod::Base64;
    CHECK(mixed.AppendPayloadData(appended, second_payload, sizeof(second_payload)));
    mixed.RebuildDirectLookupTable();

    ntd::vector<uint8_t> saved = mixed.SaveToMemory(false);
    CHECK(!saved.empty());
    GPRX::DataContextSingleBuffer saved_context;
    saved_context.AssignBuffer(saved.data(), saved.size());
    ContainerContext reloaded(saved_context);
    CHECK(reloaded.GetValidationStatus().is_valid);

    uint8_t actual[sizeof(second_payload)] = {};
    GPRX::BlockContext* first = reloaded.GetBlock(2);
    GPRX::BlockContext* second = reloaded.GetBlock(3);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    if (first)
    {
        CHECK(reloaded.ReadPayloadDataToBuffer(
            *first, 0, sizeof(first_payload), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, first_payload, sizeof(first_payload)) == 0);
    }
    if (second)
    {
        CHECK(reloaded.ReadPayloadDataToBuffer(
            *second, 0, sizeof(second_payload), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, second_payload, sizeof(second_payload)) == 0);
    }
}

TEST_CASE(container_converts_loaded_raw_to_ascii_indexed)
{
    MpInit();
    uint8_t payload[4097];
    fill(payload, sizeof(payload), 25);
    ntd::vector<uint8_t> raw = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, true);
    CHECK(!raw.empty());

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(raw.data(), raw.size());
    ContainerContext converted(context);
    CHECK(converted.GetValidationStatus().is_valid);

    // Materialize the raw source before changing the desired output encoding.
    converted.CacheReadOnlyBlockPayloads();
    converted.SetContainerEncodingMethod(BlockPayloadEncodingMethod::LZ4_Base64);

    GPRX::StreamProcessorChunkedMemory output;
    CHECK(converted.SaveToStream(output, false, false, true));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(!saved.empty());
    check_ascii_indexed_reload(saved, payload, sizeof(payload), 3,
        BlockPayloadEncodingMethod::LZ4_Base64);
}

TEST_CASE(container_streaming_trailer_roundtrip)
{
    MpInit();
    uint8_t payload[4097];
    fill(payload, sizeof(payload), 19);

    ContainerContext source;
    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    source.AddBlock(1, true);
    source.InitMagicBlock(source.GetBlocks().back(), BlockPayloadEncodingMethod::LZ4);
    source.AddBlock(2, true);
    GPRX::BlockContext& data = source.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    data.headerPtr->encoding_method = BlockPayloadEncodingMethod::LZ4;
    CHECK(source.AppendPayloadData(data, payload, sizeof(payload)));
    source.RebuildDirectLookupTable();

    GPRX::StreamProcessorChunkedMemory output(37); // deliberately awkward chunks
    CHECK(source.SaveToStream(output, true, true, false));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(!saved.empty());

    GPRX::MagicParsed magic = GPRX::MagicParseData(saved.data());
    CHECK(magic.isOk);
    CHECK_EQ(magic.headerSerializationMethod, GPRX::HeaderSerializationMethod::Raw);
    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(saved.data(), saved.size());
    ContainerContext reloaded(context);
    const ContainerContext::ValidationStatus& status = reloaded.GetValidationStatus();
    CHECK(status.container_magic_match);
    CHECK(status.container_version_match);
    CHECK(status.dataformat_major_version_match);
    CHECK(status.dataformat_signature_match);
    CHECK(status.is_valid);

    GPRX::BlockContext* block = reloaded.GetBlock(2);
    CHECK(block != nullptr);
    uint8_t actual[sizeof(payload)] = {};
    if (block)
    {
        CHECK(reloaded.ReadPayloadDataToBuffer(
            *block, 0, sizeof(actual), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, payload, sizeof(payload)) == 0);
    }
}

TEST_CASE(container_rejects_mismatched_raw_deferred_size)
{
    uint8_t payload[257];
    fill(payload, sizeof(payload), 27);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext source;
    source.AddBlock(1, true);
    source.InitMagicBlock(source.GetBlocks().back());
    source.AddBlock(2, true);
    GPRX::BlockContext& data = source.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(source.AppendPayloadData(data, payload, sizeof(payload)));

    // A serialization-only raw index uses a deferred size. Its uncompressed size
    // must equal the trailer's actual payload size (which excludes trailer padding).
    GPRX::StreamProcessorChunkedMemory output;
    CHECK(source.SaveToStream(output, false, true, true));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(saved.size() >= GPRX::BlockTrailerFullSize);
    if (saved.size() < GPRX::BlockTrailerFullSize)
        return;

    GPRX::PayloadTrailer trailer{};
    std::memcpy(&trailer,
        saved.data() + saved.size() - GPRX::BlockTrailerFullSize,
        sizeof(trailer));
    CHECK(trailer.distance_between_blocks >= GPRX::BlockTrailerFullSize);
    CHECK(saved.size() >= trailer.distance_between_blocks + GPRX::BlockHeaderRawSize);
    if (saved.size() < trailer.distance_between_blocks + GPRX::BlockHeaderRawSize)
        return;

    const size_t index_header_offset =
        saved.size() - trailer.distance_between_blocks - GPRX::BlockHeaderRawSize;
    GPRX::BlockHeader index_header{};
    std::memcpy(&index_header, saved.data() + index_header_offset, sizeof(index_header));
    CHECK_EQ(index_header.type_id, BlockType::GlobalIndex);
    CHECK_EQ(index_header.encoding_method, BlockPayloadEncodingMethod::Raw);
    CHECK_EQ(index_header.payload_size, GPRX::BlockPayloadSizeDeferredMarker);
    CHECK_EQ(index_header.payload_uncompressed_size, trailer.payload_actual_size);
    ++index_header.payload_uncompressed_size;
    std::memcpy(saved.data() + index_header_offset, &index_header, sizeof(index_header));

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(saved.data(), saved.size());
    ContainerContext loaded(context);
    CHECK(loaded.GetValidationStatus().is_valid);
    CHECK(!loaded.HasGlobalIndex());
    CHECK(loaded.GetBlock(2) != nullptr);
}

TEST_CASE(container_copies_unaligned_raw_global_index)
{
    uint8_t payload[257];
    fill(payload, sizeof(payload), 28);

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ContainerContext source;
    source.AddBlock(1, true);
    source.InitMagicBlock(
        source.GetBlocks().back(), BlockPayloadEncodingMethod::Base64);
    source.AddBlock(2, true);
    GPRX::BlockContext& data = source.GetBlocks().back();
    data.headerPtr->type_id = BlockType::Data;
    CHECK(source.AppendPayloadData(data, payload, sizeof(payload)));

    // Hex headers can still accompany raw payloads. Removing the four-byte magic
    // prefix then shifts a raw index away from its required eight-byte alignment.
    GPRX::StreamProcessorChunkedMemory output;
    CHECK(source.SaveToStream(output, false, false, true));
    ntd::vector<uint8_t> saved = output.GetComposedChunks();
    CHECK(saved.size() > GPRX::MagicPartSize + GPRX::BlockTrailerFullHexSize);
    if (saved.size() <= GPRX::MagicPartSize + GPRX::BlockTrailerFullHexSize)
        return;

    GPRX::PayloadTrailer trailer{};
    CHECK(GPRX::DeserializeFromHex(
        saved.data() + saved.size() - GPRX::BlockTrailerFullHexSize,
        sizeof(GPRX::PayloadTrailer) * 2, &trailer));

    ntd::vector<uint8_t> truncated(saved.size() - GPRX::MagicPartSize);
    std::memcpy(truncated.data(), saved.data() + GPRX::MagicPartSize,
        truncated.size());
    CHECK(truncated.size() >= trailer.distance_between_blocks);
    if (truncated.size() < trailer.distance_between_blocks)
        return;
    uint8_t* index_payload =
        truncated.data() + truncated.size() - trailer.distance_between_blocks;
    CHECK(!GPRX::is_aligned<GPRX::kDataAlignment>(index_payload));

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(truncated.data(), truncated.size());
    ContainerContext loaded(context, GPRX::InputTrust::Trusted);
    CHECK(loaded.GetValidationStatus().is_valid);
    CHECK(loaded.HasGlobalIndex());
    GPRX::BlockContext* loaded_data = loaded.GetBlock(2);
    CHECK(loaded_data != nullptr);
    if (loaded_data)
    {
        uint8_t actual[sizeof(payload)] = {};
        CHECK(loaded.ReadPayloadDataToBuffer(
            *loaded_data, 0, sizeof(actual), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, payload, sizeof(payload)) == 0);
    }
}

TEST_CASE(container_reload_after_early_scan_failure_resets_fingerprints)
{
    auto build = [](mpsize_t payload_size)
    {
        ntd::vector<uint8_t> payload(payload_size);
        fill(payload.data(), payload_size, payload_size);

        ContainerContext source;
        source.AddBlock(0, true);
        source.InitMagicBlock(source.GetBlocks().back());
        source.AddBlock(2, true);
        GPRX::BlockContext& data = source.GetBlocks().back();
        data.headerPtr->type_id = BlockType::Data;
        CHECK(source.AppendPayloadData(data, payload.data(), payload_size));
        return source.SaveToMemory(true);
    };

    ContainerContext::SetDataFormatMeta(kSig, kMajor, kMinor);
    ntd::vector<uint8_t> initial = build(8);
    ntd::vector<uint8_t> updated = build(16);
    CHECK(!initial.empty());
    CHECK(!updated.empty());

    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(initial.data(), initial.size());
    ContainerContext loaded(context);
    CHECK(loaded.GetValidationStatus().is_valid);

    uint32_t* removed_ids = nullptr;
    mpsize_t removed_count = 0;
    bool force_clear_caches = false;
    context.AssignBuffer(nullptr, 0);
    CHECK(loaded.SyncWithDataSource(
        &removed_ids, &removed_count, &force_clear_caches));
    CHECK(!loaded.GetValidationStatus().is_valid);

    context.AssignBuffer(updated.data(), updated.size());
    CHECK(loaded.SyncWithDataSource(
        &removed_ids, &removed_count, &force_clear_caches));
    CHECK(loaded.GetValidationStatus().is_valid);
    CHECK(!force_clear_caches);
    CHECK_EQ(removed_count, 1u);
    if (removed_count == 1)
        CHECK_EQ(removed_ids[0], 2u);
}

TEST_CASE(container_rejects_corrupt_hex_header)
{
    uint8_t payload[64];
    fill(payload, sizeof(payload), 20);
    ntd::vector<uint8_t> saved = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Base64,
        BlockPayloadEncodingMethod::Base64, false, false);
    CHECK(saved.size() > GPRX::BlockHeaderHexSize + 8);
    if (saved.size() <= GPRX::BlockHeaderHexSize + 8)
        return;

    saved[GPRX::BlockHeaderHexSize + 1] = 'Z';
    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(saved.data(), saved.size());
    ContainerContext corrupt(context);
    CHECK(corrupt.GetBlock(2) == nullptr);
}

TEST_CASE(container_does_not_trust_corrupt_global_index_entries)
{
    uint8_t payload[96];
    fill(payload, sizeof(payload), 21);
    ntd::vector<uint8_t> saved = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, true);
    CHECK(saved.size() >= GPRX::BlockTrailerFullSize);
    if (saved.size() < GPRX::BlockTrailerFullSize)
        return;

    GPRX::PayloadTrailer trailer{};
    std::memcpy(&trailer,
        saved.data() + saved.size() - GPRX::BlockTrailerFullSize,
        sizeof(trailer));
    const size_t index_payload = saved.size() - trailer.distance_between_blocks;
    // Entry layout is BlockHeader + uint64 payload offset, aligned to eight bytes.
    // Corrupt block 2's indexed offset while leaving its real on-disk header untouched.
    const size_t second_entry_offset = index_payload + sizeof(GPRX::BlockHeader) + sizeof(mpdatasz_t);
    const size_t second_payload_offset = second_entry_offset + sizeof(GPRX::BlockHeader);
    CHECK(second_payload_offset + sizeof(mpdatasz_t) <= saved.size());
    if (second_payload_offset + sizeof(mpdatasz_t) > saved.size())
        return;
    const mpdatasz_t forged_offset = 0;
    std::memcpy(saved.data() + second_payload_offset, &forged_offset, sizeof(forged_offset));

    // The default untrusted loader ignores the forged index and uses the
    // authoritative sequential headers, preserving the real block contents.
    check_reload(saved, payload, sizeof(payload), false);
}

TEST_CASE(container_trusted_index_fallback_resets_provider_locations)
{
    uint8_t payload[96];
    fill(payload, sizeof(payload), 41);
    ntd::vector<uint8_t> saved = build_container(
        payload, sizeof(payload), BlockPayloadEncodingMethod::Raw,
        BlockPayloadEncodingMethod::Raw, true, true);
    CHECK(saved.size() >= GPRX::BlockTrailerFullSize);
    if (saved.size() < GPRX::BlockTrailerFullSize)
        return;

    GPRX::PayloadTrailer trailer{};
    std::memcpy(&trailer,
        saved.data() + saved.size() - GPRX::BlockTrailerFullSize,
        sizeof(trailer));
    const size_t index_payload = saved.size() - trailer.distance_between_blocks;
    const size_t index_entry_size = sizeof(GPRX::BlockHeader) + sizeof(mpdatasz_t);
    CHECK(index_payload + index_entry_size * 2 <= saved.size());
    if (index_payload + index_entry_size * 2 > saved.size())
        return;

    // Let the first indexed entry register a conflicting block location, then make
    // the second entry fail. Sequential fallback must discard that partial state.
    GPRX::BlockHeader first_entry{};
    std::memcpy(&first_entry, saved.data() + index_payload, sizeof(first_entry));
    first_entry.block_id = 2;
    std::memcpy(saved.data() + index_payload, &first_entry, sizeof(first_entry));
    const mpdatasz_t invalid_offset = 0;
    std::memcpy(saved.data() + index_payload + index_entry_size + sizeof(GPRX::BlockHeader),
        &invalid_offset, sizeof(invalid_offset));

    const uint8_t* chunks[] = {saved.data()};
    const mpsize_t chunk_sizes[] = {GPRX::to_mpsize(saved.size())};
    GPRX::DataContextMemChunks context;
    CHECK(context.AssignMemChunks(chunks, chunk_sizes, 1));
    ContainerContext loaded(context, GPRX::InputTrust::Trusted);
    CHECK(loaded.GetValidationStatus().is_valid);
    CHECK(!loaded.HasGlobalIndex());

    GPRX::BlockContext* data = loaded.GetBlock(2);
    CHECK(data != nullptr);
    if (data)
    {
        uint8_t actual[sizeof(payload)] = {};
        CHECK(loaded.ReadPayloadDataToBuffer(
            *data, 0, sizeof(actual), actual, sizeof(actual)));
        CHECK(std::memcmp(actual, payload, sizeof(payload)) == 0);
    }
}

LIBMP_TEST_MAIN("gprx_container")
