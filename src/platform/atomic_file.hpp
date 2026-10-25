#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace enrage::platform {
namespace detail {

inline std::filesystem::path atomic_temporary_path(const std::filesystem::path& path) {
    auto temporary = path;
    temporary += ".tmp";
    return temporary;
}

inline void remove_temporary_file(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

#ifdef _WIN32
inline bool write_all(HANDLE handle, std::string_view contents) noexcept {
    const char* cursor = contents.data();
    std::size_t remaining = contents.size();
    while (remaining != 0) {
        constexpr std::size_t max_chunk = static_cast<std::size_t>(0xffffffffu);
        const auto chunk_size = remaining > max_chunk ? max_chunk : remaining;
        DWORD written{};
        if (!WriteFile(
                handle,
                cursor,
                static_cast<DWORD>(chunk_size),
                &written,
                nullptr) ||
            written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}
#else
inline bool write_all(int descriptor, std::string_view contents) noexcept {
    const char* cursor = contents.data();
    std::size_t remaining = contents.size();
    while (remaining != 0) {
        const auto written = ::write(descriptor, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}
#endif

} // namespace detail

// Replaces path without first unlinking the previous destination. The caller
// therefore observes either the old file or the fully prepared new file; a
// preparation/replacement failure leaves the old destination intact.
[[nodiscard]] inline bool atomic_replace_file(
    const std::filesystem::path& path,
    std::string_view contents) {
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    const auto temporary = detail::atomic_temporary_path(path);

#ifdef _WIN32
    HANDLE handle = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    const bool prepared = detail::write_all(handle, contents) && FlushFileBuffers(handle);
    const bool closed = CloseHandle(handle) != FALSE;
    if (!prepared || !closed) {
        detail::remove_temporary_file(temporary);
        return false;
    }

    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        detail::remove_temporary_file(temporary);
        return false;
    }
    return true;
#else
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(temporary.c_str(), flags, 0600);
    if (descriptor < 0) return false;

    const bool prepared = detail::write_all(descriptor, contents) && ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    if (!prepared || !closed) {
        detail::remove_temporary_file(temporary);
        return false;
    }

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        detail::remove_temporary_file(temporary);
        return false;
    }

    // Best effort: the rename is already atomic for process-crash semantics.
    // Syncing the parent directory additionally improves persistence across a
    // sudden machine/power loss on filesystems that support directory fsync.
    if (!parent.empty()) {
        int directory_flags = O_RDONLY;
#ifdef O_CLOEXEC
        directory_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
        directory_flags |= O_DIRECTORY;
#endif
        const int directory = ::open(parent.c_str(), directory_flags);
        if (directory >= 0) {
            (void)::fsync(directory);
            (void)::close(directory);
        }
    }
    return true;
#endif
}

} // namespace enrage::platform
