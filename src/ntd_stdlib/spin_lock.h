// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Allocation-free lock for short, low-contention critical sections.

#include "compiler.h"

namespace ntd
{
namespace detail
{

class SpinLock
{
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void Lock() noexcept
    {
        while (AtomicExchangeAcquire(&m_flag, 1) != 0)
            SpinPause();
    }

    void Unlock() noexcept
    {
        AtomicStoreRelease(&m_flag, 0);
    }

private:
    AtomicFlag m_flag = 0;
};

class SpinLockGuard
{
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept
        : m_lock(lock)
    {
        m_lock.Lock();
    }

    ~SpinLockGuard()
    {
        m_lock.Unlock();
    }

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& m_lock;
};

} // namespace detail
} // namespace ntd
