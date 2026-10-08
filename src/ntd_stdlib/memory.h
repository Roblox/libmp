// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Standard smart-pointer aliases by default. Custom ntd memory backends use
// small ownership adapters so destruction returns storage to ntd::mem.

#include "ntd_config.h"

#if !NTD_TYPES_DISTINCT_FROM_STD // ---------------------------- std passthrough

#include <memory>
#include <type_traits>
#include <utility>
namespace ntd
{
namespace detail
{
template <class T>
struct StdDefaultDelete
{
    using type = std::default_delete<T>;
};
template <class T>
struct StdDefaultDelete<T[]>
{
    static_assert(!std::is_same<T, T>::value,
        "ntd::default_delete<T[]> is unsupported; use ntd::vector<T>");
};

template <class T, class D>
struct StdUniquePtr
{
    using type = std::unique_ptr<T, D>;
};
template <class T, class D>
struct StdUniquePtr<T[], D>
{
    static_assert(!std::is_same<T, T>::value,
        "ntd::unique_ptr<T[]> is unsupported; use ntd::vector<T>");
};
} // namespace detail

template <class T>
using default_delete = typename detail::StdDefaultDelete<T>::type;
template <class T, class D = std::default_delete<T>>
using unique_ptr = typename detail::StdUniquePtr<T, D>::type;

template <class T, class... Args>
unique_ptr<T> make_unique(Args&&... args)
{
    return std::make_unique<T>(std::forward<Args>(args)...);
}

using std::make_shared;
using std::shared_ptr;
} // namespace ntd

#else // ------------------------------------------- allocator-routed ntd memory
// Amalgamation expands the derived NTD_TYPES_DISTINCT_FROM_STD predicate below.
// @amalgamate: config-begin ntd-memory NTD_OWN_STDLIB || NTD_EMBEDDED_ALLOC || NTD_EXTERNAL_MALLOC

#include "allocator.h"   // ntd::New / ntd::Delete
#include "type_traits.h" // nullptr_t
#include "utility.h"     // move / forward
#include <new>

namespace ntd
{

// Explicitly marks a raw pointer as originating from ntd::New. A plain T* has
// no allocator provenance, so the default-deleter unique_ptr does not accept
// one implicitly (in particular, unique_ptr<T>(new T) is rejected).
struct adopt_ntd_t { explicit constexpr adopt_ntd_t() = default; };
inline constexpr adopt_ntd_t adopt_ntd{};

// ------------------------------------------------------------------ unique_ptr
template <class T>
struct default_delete
{
    constexpr default_delete() noexcept = default;
    template <class U> default_delete(const default_delete<U>&) noexcept {}
    void operator()(T* p) const { ntd::Delete(p); }
};
template <class T>
struct default_delete<T[]>
{
    default_delete() = delete;
    void operator()(T*) const = delete;
};

template <class T, class D = default_delete<T>>
class unique_ptr
{
public:
    using pointer = T*;

    constexpr unique_ptr() noexcept : m_ptr(nullptr), m_del() {}
    constexpr unique_ptr(nullptr_t) noexcept : m_ptr(nullptr), m_del() {}
    unique_ptr(adopt_ntd_t, pointer p) noexcept : m_ptr(p), m_del() {}
    template <class Q = D, class = enable_if_t<!is_same_v<Q, default_delete<T>>>>
    explicit unique_ptr(pointer p) noexcept : m_ptr(p), m_del() {}
    template <class Q = D, class = enable_if_t<!is_same_v<Q, default_delete<T>>>>
    unique_ptr(pointer p, D d) noexcept : m_ptr(p), m_del(d) {}
    unique_ptr(unique_ptr&& o) noexcept : m_ptr(o.m_ptr), m_del(o.m_del) { o.m_ptr = nullptr; }
    template <class U, class E>
    unique_ptr(unique_ptr<U, E>&& o) noexcept : m_ptr(o.get()), m_del(o.get_deleter())
    {
        // ntd::Delete must receive the original allocation address. A derived-to-base
        // conversion through a secondary or virtual base adjusts the pointer and would
        // later pass an interior address to ntd::mem::Free. Custom deleters own their
        // allocation semantics; enforce this invariant for the ntd default deleter.
        if constexpr (is_same_v<D, default_delete<T>> && is_same_v<E, default_delete<U>>)
        {
            if (static_cast<const void*>(m_ptr) != static_cast<const void*>(o.get()))
                detail::Trap();
        }
        o.release();
    }
    ~unique_ptr()
    {
        if (m_ptr)
            m_del(m_ptr);
    }

    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;

