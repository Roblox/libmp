// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "c_api_private.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/functional.h"
#include "ntd_stdlib/utility.h"

using namespace GPRX;
using namespace LibMP;

////////////////////////////////////////////////////////////////////////////////////////////////////
class PatternMatcher
{
public:
    constexpr static mpsize_t MAX_MASK_LEN_ON_STACK = 1024 * 4;

    struct Entry
    {
        const char* name;
        uint32_t id;
    };

    struct PathSegment 
    { 
        const char* start;
        const char* end; 
        enum Type { Regular, SingleWildcard, RecursiveWildcard } type;

        bool Matches(const char* name, bool caseSensitive) const
        {
            // * and ** both match any string text
            if (type == SingleWildcard || type == RecursiveWildcard)
                return true;
            return IsMatch(name, start, end, caseSensitive);
        }
    };

    static void CopyToLower(char* dest, const char* src, mpsize_t len)
    {
        ntd::transform(
            src, src + len, dest,
            [](char c) { return static_cast<char>(ntd::tolower(static_cast<unsigned char>(c))); }
        );
        dest[len] = '\0';
    }

    static inline bool ChrCmp(char a, char b, bool sensitive)
    {
        if (sensitive)
            return a == b;
        return ntd::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
    }

    static inline bool IsSlash(char c)
    {
        return (c == '/' || c == '\\');
    }

    static inline bool IsSlash(char* c)
    {
        return IsSlash(*c);
    }

    static ntd::pair<bool, bool> ParseStandardPathMask(char* mask, ntd::vector<PathSegment>& outSegments);

    static bool IsMatch(const char* str, const char* mask_start, const char* mask_end, bool sensitive = false);

    /**
     * Scans strings for matches.
     * @param accessor Lambda {string, id} for index 'i'
     * @param count Total items to check
     * @param multi_mask "foo*|Bar"
     * @param max_results Limit for result vector
     */
    static ntd::vector<uint32_t> FindMatches(
        ntd::function<Entry(mpsize_t)> accessor,
        mpsize_t count,
        const char* multi_mask,
        bool caseSensitive = false,
        mpsize_t max_results = 0);
};

ntd::pair<bool, bool> PatternMatcher::ParseStandardPathMask(char* mask, ntd::vector<PathSegment>& outSegments)
{
    outSegments.clear();
    if (!mask || !*mask)
        return {};

    // Skip the leading slash. Treat "/foo/bar" the same as "foo/bar".
    // However, "/foo" explicitly refers to the root level,
    // while "foo" may appear at any level in the hierarchy.
    // "foo$" explicitly requires a leaf node (see below).
    bool requireRoot = false;
    char* p = mask;
    if (IsSlash(p))
    {
        requireRoot = true;
        p++;
    }
    char* start = p;

    auto addSegment = [&](const char* s, const char* e) 
    {
        if (e > s) 
        {
            PathSegment::Type t = PathSegment::Regular;
            mpsize_t len = GPRX::to_mpsize(e - s);
            if (len == 2 && s[0] == '*' && s[1] == '*')
                t = PathSegment::RecursiveWildcard;
            else if (len == 1 && s[0] == '*')
                t = PathSegment::SingleWildcard;
            outSegments.push_back({ s, e, t });
        }
    };

    while (*p)
    {
        if (IsSlash(p)) {
            addSegment(start, p);
            start = p + 1;
        }
        p++;
    }

    // Detect leaf node requirement
    mpsize_t len = GPRX::to_mpsize(p - start);
    bool requireLeaf = (len > 0 && *(p - 1) == '$');
    if (requireLeaf)
    {
        // Treat '.../$' the same as '.../*$'
        if (len == 1)
        {
            *(p - 1) = '*';
        }
        else
        {
            p--;
            *p = '\0';
        }
    }

    addSegment(start, p);
    return { requireRoot, requireLeaf };
}

