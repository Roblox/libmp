// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "ntd_stdlib/allocator.h"

namespace GPRX
{

class ILockable : public ntd::mem::RoutedAllocation
{
public:
    static const mpsize_t LOCK_LEVEL_MAX = 8; // 30 max, due to DataContextCachingProxy::pin_mask_type size limit

protected:
    ILockable* m_dependent = nullptr;

    mpsize_t m_lock_level = 0;
    bool m_lock_active = false;
    mpsize_t m_lock_ignore_counter = 0;

    virtual void PushDataLockImpl() {}
    virtual void PopDataLockImpl() {}

public:
    virtual ~ILockable() = default;
    ILockable() {}
    ILockable(ILockable* dependent)
        : m_dependent(dependent)
    {}

    inline void SetDependentLockable(ILockable* dependent)
    {
        m_dependent = dependent;
    }

    /**
     * @brief Ensures that the next data request returns a pointer
     * that remains valid until the lock is released.
     */
    inline bool PushDataLock()
    {
        if (m_lock_level >= LOCK_LEVEL_MAX)
            return false;
        if (m_dependent)
            m_dependent->PushDataLock();
        m_lock_level++;
        m_lock_active = true;
        PushDataLockImpl();
        return true;
    }

    /**
     * @brief Releases the most recent lock and invalidates
     * the data pointer associated with it.
     */
    inline bool PopDataLock()
    {
        if (m_lock_level == 0)
            return false;
        PopDataLockImpl();
        m_lock_level--;
        m_lock_active = false;
        if (m_dependent)
            m_dependent->PopDataLock();
        return true;
    }

    inline bool ConsumeDataLock()
    {
        if (!GetActiveLockLevel())
            return false;
        DeactivateLock();
        return true;
    }

    inline bool ResetCurrentLock()
    {
        if (m_lock_level == 0)
            return false;
        PopDataLock();
        PushDataLock();
        return true;
    }

    inline mpsize_t GetActiveLockLevel()
    {
        return (!m_lock_ignore_counter && m_lock_active) ? m_lock_level : 0;
    }

    inline void DeactivateLock()
    {
        if (!m_lock_ignore_counter)
        {
            m_lock_active = false;
            if (m_dependent)
                m_dependent->DeactivateLock();
        }
    }

    inline bool PushIgnoreLock()
    {
        if (m_dependent)
            m_dependent->PushIgnoreLock();
        m_lock_ignore_counter++;
        return true;
    }

    inline bool PopIgnoreLock()
    {
        if (!m_lock_ignore_counter)
            return false;
        m_lock_ignore_counter--;
        if (m_dependent)
            m_dependent->PopIgnoreLock();
        return true;
    }
};

} // GPRX
