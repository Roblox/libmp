// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// std hash/comparator wrappers plus an allocator-routed callable when a custom
// ntd memory backend is selected. C++17 std::function has no allocator parameter.

#include "ntd_config.h"

#if NTD_TYPES_DISTINCT_FROM_STD
    #include "string.h"
namespace ntd
{
namespace detail
{
struct StringHash
{
    size_t operator()(const string& value) const noexcept
    {
        // FNV-1a: compact, deterministic, and sufficient for the compatibility
        // layer's general-purpose hash tables.
        size_t result = static_cast<size_t>(2166136261u);
        for (char byte : value)
        {
            result ^= static_cast<unsigned char>(byte);
            result *= static_cast<size_t>(16777619u);
        }
        return result;
    }
};
} // namespace detail
} // namespace ntd
#endif

#if NTD_OWN_STDLIB
// Hash and comparator definitions are reserved for the embedded STL-primitives
// implementation.
#else
#include <functional>
namespace ntd
{
using std::equal_to;
#if NTD_ROUTED_STD_TYPES
template <class K>
struct hash : std::hash<K>
{
};
template <>
struct hash<string> : detail::StringHash {};
#else
using std::hash;
#endif
using std::less;
} // namespace ntd
#endif

#if !NTD_TYPES_DISTINCT_FROM_STD // ---------------------------- std passthrough

namespace ntd
{
using std::function;
} // namespace ntd

#else // ----------------------------------------- allocator-routed ntd function
// Amalgamation expands the derived NTD_TYPES_DISTINCT_FROM_STD predicate below.
// @amalgamate: config-begin ntd-function NTD_OWN_STDLIB || NTD_EMBEDDED_ALLOC || NTD_EXTERNAL_MALLOC

#include "allocator.h"   // ntd::New / ntd::Delete
#include "type_traits.h" // enable_if / is_same / decay / nullptr_t
#include "utility.h"     // move
#include <new>

namespace ntd
{
template <class Sig> class function;

template <class R, class... A>
class function<R(A...)>
{
    struct Base : mem::RoutedAllocation
    {
        virtual R call(A...) = 0;
        virtual Base* clone() const = 0;
        virtual ~Base() = default;
    };
    template <class F>
    struct Holder : Base
    {
        F f;
        explicit Holder(const F& ff) : f(ff) {}
        explicit Holder(F&& ff) : f(ntd::move(ff)) {}
        R call(A... a) override { return f(static_cast<A&&>(a)...); }
        Base* clone() const override { return ntd::New<Holder>(f); }
    };

public:
    function() noexcept : m_p(nullptr) {}
    function(nullptr_t) noexcept : m_p(nullptr) {}

    template <class F, class = enable_if_t<!is_same_v<decay_t<F>, function>>>
    function(F f) : m_p(ntd::New<Holder<decay_t<F>>>(ntd::move(f)))
    {
    }

    function(const function& o) : m_p(o.m_p ? o.m_p->clone() : nullptr) {}
    function(function&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
    ~function() { ntd::Delete(m_p); }

    function& operator=(const function& o)
    {
        if (this != &o)
        {
            ntd::Delete(m_p);
            m_p = o.m_p ? o.m_p->clone() : nullptr;
        }
        return *this;
    }
    function& operator=(function&& o) noexcept
    {
        if (this != &o)
        {
            ntd::Delete(m_p);
            m_p = o.m_p;
            o.m_p = nullptr;
        }
        return *this;
    }
    function& operator=(nullptr_t) noexcept
    {
        ntd::Delete(m_p);
        m_p = nullptr;
        return *this;
    }

    R operator()(A... a) const { return m_p->call(static_cast<A&&>(a)...); }
    explicit operator bool() const noexcept { return m_p != nullptr; }

private:
    Base* m_p;
};

} // namespace ntd

// @amalgamate: config-end ntd-function
#endif // NTD_TYPES_DISTINCT_FROM_STD