bool PatternMatcher::IsMatch(const char* str, const char* mask_start, const char* mask_end, bool sensitive)
{
    const char* s = str;
    const char* m = mask_start;
    
    // Backtracking markers
    const char* star_m = nullptr;
    const char* star_s = nullptr;

    while (*s)
    {
        // Treat reaching 'mask_end' as if we hit a null terminator
        bool mask_finished = (m >= mask_end);
        char m_char = mask_finished ? '\0' : *m;

        // Exact match or single char wildcards
        if (!mask_finished && (ChrCmp(*s, m_char, sensitive) || m_char == '?' || m_char == '#'))
        {
            s++; 
            m++;
        } 
        // Multi-char wildcard
        else if (!mask_finished && m_char == '*')
        {
            star_m = m++; // Advance mask, store position
            star_s = s;   // Store string position
        } 
        // Mismatch - backtrack if we saw a star previously
        else if (star_m)
        {
            m = star_m + 1; // Reset mask to after star
            s = ++star_s;   // Consume one char from string and retry
        } 
        // Mismatch - no recovery
        else
        {
            return false;
        }
    }

    // Handle trailing stars
    while (m < mask_end && *m == '*')
    {
        m++;
    }

    // Match is successful only if we consumed the entire pattern segment
    return m == mask_end;
}

