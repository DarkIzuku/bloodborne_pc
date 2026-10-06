// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <span>
#include "common/io_file.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Storage {
// Replace only after a complete, flushed write. A failed write/rename leaves the old file intact.
// PID + sequence also prevents two renderer processes from sharing a temporary file.
inline bool ReplaceCacheFile(const std::filesystem::path& path, std::span<const u8> bytes) {
    static std::atomic<u64> sequence{};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    auto temporary = path;
    temporary += ".tmp." + std::to_string(pid) + "." + std::to_string(sequence++);
    bool written = false;
    {
        Common::FS::IOFile file{temporary, Common::FS::FileAccessMode::Create};
        written = file.IsOpen() && file.WriteSpan(bytes) == bytes.size() &&
                  file.Flush() && file.Commit();
    }
    std::error_code error;
    if (written) {
#ifdef _WIN32
        if (MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
#else
        std::filesystem::rename(temporary, path, error);
        if (!error) {
            return true;
        }
#endif
    }
    std::filesystem::remove(temporary, error);
    return false;
}
} // namespace Storage
