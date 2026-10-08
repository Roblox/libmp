// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Binary file I/O mirroring a small slice of <fstream> -- ifstream
// (open/read/seek/tell/gcount/good/fail) and ofstream (open/write/good/fail/
// flush), plus the ios open-mode flags and streamoff / streamsize.
//   NTD_OWN_FILEIO : minimal OS-level implementation (POSIX / Win32).
//   else           : aliases the matching std::* types.
// The OS-level implementation requires POSIX or Win32 file APIs.

#include "ntd_config.h"

#if !NTD_OWN_FILEIO // ----------------------------------------- std passthrough

#include <fstream>
namespace ntd
{
using std::fstream;
using std::ifstream;
using std::ios;
using std::ofstream;
using std::streamoff;
using std::streamsize;
} // namespace ntd

#else // ----------------------------------------------- OS-level implementation
// @amalgamate: config-begin NTD_OWN_FILEIO

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
    #include "windows_api.h"
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <limits.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace ntd
{

using streamoff = long long;
using streamsize = long long;

// Open-mode / seek-dir flags mirroring the std::ios values used here.
struct ios
{
    static constexpr int in = 1;
    static constexpr int out = 2;
    static constexpr int binary = 4; // no-op on POSIX/Win32 binary handles
    static constexpr int ate = 8;
    static constexpr int trunc = 16;
    static constexpr int beg = 0;
    static constexpr int cur = 1;
    static constexpr int end = 2;
};

namespace detail
{
#if defined(_WIN32)
using FileHandle = HANDLE;

inline FileHandle InvalidFileHandle() { return INVALID_HANDLE_VALUE; }
inline bool IsFileOpen(FileHandle handle) { return handle != INVALID_HANDLE_VALUE; }

inline FileHandle OpenFileRead(const char* path)
{
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline FileHandle OpenFileWrite(const char* path)
{
    return CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline FileHandle OpenFileReadWrite(const char* path, bool truncate)
{
    return CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

inline bool CloseFile(FileHandle handle)
{
    return CloseHandle(handle) != 0;
}

inline bool TellFile(FileHandle handle, streamoff& offset)
{
    LARGE_INTEGER zero{};
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(handle, zero, &position, FILE_CURRENT))
        return false;
    offset = static_cast<streamoff>(position.QuadPart);
    return true;
}

inline bool SeekFile(FileHandle handle, streamoff offset, int direction)
{
    LARGE_INTEGER distance{};
    distance.QuadPart = offset;
    const DWORD method = static_cast<DWORD>(
        direction == ios::cur ? FILE_CURRENT : direction == ios::end ? FILE_END : FILE_BEGIN);
    return SetFilePointerEx(handle, distance, nullptr, method) != 0;
}

inline streamsize ReadFileChunk(FileHandle handle, char* buffer, streamsize remaining)
{
    const DWORD request = remaining > static_cast<streamsize>(MAXDWORD)
        ? MAXDWORD : static_cast<DWORD>(remaining);
    DWORD transferred = 0;
    return ReadFile(handle, buffer, request, &transferred, nullptr)
        ? static_cast<streamsize>(transferred) : -1;
}

inline streamsize WriteFileChunk(FileHandle handle, const char* buffer, streamsize remaining)
{
    const DWORD request = remaining > static_cast<streamsize>(MAXDWORD)
        ? MAXDWORD : static_cast<DWORD>(remaining);
    DWORD transferred = 0;
    return WriteFile(handle, buffer, request, &transferred, nullptr)
        ? static_cast<streamsize>(transferred) : -1;
}
#else
using FileHandle = int;

inline FileHandle InvalidFileHandle() { return -1; }
inline bool IsFileOpen(FileHandle handle) { return handle >= 0; }

inline FileHandle OpenFileRead(const char* path)
{
    return ::open(path, O_RDONLY);
}

inline FileHandle OpenFileWrite(const char* path)
{
    return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

inline FileHandle OpenFileReadWrite(const char* path, bool truncate)
{
    return ::open(path, O_RDWR | O_CREAT | (truncate ? O_TRUNC : 0), 0644);
}

inline bool CloseFile(FileHandle handle)
{
    return ::close(handle) == 0;
}

inline bool TellFile(FileHandle handle, streamoff& offset)
{
    const off_t position = ::lseek(handle, 0, SEEK_CUR);
    if (position == static_cast<off_t>(-1))
        return false;
    offset = static_cast<streamoff>(position);
    return static_cast<off_t>(offset) == position;
}

inline bool SeekFile(FileHandle handle, streamoff offset, int direction)
{
    const int whence = direction == ios::cur ? SEEK_CUR : direction == ios::end ? SEEK_END : SEEK_SET;
    const off_t nativeOffset = static_cast<off_t>(offset);
    return static_cast<streamoff>(nativeOffset) == offset &&
        ::lseek(handle, nativeOffset, whence) != static_cast<off_t>(-1);
}

inline streamsize ReadFileChunk(FileHandle handle, char* buffer, streamsize remaining)
{
    const size_t request = remaining > static_cast<streamsize>(SSIZE_MAX)
        ? static_cast<size_t>(SSIZE_MAX) : static_cast<size_t>(remaining);
    ssize_t transferred;
    do
    {
        transferred = ::read(handle, buffer, request);
    } while (transferred < 0 && errno == EINTR);
    return static_cast<streamsize>(transferred);
}

inline streamsize WriteFileChunk(FileHandle handle, const char* buffer, streamsize remaining)
{
    const size_t request = remaining > static_cast<streamsize>(SSIZE_MAX)
        ? static_cast<size_t>(SSIZE_MAX) : static_cast<size_t>(remaining);
    ssize_t transferred;
    do
    {
        transferred = ::write(handle, buffer, request);
    } while (transferred < 0 && errno == EINTR);
    return static_cast<streamsize>(transferred);
}
#endif
} // namespace detail

class ifstream
{
public:
    ifstream() = default;
    ~ifstream() { close(); }
    ifstream(const ifstream&) = delete;
    ifstream& operator=(const ifstream&) = delete;

    void open(const char* path, int mode = ios::in | ios::binary)
    {
        close();
        m_fail = false;
        m_gcount = 0;
        m_handle = detail::OpenFileRead(path);
        if (!is_open())
        {
            m_fail = true;
            return;
        }
        if (mode & ios::ate)
        {
            seekg(0, ios::end);
            if (m_fail) close();
        }
    }

    bool is_open() const
    {
        return detail::IsFileOpen(m_handle);
    }

    void close()
    {
        if (is_open())
        {
            if (!detail::CloseFile(m_handle))
                m_fail = true;
            m_handle = detail::InvalidFileHandle();
        }
    }

    streamoff tellg()
    {
        if (!is_open()) { m_fail = true; return -1; }
        streamoff position = 0;
        if (!detail::TellFile(m_handle, position))
        {
            m_fail = true;
            return -1;
        }
        return position;
    }

    void seekg(streamoff off, int dir = ios::beg)
    {
        if (!is_open()) { m_fail = true; return; }
        if (!detail::SeekFile(m_handle, off, dir))
            m_fail = true;
    }

    void clear() { m_fail = false; }

    ifstream& read(char* buf, streamsize n)
    {
        m_gcount = 0;
        if (!is_open() || n < 0 || (!buf && n != 0)) { m_fail = true; return *this; }
        while (m_gcount < n)
        {
            const streamsize transferred = detail::ReadFileChunk(
                m_handle, buf + m_gcount, n - m_gcount);
            if (transferred <= 0)
            {
                m_fail = true; // error, or EOF before the requested count
                break;
            }
            m_gcount += transferred;
        }
        return *this;
    }

    streamsize gcount() const { return m_gcount; }
    bool good() const { return is_open() && !m_fail; }
    bool fail() const { return m_fail; }

private:
    detail::FileHandle m_handle = detail::InvalidFileHandle();
    streamsize m_gcount = 0;
    bool m_fail = false;
};

class ofstream
{
public:
    ofstream() = default;
    ~ofstream() { close(); }
    ofstream(const ofstream&) = delete;
    ofstream& operator=(const ofstream&) = delete;

    void open(const char* path, int /*mode*/ = ios::out | ios::binary | ios::trunc)
    {
        close();
        m_fail = false;
        m_handle = detail::OpenFileWrite(path);
        if (!is_open())
            m_fail = true;
    }

    bool is_open() const
    {
        return detail::IsFileOpen(m_handle);
    }

    void close()
    {
        if (is_open())
        {
            if (!detail::CloseFile(m_handle))
                m_fail = true;
            m_handle = detail::InvalidFileHandle();
        }
    }

    ofstream& write(const char* buf, streamsize n)
    {
        if (!is_open() || n < 0 || (!buf && n != 0)) { m_fail = true; return *this; }
        streamsize total = 0;
        while (total < n)
        {
            const streamsize transferred = detail::WriteFileChunk(
                m_handle, buf + total, n - total);
            if (transferred <= 0)
            {
                m_fail = true;
                break;
            }
            total += transferred;
        }
        return *this;
    }

    // Data is handed straight to the OS by write(); there is no user buffer to
    // flush, so this only reports status.
    ofstream& flush() { return *this; }
    bool good() const { return is_open() && !m_fail; }
    bool fail() const { return m_fail; }

private:
    detail::FileHandle m_handle = detail::InvalidFileHandle();
    bool m_fail = false;
};

class fstream
{
public:
    fstream() = default;
    ~fstream() { close(); }
    fstream(const fstream&) = delete;
    fstream& operator=(const fstream&) = delete;

    fstream(fstream&& other) noexcept
    {
        moveFrom(other);
    }

    fstream& operator=(fstream&& other) noexcept
    {
        if (this != &other)
        {
            close();
            moveFrom(other);
        }
        return *this;
    }

    void open(const char* path, int mode = ios::in | ios::out | ios::binary)
    {
        close();
        m_fail = false;
        m_gcount = 0;
        m_handle = mode & ios::out
            ? detail::OpenFileReadWrite(path, mode & ios::trunc)
            : detail::OpenFileRead(path);
        if (!is_open())
        {
            m_fail = true;
            return;
        }
        if (mode & ios::ate)
        {
            seekg(0, ios::end);
            if (m_fail)
                close();
        }
    }

    bool is_open() const
    {
        return detail::IsFileOpen(m_handle);
    }

    void close()
    {
        if (is_open())
        {
            if (!detail::CloseFile(m_handle))
                m_fail = true;
            m_handle = detail::InvalidFileHandle();
        }
    }

    streamoff tellg()
    {
        if (!is_open())
        {
            m_fail = true;
            return -1;
        }
        streamoff position = 0;
        if (!detail::TellFile(m_handle, position))
        {
            m_fail = true;
            return -1;
        }
        return position;
    }

    void seekg(streamoff off, int dir = ios::beg)
    {
        if (!is_open() || !detail::SeekFile(m_handle, off, dir))
            m_fail = true;
    }

    void clear()
    {
        m_fail = false;
    }

    fstream& read(char* buf, streamsize n)
    {
        m_gcount = 0;
        if (!is_open() || n < 0 || (!buf && n != 0))
        {
            m_fail = true;
            return *this;
        }
        while (m_gcount < n)
        {
            const streamsize transferred = detail::ReadFileChunk(m_handle, buf + m_gcount, n - m_gcount);
            if (transferred <= 0)
            {
                m_fail = true;
                break;
            }
            m_gcount += transferred;
        }
        return *this;
    }

    fstream& write(const char* buf, streamsize n)
    {
        if (!is_open() || n < 0 || (!buf && n != 0))
        {
            m_fail = true;
            return *this;
        }
        streamsize total = 0;
        while (total < n)
        {
            const streamsize transferred = detail::WriteFileChunk(m_handle, buf + total, n - total);
            if (transferred <= 0)
            {
                m_fail = true;
                break;
            }
            total += transferred;
        }
        return *this;
    }

    fstream& flush() { return *this; }
    streamsize gcount() const { return m_gcount; }
    bool good() const { return is_open() && !m_fail; }
    bool fail() const { return m_fail; }

private:
    void moveFrom(fstream& other)
    {
        m_handle = other.m_handle;
        m_gcount = other.m_gcount;
        m_fail = other.m_fail;
        other.m_handle = detail::InvalidFileHandle();
        other.m_gcount = 0;
        other.m_fail = false;
    }

    detail::FileHandle m_handle = detail::InvalidFileHandle();
    streamsize m_gcount = 0;
    bool m_fail = false;
};

} // namespace ntd

// @amalgamate: config-end NTD_OWN_FILEIO
#endif // NTD_OWN_FILEIO
