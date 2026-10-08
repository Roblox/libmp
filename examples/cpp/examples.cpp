// examples.cpp -- LibMP C++ examples
//
// Build as part of the native build, from the repository root:
//   python3 src/scripts/build.py --native
// This produces the libmp_examples executable under src/build/native
// (src/build/native/Release/libmp_examples.exe with MSVC).
//
// Or build standalone: download libmp.h and libmp.cpp from
// https://github.com/Roblox/libmp/releases, or generate them with
// `python3 src/scripts/amalgamate.py` (output: src/build/amalgamated). Place
// them beside this file and compile with LIBMP_AMALGAMATED defined:
//   c++ -std=c++17 -O2 -DLIBMP_AMALGAMATED examples.cpp libmp.cpp -o libmp_examples
//   (with clang on Windows, name the output libmp_examples.exe)
//   cl /std:c++17 /EHsc /O2 /DLIBMP_AMALGAMATED examples.cpp libmp.cpp /Fe:libmp_examples.exe
//
// Run: libmp_examples --threads|--counters|--svg /path/to/capture.x.html (or capture.gprx)
// (--svg writes timeline.svg to the current directory)
//
// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#ifdef LIBMP_AMALGAMATED
#include "libmp.h"
#else
#include "mp_api/cpp_api.h"
#endif

#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>
#include <map>
#include <vector>

constexpr int errorCode = 1;