    unique_ptr& operator=(unique_ptr&& o) noexcept
    {
        if (this != &o)
        {
            reset_owned(o.release());
            m_del = o.m_del;
        }
        return *this;
    }
    unique_ptr& operator=(nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    pointer get() const noexcept { return m_ptr; }
    D& get_deleter() noexcept { return m_del; }
    const D& get_deleter() const noexcept { return m_del; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    T& operator*() const { return *m_ptr; }
    pointer operator->() const noexcept { return m_ptr; }

    pointer release() noexcept
    {
        pointer p = m_ptr;
        m_ptr = nullptr;
        return p;
    }
    void reset() noexcept { reset_owned(nullptr); }
    void reset(nullptr_t) noexcept { reset_owned(nullptr); }
    void reset(adopt_ntd_t, pointer p) noexcept { reset_owned(p); }
    template <class Q = D, class = enable_if_t<!is_same_v<Q, default_delete<T>>>>
    void reset(pointer p) noexcept
    {
        reset_owned(p);
    }

private:
    void reset_owned(pointer p) noexcept
    {
        pointer old = m_ptr;
        m_ptr = p;
        if (old)
            m_del(old);
    }

    pointer m_ptr;
    D m_del;
    template <class, class> friend class unique_ptr;
};

// Array ownership is outside ntd's supported subset. Keeping an explicit
// specialization gives an intentional diagnostic instead of allowing the
// primary template to fail indirectly or fall back to global delete[].
template <class T, class D>
class unique_ptr<T[], D>
{
    static_assert(!is_same_v<T, T>,
        "ntd::unique_ptr<T[]> is unsupported; use ntd::vector<T>");
};

template <class T, class... Args>
unique_ptr<T> make_unique(Args&&... args)
{
    return unique_ptr<T>(adopt_ntd, ntd::New<T>(ntd::forward<Args>(args)...));
}

// ------------------------------------------------------------------ shared_ptr
namespace detail
{
struct SpCtrl : mem::RoutedAllocation
{
    long count = 1;
    virtual void dispose() noexcept = 0;
    virtual ~SpCtrl() = default;
    void incref() noexcept
    {
        AtomicIncrementRelaxed(&count);
    }
    long decref() noexcept
    {
        return AtomicDecrementAcquireRelease(&count);
    }
    long use_count() noexcept
    {
        return AtomicLoadRelaxed(&count);
    }
};
#if defined(_MSC_VER)
#pragma warning(push)
// C4324: the alignas storage below intentionally over-aligns (and thus pads)
// this control block so the inline object matches T's alignment.
#pragma warning(disable : 4324)
#endif
template <class T>
struct SpCtrlInline : SpCtrl
{
    alignas(T) unsigned char buf[sizeof(T)];
    template <class... A> explicit SpCtrlInline(A&&... a) { new (&buf) T(ntd::forward<A>(a)...); }
    T* obj() { return reinterpret_cast<T*>(&buf); }
    void dispose() noexcept override { obj()->~T(); }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
} // namespace detail

template <class T> class shared_ptr;
template <class T, class... A> shared_ptr<T> make_shared(A&&... a);

template <class T>
class shared_ptr
{
public:
    constexpr shared_ptr() noexcept : m_ptr(nullptr), m_ctrl(nullptr) {}
    constexpr shared_ptr(nullptr_t) noexcept : m_ptr(nullptr), m_ctrl(nullptr) {}
    template <class U>
    explicit shared_ptr(U*) = delete; // allocator provenance is unknowable; use make_shared
    shared_ptr(const shared_ptr& o) noexcept : m_ptr(o.m_ptr), m_ctrl(o.m_ctrl)
    {
        if (m_ctrl)
            m_ctrl->incref();
    }
    shared_ptr(shared_ptr&& o) noexcept : m_ptr(o.m_ptr), m_ctrl(o.m_ctrl)
    {
        o.m_ptr = nullptr;
        o.m_ctrl = nullptr;
    }
    ~shared_ptr() { release(); }

    shared_ptr& operator=(const shared_ptr& o) noexcept
    {
        if (this != &o)
        {
            release();
            m_ptr = o.m_ptr;
            m_ctrl = o.m_ctrl;
            if (m_ctrl)
                m_ctrl->incref();
        }
        return *this;
    }
    shared_ptr& operator=(shared_ptr&& o) noexcept
    {
        if (this != &o)
        {
            release();
            m_ptr = o.m_ptr;
            m_ctrl = o.m_ctrl;
            o.m_ptr = nullptr;
            o.m_ctrl = nullptr;
        }
        return *this;
    }

    T* get() const noexcept { return m_ptr; }
    T& operator*() const noexcept { return *m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    long use_count() const noexcept { return m_ctrl ? m_ctrl->use_count() : 0; }
    void reset() noexcept
    {
        release();
        m_ptr = nullptr;
        m_ctrl = nullptr;
    }

private:
    struct FromCtrl {};
    shared_ptr(FromCtrl, detail::SpCtrl* c, T* p) noexcept : m_ptr(p), m_ctrl(c) {}
    void release() noexcept
    {
        if (m_ctrl && m_ctrl->decref() == 0)
        {
            m_ctrl->dispose();
            ntd::Delete(m_ctrl);
        }
    }
    T* m_ptr;
    detail::SpCtrl* m_ctrl;
    template <class U, class... A> friend shared_ptr<U> make_shared(A&&... a);
};

template <class T, class... A>
shared_ptr<T> make_shared(A&&... a)
{
    auto* c = ntd::New<detail::SpCtrlInline<T>>(ntd::forward<A>(a)...);
    return shared_ptr<T>(typename shared_ptr<T>::FromCtrl{}, c, c->obj());
}

} // namespace ntd

// @amalgamate: config-end ntd-memory
#endif // NTD_TYPES_DISTINCT_FROM_STD
