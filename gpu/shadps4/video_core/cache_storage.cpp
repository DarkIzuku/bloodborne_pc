// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/cache_file.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <algorithm>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace {

std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};
bool accepting_requests{};
constexpr u64 MaxRecordBytes = 64 * 1024 * 1024;
std::unordered_map<std::string, mz_uint> archive_index;

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    for (;;) {
        std::packaged_task<void()> request;
        {
            std::unique_lock lock{m_request};
            Common::CondvarWait(request_cv, lock, stoken, [&] { return !req_queue.empty(); });
            // Stop only after draining accepted writes. Shader/meta/key ordering is FIFO.
            if (req_queue.empty() && stoken.stop_requested()) {
                break;
            }
            if (req_queue.empty()) {
                continue;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }
        request();
        try {
            request.get_future().get();
        } catch (const std::exception& error) {
            LOG_WARNING(Render, "Pipeline cache write failed: {}", error.what());
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        ar_is_read_only = true;
        archive_index.clear();
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_zero_struct(&zip_ar);
            if (!mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0)) {
                return;
            }
            ar_is_read_only = false;
        } else {
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip_ar); ++i) {
                std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> name{};
                mz_zip_reader_get_filename(&zip_ar, i, name.data(), name.size());
                // Old archives may contain multiple generations of the same name.
                archive_index.insert_or_assign(name.data(), i);
            }
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
    }

    {
        std::scoped_lock lock{m_request};
        accepting_requests = true;
    }
    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!opened.exchange(false)) {
        return;
    }

    {
        std::scoped_lock lock{m_request};
        accepting_requests = false;
    }
    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (ar_is_read_only) {
            mz_zip_reader_end(&zip_ar);
        } else {
            mz_zip_writer_finalize_archive(&zip_ar);
            mz_zip_writer_end(&zip_ar);
        }
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[type, path = std::move(path_),
                                                  v = std::move(v)]() mutable {
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                const auto bytes = std::span{reinterpret_cast<const u8*>(v.data()),
                                             v.size() * sizeof(T)};
                if (!ReplaceCacheFile(path, bytes)) {
                    LOG_WARNING(Render, "Cannot replace pipeline cache record {}", path.string());
                }
            }
        }};
        std::scoped_lock lock{m_request};
        if (!accepting_requests) {
            return false;
        }
        req_queue.emplace(std::move(request));
    }

    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto entry = archive_index.find(path.string());
        if (entry == archive_index.end()) {
            return;
        }
        const auto index = entry->second;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
            stat.m_uncomp_size > MaxRecordBytes || stat.m_uncomp_size % sizeof(T)) {
            return;
        }
        v.resize(stat.m_uncomp_size / sizeof(T));
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0)) {
            v.clear();
        }
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen() || file.GetSize() > MaxRecordBytes || file.GetSize() % sizeof(T)) {
            return;
        }
        v.resize(file.GetSize() / sizeof(T));
        if (file.Read(v) != v.size()) {
            v.clear();
        }
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    for (const auto& name : ListBlobs(type)) {
        std::vector<u8> data;
        Load(type, name, data);
        func(std::move(data));
    }
}

std::vector<std::string> DataBase::ListBlobs(BlobType type) const {
    std::vector<std::string> names;
    const auto ext = "." + GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        for (const auto& [name, index] : archive_index) {
            if (std::filesystem::path(name).extension() == ext) {
                names.push_back(std::filesystem::path(name).stem().string());
            }
        }
    } else {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator{cache_path, error}) {
            if (entry.is_regular_file(error) && entry.path().extension() == ext) {
                names.push_back(entry.path().stem().string());
            }
        }
    }
    std::ranges::sort(names);
    return names;
}

void DataBase::Clear() {
    if (!opened) {
        return;
    }
    // Only called during preload, before any asynchronous writes have been submitted.
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (ar_is_read_only) {
            mz_zip_reader_end(&zip_ar);
        } else {
            mz_zip_writer_end(&zip_ar);
        }
        mz_zip_zero_struct(&zip_ar);
        archive_index.clear();
        ar_is_read_only = false;
        if (!mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0)) {
            LOG_WARNING(Render, "Cannot recreate pipeline cache archive");
            Close();
        }
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {
    if (opened && EmulatorSettings.IsPipelineCacheArchived() && ar_is_read_only) {
        if (!mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str())) {
            LOG_WARNING(Render, "Cannot reopen pipeline cache archive for writing");
            Close();
            return;
        }
        ar_is_read_only = false;
    }
}

} // namespace Storage