static std::unique_ptr<LibMP::Session> OpenCapture(int argc, char** argv)
{
    if (argc != 1)
    {
        std::cerr << "This example requires one argument: the input .x.html or .gprx path.\n";
        return {};
    }

    const std::string inputPath = argv[0];
    auto session = LibMP::Session::OpenFromFile(inputPath);
    if (!session)
    {
        std::cerr << "Could not open '" << inputPath << "'.\n";
        return session;
    }
    if (!session->IsValid())
    {
        std::cerr << "'" << inputPath << "' is not a valid GPRX capture.\n";
        return {};
    }
    return session;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// List all thread names found in the dump

static int ListThreads(int argc, char** argv)
{
    auto session = OpenCapture(argc, argv);
    if (!session)
        return errorCode;
    auto tids = session->GetThreadIds();
    std::cout << "Threads:" << std::endl;
    for (mpsize_t i = 0; i < tids.Size(); i++)
    {
        uint32_t tid = tids[i];
        auto thread = session->GetThreadDesc(tid);
        if (thread && thread->thread_name())
            std::cout << thread->thread_name()->c_str() << std::endl;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Display the counter hierarchy from the dump

static void PrintIndent(uint32_t level)
{
    for (uint32_t i = 0; i < level; ++i)
    {
        std::cout << "  ";
    }
}

static void PrintCounterName(LibMP::Session* session, uint32_t counterId)
{
    const auto* desc = session->GetCounterDesc(counterId);
    if (desc)
    {
        std::cout << desc->counter_name()->c_str();
    }
    else
    {
        std::cout << "Unknown_ID_" << counterId;
    }
}

static void IterateCounterSubtrees(LibMP::Session* session, const std::vector<uint32_t>& rootIds)
{
    bool hasRoots = !rootIds.empty();
    if (hasRoots)
        std::cout << "\n*** Iterating selected counter roots ***\n";
    else
        std::cout << "\n*** Iterating all counter roots ***\n";

    auto iter = session->CreateCounterIterator();
    auto state = iter.GetState();

    LibMP::Types::CounterIteratorConfigT config;
    config.root_counter_ids = rootIds;
    iter.Configure(config);

    while (iter.Step())
    {
        PrintIndent(state->relative_level());

        std::cout << "- ";
        PrintCounterName(session, state->counter_id());

        std::cout << " (Lvl: " << state->level();
        if (hasRoots)
        {
            std::cout << " Rel_Lvl: " << state->relative_level() <<  " Root_Lvl: " << state->root_level();
        }
        std::cout << ", Leaf: " << (state->is_leaf() ? "Yes" : "No") << ", ID: " << state->counter_id() << ")";

        mpsize_t stackSize = 0;
        const uint32_t* stack = iter.GetStack(&stackSize);
        if (state->hierarchy_changed())
        {
            uint32_t level = state->level();
            std::cout << " [Path Size: " << (level + 1) << "]";
            std::cout << " -- ";
            for (uint32_t i = 0; i <= level; i++)
            {
                std::cout << "/" << stack[i];
            }
        }

        std::cout << "\n";
    }
}

static int ListCounters(int argc, char** argv)
{
    auto session = OpenCapture(argc, argv);
    if (!session)
        return errorCode;

    auto rootCounterIds = session->FindCounterIds("/memory/Luau");
    IterateCounterSubtrees(session.get(), std::vector<uint32_t>{});
    IterateCounterSubtrees(session.get(), rootCounterIds);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Render the dump as an SVG timeline visualization

// Simple SVG writer helper
class SvgWriter
{
    std::ofstream m_file;

public:
    SvgWriter(const std::string& filename, float width, float height)
    {
        m_file.open(filename);
        m_file << "<?xml version=\"1.0\" standalone=\"no\"?>\n";
        m_file << "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n";
        m_file << "<svg width=\"" << width << "\" height=\"" << height << "\" xmlns=\"http://www.w3.org/2000/svg\">\n";

        // Background
        m_file << "<rect width=\"100%\" height=\"100%\" fill=\"#202020\" />\n";
    }

    ~SvgWriter()
    {
        m_file << "</svg>\n";
        m_file.close();
    }

    // Convert integer color (0xRRGGBB) to hex string
    static std::string HexColor(uint32_t color)
    {
        std::stringstream ss;
        ss << "#" << std::hex << std::setw(6) << std::setfill('0') << (color & 0xFFFFFF);
        return ss.str();
    }

    // Escape characters that would otherwise be parsed as XML markup
    static std::string EscapeXml(const std::string& text)
    {
        std::string result;
        for (char c : text)
        {
            switch (c)
            {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            default: result += c; break;
            }
        }
        return result;
    }

    void DrawRect(float x, float y, float w, float h, uint32_t color, float opacity = 1.0f)
    {
        // Simple culling for very small blocks
        if (w < 0.5f)
            return;
        m_file << "<rect x=\"" << x << "\" y=\"" << y
               << "\" width=\"" << w << "\" height=\"" << h
               << "\" fill=\"" << HexColor(color) << "\" fill-opacity=\"" << opacity << "\" stroke=\"black\" stroke-width=\"0.5\" />\n";
    }

    void DrawText(float x, float y, const std::string& text, uint32_t color = 0xFFFFFF) {
        if (text.empty())
            return;
        m_file << "<text x=\"" << x << "\" y=\"" << y
               << "\" fill=\"" << HexColor(color) << "\" font-family=\"Arial\" font-size=\"12\">"
               << EscapeXml(text) << "</text>\n";
    }
};

// Main logic
int RenderToSvg(int argc, char** argv)
{
    const std::string outputPath = "timeline.svg";

    auto session = OpenCapture(argc, argv);
    if (!session)
        return errorCode;

    // Layout configuration
    const mpsize_t MAX_FRAMES = 10;
    const float WIDTH = 20000.0f;    // Width of the rendered frame range
    const float ROW_HEIGHT = 170.0f; // Height per thread row
    const float BAR_HEIGHT = 15.0f;  // Height of one stack frame

    // Fit the first MAX_FRAMES frames into the timeline width.
    // Fetch copies the descriptors; a GetFrameDesc view is only valid until the next query.
    uint32_t startFrameId = session->GetFrameIdMin();
    uint32_t endFrameId = std::min<uint32_t>(startFrameId + MAX_FRAMES - 1, session->GetFrameIdMax());
    auto startFrame = session->FetchFrameDesc(startFrameId);
    auto endFrame = session->FetchFrameDesc(endFrameId);
    if (!startFrame || !endFrame || endFrame->tick_end_cpu <= startFrame->tick_start_cpu)
    {
        std::cerr << "The capture has no frames to render.\n";
        return errorCode;
    }
    uint64_t startTick = startFrame->tick_start_cpu;
    float scaleX = WIDTH / (float)(endFrame->tick_end_cpu - startTick); // Pixels per tick

    // Scopes that started before the first frame are clipped at the left edge
    auto tickToX = [&](uint64_t tick) { return tick > startTick ? (float)(tick - startTick) * scaleX : 0.0f; };

    auto tids = session->GetThreadIds();
    SvgWriter svg(outputPath, WIDTH, tids.Size() * ROW_HEIGHT + 100.0f);

    // Assign vertical space for threads
    std::map<uint32_t, float> threadYOffsets;
    {
        float currentY = 50.0f;
        for (mpsize_t i = 0; i < tids.Size(); i++)
        {
            uint32_t tid = tids[i];
            auto thread = session->GetThreadDesc(tid);
            if (thread && !thread->is_gpu())
            {
                // Draw thread labels
                if (thread->thread_name())
                    svg.DrawText(10.0f, currentY - 10.0f, "Thread: " + std::string(thread->thread_name()->c_str()));
                // Draw baseline
                svg.DrawRect(0, currentY, WIDTH, 1.0f, 0x404040);

                threadYOffsets[tid] = currentY;
                currentY += ROW_HEIGHT;
            }
        }
    }

    // Prepare for log iteration
    auto iter = session->CreateLogIterator();

    // Configure to match our thread filters
    LibMP::Types::LogIteratorConfigT config;
    config.skip_gpu_threads = true;
    config.start_frame_id = startFrameId;
    config.end_frame_id = endFrameId;
    iter.Configure(config);

    while(iter.Step())
    {
        auto state = iter.GetState();

        // Skip exits of scopes nested deeper than the iterator's thread stack can track
        if (state->is_exit() && !state->thread_stack_was_overflowed())
        {
            mpsize_t depth = state->thread_stack_depth();
            uint64_t enterTime = startTick;

            // An underflowed stack means the scope was entered before the first frame.
            // Otherwise, look at the element that was just popped from the stack to find
            // when this scope started.
            if (!state->thread_stack_is_underflowed())
            {
                auto stackElem = iter.GetCurrentThreadStackElement(depth);
                if (!stackElem)
                    continue;
                enterTime = stackElem->enter_timestamp();
            }
            uint64_t exitTime = state->timestamp();

            // Calculate coordinates
            float startX = tickToX(enterTime);
            float endX = tickToX(exitTime);
            float width = endX - startX;

            // Lookup thread Y position
            float threadY = threadYOffsets[state->thread_id()];
            float barY = threadY + (depth * BAR_HEIGHT);

            // Get color/name
            auto timer = session->GetTimerDesc(state->timer_id());
            if (!timer)
                continue;

            // Draw
            svg.DrawRect(startX, barY, width, BAR_HEIGHT - 2.0f, timer->color());

            // Only draw text if the box is wide enough
            if (width > 20.0f && timer->timer_name())
            {
                svg.DrawText(startX + 2.0f, barY + 11.0f, timer->timer_name()->c_str(), 0x000000);
            }
        }
    }

    std::cout << "SVG saved to: " << outputPath << std::endl;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Entry point with example selector

using ExampleFunction = int (*)(int, char**);

struct Example
{
    const char* name;
    const char* description;
    ExampleFunction function;
};

constexpr Example examples[] = {
    { "--threads", "list thread names", ListThreads },
    { "--counters", "display the counter hierarchy", ListCounters },
    { "--svg", "render an SVG timeline", RenderToSvg },
};

static void PrintUsage(std::ostream& output, const char* executable)
{
    output << "Usage: " << executable << " <example> <capture.x.html|capture.gprx>\n\nExamples:\n";
    for (const Example& example : examples)
        output << "  " << example.name << "\t" << example.description << '\n';
}

int main(int argc, char** argv)
{
    LibMP::Init();

    if (argc < 2)
    {
        PrintUsage(std::cerr, argv[0]);
        return errorCode;
    }

    const std::string selected = argv[1];
    if (selected == "--help" || selected == "-h")
    {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }

    for (const Example& example : examples)
    {
        if (selected == example.name)
            return example.function(argc - 2, argv + 2);
    }

    std::cerr << "Unknown example '" << selected << "'.\n";
    PrintUsage(std::cerr, argv[0]);
    return errorCode;
}
