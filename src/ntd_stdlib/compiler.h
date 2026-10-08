// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Small compiler-specific primitives shared by the freestanding ntd layer.

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace ntd
{
namespace detail
{

[[noreturn]] inline void Trap() noexcept
{
#if defined(_MSC_VER)
    __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT: non-continuable, allocation-free termination
#else
    __builtin_trap();
#endif
}

// Atomic operations used by ntd's spinlock and shared_ptr. Their names encode
// the minimum ordering required by the caller; MSVC's interlocked operations
// provide the stronger full-barrier semantics.
#if defined(_MSC_VER)
using AtomicFlag = long;
#else
using AtomicFlag = int;
#endif

inline AtomicFlag AtomicExchangeAcquire(AtomicFlag* value, AtomicFlag desired) noexcept
{
#if defined(_MSC_VER)
    return _InterlockedExchange(value, desired);
#else
    return __atomic_exchange_n(value, desired, __ATOMIC_ACQUIRE);
#endif
}

inline void AtomicStoreRelease(AtomicFlag* value, AtomicFlag desired) noexcept
{
#if defined(_MSC_VER)
    _InterlockedExchange(value, desired);
#else
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
#endif
}

inline void SpinPause() noexcept
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
    __yield();
#endif
}

inline void AtomicIncrementRelaxed(long* value) noexcept
{
#if defined(_MSC_VER)
    _InterlockedIncrement(value);
#else
    __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

inline long AtomicDecrementAcquireRelease(long* value) noexcept
{
#if defined(_MSC_VER)
    return _InterlockedDecrement(value);
#else
    return __atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL);
#endif
}

inline long AtomicLoadRelaxed(long* value) noexcept
{
#if defined(_MSC_VER)
    return _InterlockedCompareExchange(value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_RELAXED);
#endif
}

template <class Pointer>
inline Pointer AtomicLoadPointerAcquire(Pointer* address) noexcept
{
#if defined(_MSC_VER)
    return reinterpret_cast<Pointer>(_InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(address), nullptr, nullptr));
#else
    return __atomic_load_n(address, __ATOMIC_ACQUIRE);
#endif
}

template <class Pointer>
inline void AtomicStorePointerRelaxed(Pointer* address, Pointer value) noexcept
{
#if defined(_MSC_VER)
    _InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(address), reinterpret_cast<void*>(value));
#else
    __atomic_store_n(address, value, __ATOMIC_RELAXED);
#endif
}

template <class Pointer>
inline void AtomicStorePointerRelease(Pointer* address, Pointer value) noexcept
{
#if defined(_MSC_VER)
    _InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(address), reinterpret_cast<void*>(value));
#else
    __atomic_store_n(address, value, __ATOMIC_RELEASE);
#endif
}

} // namespace detail
} // namespace ntd
