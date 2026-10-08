// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "tools.h"

#include "c_api.h"
#include "c_api_private.h"
#include "control_channel.h"
#include "gprx_container/container_context.h"
#include "gprx_container/stream_processor.h"
#include "gprx_container/utils_ex.h"
#include "ntd_stdlib/string.h"
#include <cstdio>

#ifndef LIBMP_HTML_ASSETS_BASE_URL
    // Override at compile time; runtime callers can replace it through the Tools API.
    #define LIBMP_HTML_ASSETS_BASE_URL "."
#endif

namespace LibMP
{
namespace
{

static constexpr char EmbeddedHtmlAssetsBaseUrl[] = LIBMP_HTML_ASSETS_BASE_URL;
static constexpr const char* HtmlAssetNames[] = {
    "libmp.js", "tools.js", "viewer.js",
};
static ntd::string HtmlAssetsBaseUrlOverride;

ntd::string BuildHtmlFinalizer(uint8_t data_format_major, uint8_t data_format_minor)
{
    ntd::string asset_prefix(HtmlAssetsBaseUrlOverride.empty()
        ? EmbeddedHtmlAssetsBaseUrl
        : HtmlAssetsBaseUrlOverride);
    const bool use_version = !asset_prefix.empty();
    if (use_version && asset_prefix[asset_prefix.size() - 1] != '/')
        asset_prefix += "/";
    if (use_version)
    {
        char version[sizeof("255.255")];
        const int version_size = snprintf(version, sizeof(version), "%u.%u",
            static_cast<unsigned>(data_format_major),
            static_cast<unsigned>(data_format_minor));
        if (version_size <= 0 || static_cast<size_t>(version_size) >= sizeof(version))
            return {};
        asset_prefix.append(version, static_cast<size_t>(version_size));
        asset_prefix += "/";
    }

    ntd::string finalizer("\n<!DOCTYPE html>\n<html>\n");
    for (const char* asset : HtmlAssetNames)
    {
        finalizer += "<script src=\"";
        finalizer += asset_prefix;
        finalizer += asset;
        finalizer += "\"></script>\n";
    }
    finalizer += "</html>\n";
    return finalizer;
}

class ExternalToolOutput final : public GPRX::IStreamProcessor
{
public:
    bool ProcessChunk(const uint8_t* data, mpsize_t size) override
    {
        if ((!data && size > 0) || m_bytes_written > GPRX::max_mpdatasz_t - size)
            return false;
        if (size > 0 && !LibMpExtToolOutput(data, size))
            return false;
        m_bytes_written += size;
        return true;
    }
};

bool SetHtmlAssetsBaseUrl(const char* base_url, mpsize_t size)
{
    if (!base_url && size != 0)
        return false;
    if (size == 0)
    {
        HtmlAssetsBaseUrlOverride.clear();
        return true;
    }
    for (mpsize_t i = 0; i < size; ++i)
    {
        if (base_url[i] == '\0')
            return false;
    }
    HtmlAssetsBaseUrlOverride.assign(base_url, size);
    return true;
}

} // namespace

bool ConvertSessionToStream(
    MpSession* session,
    TypesNtd::ContainerFormat format,
    GPRX::IStreamProcessor& output)
{
    if (!session || !session->dumpContainer)
        return false;
    if (format > TypesNtd::ContainerFormat_MAX)
        return false;
    if (session->bSyncRequired && !MpSyncWithDataSource(session))
        return false;
    const auto& validation = session->dumpContainer->GetValidationStatus();
    if (!validation.is_valid)
        return false;

    return ConvertContainerToStream(*session->dumpContainer, format, output);
}

bool ConvertContainerToStream(
    GPRX::ContainerContext& container,
    TypesNtd::ContainerFormat format,
    GPRX::IStreamProcessor& output)
{
    const auto& validation = container.GetValidationStatus();
    // Runtime-created containers have valid format metadata in their magic
    // block but have not gone through the input validation path.
    if (!validation.is_valid && container.GetBlocks().empty())
        return false;

    GPRX::BlockPayloadEncodingMethod encoding = GPRX::BlockPayloadEncodingMethod::Raw;
    bool raw_headers = true;
    bool use_trailer = false;
    const GPRX::StreamFinalizerOptions* finalizer = nullptr;
    GPRX::StreamFinalizerOptions html_finalizer;
    ntd::string html_finalizer_text;

    switch (format)
    {
        case TypesNtd::ContainerFormat_Raw:
            encoding = GPRX::BlockPayloadEncodingMethod::Raw;
            break;
        case TypesNtd::ContainerFormat_Compressed:
            encoding = GPRX::BlockPayloadEncodingMethod::LZ4;
            use_trailer = true;
            break;
        case TypesNtd::ContainerFormat_CompressedAscii:
            encoding = GPRX::BlockPayloadEncodingMethod::LZ4_Base64;
            use_trailer = true;
            raw_headers = false;
            break;
        case TypesNtd::ContainerFormat_CompressedHtml:
            encoding = GPRX::BlockPayloadEncodingMethod::LZ4_Base64;
            use_trailer = true;
            raw_headers = false;
            html_finalizer_text = BuildHtmlFinalizer(
                validation.is_valid
                    ? validation.dataformat_major_version
                    : static_cast<uint8_t>(Version::GetMajor(LocalVersion::DATA_FORMAT)),
                validation.is_valid
                    ? validation.dataformat_minor_version
                    : static_cast<uint8_t>(Version::GetMinor(LocalVersion::DATA_FORMAT)));
            if (html_finalizer_text.empty())
                return false;
            html_finalizer.finalizer_data =
                reinterpret_cast<const uint8_t*>(html_finalizer_text.data());
            html_finalizer.finalizer_size =
                GPRX::to_mpsize(html_finalizer_text.size());
            finalizer = &html_finalizer;
            break;
    }

    return container.SaveToStreamWithEncoding(
        output, encoding, use_trailer, raw_headers, true, finalizer);
}

bool BeginIncrementalHtmlStream(
    GPRX::IncrementalContainerWriter& writer,
    GPRX::IStreamProcessor& output)
{
    return writer.Begin(
        output, GPRX::BlockPayloadEncodingMethod::LZ4_Base64, false);
}

bool FinishIncrementalHtmlStream(GPRX::IncrementalContainerWriter& writer)
{
    const ntd::string finalizer_text = BuildHtmlFinalizer(
        static_cast<uint8_t>(Version::GetMajor(LocalVersion::DATA_FORMAT)),
        static_cast<uint8_t>(Version::GetMinor(LocalVersion::DATA_FORMAT)));
    if (finalizer_text.empty())
        return false;
    const GPRX::StreamFinalizerOptions finalizer = {
        reinterpret_cast<const uint8_t*>(finalizer_text.data()),
        GPRX::to_mpsize(finalizer_text.size()),
    };
    return writer.Finish(&finalizer);
}

} // namespace LibMP

extern "C" MP_API_EXPORT uint32_t MpToolConvertSession(
    MpSession* session, uint32_t format)
{
    LibMP::ExternalToolOutput output;
    return GPRX::bool_to_int(LibMP::ConvertSessionToStream(
        session, static_cast<LibMP::TypesNtd::ContainerFormat>(format), output));
}

extern "C" MP_API_EXPORT bool MpToolSetHtmlAssetsBaseUrl(
    const char* base_url, mpsize_t size)
{
    return LibMP::SetHtmlAssetsBaseUrl(base_url, size);
}

#if !ENABLE_EXTERNAL_TOOLS_OUTPUT

uint32_t LibMpExtToolOutput(const uint8_t* /*data*/, mpsize_t /*size*/)
{
    return 0;
}

#endif