ntd::vector<uint32_t> PatternMatcher::FindMatches(
    ntd::function<Entry(mpsize_t)> accessor,
    mpsize_t count,
    const char* multi_mask,
    bool caseSensitive,
    mpsize_t max_results)
{
    ntd::vector<uint32_t> results;
    if (max_results > 0)
        results.reserve(ntd::min(count, max_results));

    const char* mask = multi_mask;
    const mpsize_t mask_len = GPRX::to_mpsize(
        ntd::strnlen(mask, MAX_MASK_LEN_ON_STACK));
    if (mask_len == MAX_MASK_LEN_ON_STACK)
    {
        XASSERT_DIAG(false,
            "PatternMatcher - mask must be null-terminated within MAX_MASK_LEN_ON_STACK");
        return results;
    }

    if (!caseSensitive)
    {
        char* mask_lower = static_cast<char*>(alloca(mask_len + 1)); // +1 for null terminator
        CopyToLower(mask_lower, mask, mask_len);
        mask = mask_lower;
    }

    const char* p_start = mask;
    const char* p_end = nullptr;

    // Loop through the multi string
    while (p_start != nullptr && *p_start != '\0')
    {
        if (max_results > 0 && results.size() >= max_results)
            break;

        // Find the delimiter '|' or the null terminator
        const char* delimiter = p_start;
        while (*delimiter != '\0' && *delimiter != '|')
        {
            delimiter++;
        }
        p_end = delimiter; // p_end points to '|' or '\0'

        // Inner Loop: Check all names against current segment [p_start, p_end)
        for (mpsize_t i = 0; i < count; i++)
        {
            Entry entry = accessor(i);
            if (entry.name && IsMatch(entry.name, p_start, p_end, caseSensitive))
            {
                if (results.size() >= GPRX::max_mpsize_t)
                {
                    XASSERT_DIAG(false,
                        "PatternMatcher::FindMatches - result count exceeds mpsize_t");
                    return results;
                }
                results.push_back(entry.id);
                if (max_results > 0 && results.size() >= max_results)
                    return results;
            }
        }

        // Advance p_start. If we hit '\0', stop. If we hit '|', skip over it.
        if (*p_end == '\0')
            p_start = nullptr;
        else
            p_start = p_end + 1;
    }

    return results;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Finds counters matching the specified name pattern, checking their full path in the hierarchy
static mpsize_t MpFindCountersImpl(MpSession* session, char* mask, bool caseSensitive, mpsize_t limit, ntd::vector<uint32_t>& outResults)
{
    mpsize_t result_count = 0;

    ntd::vector<PatternMatcher::PathSegment> segments;
    const auto [ requireRoot, requireLeaf ] = PatternMatcher::ParseStandardPathMask(mask, segments);

    if (segments.empty())
        return 0;
    if (segments.size() > static_cast<size_t>(GPRX::max_mpssize_t))
    {
        XASSERT_DIAG(false,
            "MpFindCountersImpl - mask segment count exceeds mpssize_t");
        return 0;
    }

    bool skipHierarchyCheck = (!requireRoot && segments.size() == 1);
    const PatternMatcher::PathSegment& leaf = segments.back();
    
    // Calculate minimum depth required (recursive ** segments cost 0 depth, others cost 1)
    mpsize_t minDepth = 0;
    for (const auto& s : segments)
    {
        if (s.type != PatternMatcher::PathSegment::RecursiveWildcard)
            minDepth++;
    }

    MpArray* counterIdsArr = MpGetCounterIds(session);
    mpsize_t counterIdsArrSize = MpGetArraySize(counterIdsArr);
    auto get_parent_desc = [&](uint32_t parent_id)
        -> const LibMP::TypesNtd::CounterDesc*
    {
        auto parent_obj = MpGetCounterDesc(session, parent_id);
        if (!parent_obj)
        {
            XASSERT_DIAG(false,
                "MpFindCountersImpl - counter parent ID has no descriptor");
            return nullptr;
        }
        return flatbuffers::GetRoot<LibMP::TypesNtd::CounterDesc>(parent_obj);
    };
    for (mpsize_t i = 0; i < counterIdsArrSize; i++)
    {
        uint32_t id = MpGetIdFromArray(session, counterIdsArr, i);

        auto obj = MpGetCounterDesc(session, id);
        if (!obj)
            continue;
        auto desc = flatbuffers::GetRoot<LibMP::TypesNtd::CounterDesc>(obj);

        // --- Optimization 0: Leaf Requirement ---
        if (requireLeaf && desc->first_child() > 0)
            continue; // User asked for a leaf ($), but this counter has a child.

        // --- Optimization 1: Level Pruning ---
        // level() is 0-based (level 0 = 1 node deep)
        // If we need 3 specific nodes (A/B/C), level must be at least 2.
        if (static_cast<mpsize_t>(desc->level()) + 1 < minDepth)
            continue;

        // --- Optimization 2: Fast Leaf Check ---
        // If the leaf is NOT **, the counter's name must match it exactly/wildcard.
        if (!desc->counter_name())
            continue;
        if (leaf.type != PatternMatcher::PathSegment::RecursiveWildcard)
            if (!leaf.Matches(desc->counter_name()->c_str(), caseSensitive))
                continue;

        // --- Optimization 3: Bottom-Up Traversal ---
        uint32_t currentParentId = desc->parent();
        
        // Start matching from the segment BEFORE the leaf
        // (If leaf was **, we start matching FROM the leaf, effectively)
        // segIdx may take negative values.
        mpssize_t segIdx = static_cast<mpssize_t>(segments.size()) -
            (leaf.type == PatternMatcher::PathSegment::RecursiveWildcard ? 1 : 2);

        // While we have segments left to match...
        while (segIdx >= 0)
        {
            const auto& segment = segments[static_cast<size_t>(segIdx)];

            if (segment.type == PatternMatcher::PathSegment::RecursiveWildcard)
            {
                // [Recursive **]
                // "Absorb" parents until we match the NEXT segment (segIdx - 1)

                if (segIdx == 0)
                {
                    // Case: "**/foo". We reached the top of mask.
                    // Any remaining parents are valid. Match!
                    segIdx--;
                    continue;
                }

                // We need to find the specific ancestor that matches the segment BEFORE the **
                const auto& target = segments[static_cast<size_t>(segIdx - 1)];
                bool foundTarget = false;

                // Scan upwards
                mpsize_t ancestorsVisited = 0;
                while (currentParentId > 0 && ancestorsVisited++ < counterIdsArrSize)
                {
                    const auto* pDesc = get_parent_desc(currentParentId);
                    if (!pDesc)
                        break;
                    if (!pDesc->counter_name())
                        break;

                    if (target.Matches(pDesc->counter_name()->c_str(), caseSensitive))
                    {
                        // Found it! Lock this parent as the match for 'target'
                        foundTarget = true;
                        currentParentId = pDesc->parent(); // Move past it
                        break;
                    }

                    // Not a match yet, keep consuming parents via **
                    currentParentId = pDesc->parent();
                }

                if (!foundTarget)
                {
                    if (currentParentId > 0 && ancestorsVisited >= counterIdsArrSize)
                    {
                        XASSERT_DIAG(false,
                            "MpFindCountersImpl - counter hierarchy exceeds counter count");
                    }
                    break;
                }

                // We consumed "**" AND "target"
                segIdx -= 2;
            }
            else
            {
                // [Regular or Single *]
                // The immediate parent MUST match this segment.

                if (currentParentId == 0)
                    break;

                const auto* pDesc = get_parent_desc(currentParentId);
                if (!pDesc)
                    break;

                if (!pDesc->counter_name() ||
                    !segment.Matches(pDesc->counter_name()->c_str(), caseSensitive))
                    break;

                currentParentId = pDesc->parent();
                segIdx--;
            }
        }

        if (segIdx >= 0)
            continue;

        // Without a leading "**", a hierarchical mask must reach the root.
        const bool maskStartsRecursive =
            segments[0].type == PatternMatcher::PathSegment::RecursiveWildcard;
        if (!skipHierarchyCheck && !maskStartsRecursive && currentParentId > 0)
            continue;

        if (outResults.size() >= GPRX::max_mpsize_t)
        {
            XASSERT_DIAG(false,
                "MpFindCountersImpl - result count exceeds mpsize_t");
            break;
        }
        outResults.push_back(id);
        result_count++;
        if (limit > 0 && result_count >= limit)
            break;
    }
    MpReleaseArray(counterIdsArr);

    return result_count;
}

// Wrapper around MpFindCountersImpl that accepts multiple path masks and processes them sequentially
static ntd::vector<uint32_t> MpFindCounters(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit = 0)
{
    ntd::vector<uint32_t> results;
    if (!nameMask || *nameMask == '\0')
        return results;

    const mpsize_t len = GPRX::to_mpsize(
        ntd::strnlen(nameMask, PatternMatcher::MAX_MASK_LEN_ON_STACK));
    if (len == PatternMatcher::MAX_MASK_LEN_ON_STACK)
    {
        XASSERT_DIAG(false,
            "PatternMatcher - mask must be null-terminated within MAX_MASK_LEN_ON_STACK");
        return results;
    }
    
    char* mask = static_cast<char*>(alloca(len + 1)); // +1 for null terminator
    if (caseSensitive)
    {
        ntd::memcpy(mask, nameMask, len + 1);
    }
    else
    {
        PatternMatcher::CopyToLower(mask, nameMask, len);
    }

    bool hasLimit = limit > 0;
    
    char* start = mask;
    char* current = mask;

    while (*current != '\0')
    {
        // Global limit check
        if (hasLimit && results.size() >= limit)
            break;

        if (*current == '|')
        {
            *current = '\0'; // Terminate current sub-mask

            // Pass the stack buffer segment to the finder
            mpsize_t added = MpFindCountersImpl(session, start, caseSensitive, limit, results);
            limit -= hasLimit ? added : 0;

            start = current + 1; // Move to next segment
        }
        current++;
    }

    // Process the final segment
    if (!hasLimit || results.size() < limit)
        MpFindCountersImpl(session, start, caseSensitive, limit, results);

    return results;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
inline static uint32_t FirstOrInvalid(const ntd::vector<uint32_t>& ids)
{
    return ids.empty() ? 0 : ids[0];
}

static MpArray* ToMpArray(ntd::vector<uint32_t> ids) // Pass by value to move
{
    if (ids.size() > GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false, "ToMpArray - value count exceeds mpsize_t");
        return nullptr;
    }

    MpArray* array = MpCreateArray();
    array->value_size_in_u32 = 1;
    array->values_count = GPRX::to_mpsize(ids.size());
    array->entries = ntd::move(ids);
    return array;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename FGetName>
static auto FindObjectsInIdsRange(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit, uint32_t maxId, FGetName getNameFunc)
{
    mpsize_t count = maxId;
    auto accessor = [session, getNameFunc](mpsize_t i) -> PatternMatcher::Entry
    {
        uint32_t id = i + 1;
        const char* name = getNameFunc(session, id);
        return { name, id };
    };
    return PatternMatcher::FindMatches(accessor, count, nameMask, caseSensitive, limit);
}

template <typename FGetIds, typename FGetName>
static auto FindObjectsInIdsArray(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit, FGetIds getIdsFunc, FGetName getNameFunc)
{
    MpArray* idsArr = getIdsFunc(session);
    mpsize_t arrSize = MpGetArraySize(idsArr);
    auto accessor = [session, idsArr, getNameFunc](mpsize_t i) -> PatternMatcher::Entry
    {
        uint32_t id = MpGetIdFromArray(session, idsArr, i);
        const char* name = getNameFunc(session, id);
        return { name, id };
    };
    auto res = PatternMatcher::FindMatches(accessor, arrSize, nameMask, caseSensitive, limit);
    MpReleaseArray(idsArr);
    return res;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
static auto MpFindGroups(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit = 0)
{
    return FindObjectsInIdsRange(
        session, nameMask, caseSensitive, limit,
        MpGetGroupIdMax(session),
        [](MpSession* s, uint32_t id)
        {
            auto obj = MpGetGroupDesc(s, id);
            if (!obj)
                return static_cast<const char*>(nullptr);
            auto desc = flatbuffers::GetRoot<LibMP::TypesNtd::GroupDesc>(obj);
            return desc->group_name() ? desc->group_name()->c_str() : nullptr;
        }
    );
}

static auto MpFindTimers(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit = 0)
{
    return FindObjectsInIdsArray(
        session, nameMask, caseSensitive, limit,
        MpGetTimerIds,
        [](MpSession* s, uint32_t id)
        {
            auto obj = MpGetTimerDesc(s, id);
            if (!obj)
                return static_cast<const char*>(nullptr);
            auto desc = flatbuffers::GetRoot<LibMP::TypesNtd::TimerDesc>(obj);
            return desc->timer_name() ? desc->timer_name()->c_str() : nullptr;
        }
    );
}

static auto MpFindThreads(MpSession* session, const char* nameMask, bool caseSensitive, mpsize_t limit = 0)
{
    return FindObjectsInIdsArray(
        session, nameMask, caseSensitive, limit,
        MpGetThreadIds,
        [](MpSession* s, uint32_t id)
        {
            auto obj = MpGetThreadDesc(s, id);
            if (!obj)
                return static_cast<const char*>(nullptr);
            auto desc = flatbuffers::GetRoot<LibMP::TypesNtd::ThreadDesc>(obj);
            return desc->thread_name() ? desc->thread_name()->c_str() : nullptr;
        }
    );
}

////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t MpFindGroupId(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return 0;
    return FirstOrInvalid(MpFindGroups(session, nameMask, !!caseSensitive, 1));
}

MpArray* MpFindGroupIds(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return nullptr;
    return ToMpArray(MpFindGroups(session, nameMask, !!caseSensitive));
}

uint32_t MpFindTimerId(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return 0;
    return FirstOrInvalid(MpFindTimers(session, nameMask, !!caseSensitive, 1));
}

MpArray* MpFindTimerIds(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return nullptr;
    return ToMpArray(MpFindTimers(session, nameMask, !!caseSensitive));
}

uint32_t MpFindCounterId(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return 0;
    return FirstOrInvalid(MpFindCounters(session, nameMask, !!caseSensitive, 1));
}

MpArray* MpFindCounterIds(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return nullptr;
    return ToMpArray(MpFindCounters(session, nameMask, !!caseSensitive));
}

uint32_t MpFindThreadId(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return 0;
    return FirstOrInvalid(MpFindThreads(session, nameMask, !!caseSensitive, 1));
}

MpArray* MpFindThreadIds(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return nullptr;
    return ToMpArray(MpFindThreads(session, nameMask, !!caseSensitive));
}

MpArray* MpGetTimerIdsByGroupId(MpSession* session, uint32_t groupId)
{
    CheckSync(session);
    if (!session)
        return nullptr;
    ntd::vector<uint32_t> matchingIds;
    MpArray* idsArr = MpGetTimerIds(session);
    mpsize_t count = MpGetArraySize(idsArr);
    for (mpsize_t i = 0; i < count; i++)
    {
        uint32_t id = MpGetIdFromArray(session, idsArr, i);
        auto obj = MpGetTimerDesc(session, id);
        if (!obj)
            continue;
        auto desc = flatbuffers::GetRoot<LibMP::TypesNtd::TimerDesc>(obj);
        if (desc->group_id() == groupId)
            matchingIds.push_back(id);
    }
    MpReleaseArray(idsArr);
    return ToMpArray(ntd::move(matchingIds));
}

MpArray* MpGetTimerIdsByGroupNames(MpSession* session, const char* nameMask, uint32_t caseSensitive)
{
    CheckSync(session);
    if (!session || !nameMask)
        return nullptr;
    auto ids = FindObjectsInIdsArray(
        session, nameMask, !!caseSensitive, 0,
        MpGetTimerIds,
        [](MpSession* s, uint32_t timerId)
        {
            auto tObj = MpGetTimerDesc(s, timerId);
            if (!tObj)
                return static_cast<const char*>(nullptr);
            auto tDesc = flatbuffers::GetRoot<LibMP::TypesNtd::TimerDesc>(tObj);
            auto gObj = MpGetGroupDesc(s, tDesc->group_id());
            if (!gObj)
                return static_cast<const char*>(nullptr);
            auto gDesc = flatbuffers::GetRoot<LibMP::TypesNtd::GroupDesc>(gObj);
            return gDesc->group_name() ? gDesc->group_name()->c_str() : nullptr;
        }
    );
    return ToMpArray(ntd::move(ids));
}
