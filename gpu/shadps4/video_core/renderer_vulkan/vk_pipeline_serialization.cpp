// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_set>
#include "common/serdes.h"
#include "common/hash.h"
#include <algorithm>
#include <stdexcept>
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 7u; // bbport: interpolated integer fix (Pascal)
static constexpr u32 MotionShaderBinaryVersion = 8u; // runtime address descriptor, set 1
static constexpr u32 ShaderMetaVersion = 7u; // bbport: ImageResource::needs_native
static constexpr u32 PipelineKeyVersion = 6u; // pointer-free pipeline state + generated fragment
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    meta.Write(vertex_motion ? Serialization::MotionShaderBinaryVersion
                             : Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx, u64 expected_hash) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion &&
        binary_version != Serialization::MotionShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    if (!spec.Deserialize(ar) || !info.Deserialize(ar)) {
        return false;
    }
    if (perm_idx >= 4096 || perm_hash_ar != expected_hash ||
        HashCombine(info.pgm_hash, perm_idx) != expected_hash) {
        return false;
    }

    // Only old motion vertex binaries embed process-local addresses. Other v7 shaders remain
    // reusable; v8 motion binaries load the current addresses from a renderer-owned descriptor.
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    if (binary_version != (vertex_motion ? Serialization::MotionShaderBinaryVersion
                                        : Serialization::ShaderBinaryVersion)) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0, compute_key.value) ||
        sel.infos[0]->hw_stage != Shader::HwStage::Compute) {
        return false;
    }

    if (compute_pipelines.contains(compute_key)) {
        return true;
    }
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    compute_pipelines.emplace(compute_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer data{ar};
    data.Write(vertex_attributes);
    data.Write(vertex_bindings);
    data.Write(divisors);
    // No pNext, pSampleMask, or Vulkan handles belong in a persistent record.
    ASSERT(!multisampling.pNext && !multisampling.pSampleMask);
    data.Write(multisampling.rasterizationSamples);
    data.Write(multisampling.sampleShadingEnable);
    data.Write(multisampling.minSampleShading);
    data.Write(multisampling.alphaToCoverageEnable);
    data.Write(multisampling.alphaToOneEnable);
    data.Write(tcs);
    data.Write(tes);
    data.Write(fragment);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar, bool legacy) {
    Serialization::Reader data{ar};
    if (legacy) {
        // Read old v5 records without ever using persisted pointers. Current writers use v6.
        data.Read(&vertex_attributes, sizeof(vertex_attributes));
        data.Read(&vertex_bindings, sizeof(vertex_bindings));
        data.Read(&divisors, sizeof(divisors));
        if (vertex_attributes.size() > vertex_attributes.capacity() ||
            vertex_bindings.size() > vertex_bindings.capacity() ||
            divisors.size() > divisors.capacity()) {
            throw std::runtime_error("Invalid cached vertex input count");
        }
        data.Read(multisampling);
        if (multisampling.pNext || multisampling.pSampleMask) {
            return false;
        }
    } else {
        data.Read(vertex_attributes);
        data.Read(vertex_bindings);
        data.Read(divisors);
        data.Read(multisampling.rasterizationSamples);
        data.Read(multisampling.sampleShadingEnable);
        data.Read(multisampling.minSampleShading);
        data.Read(multisampling.alphaToCoverageEnable);
        data.Read(multisampling.alphaToOneEnable);
    }
    data.Read(tcs);
    data.Read(tes);
    if (!legacy) {
        data.Read(fragment);
    }
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, bool legacy) {
    sel.graphics_key.Deserialize(ar);
    if (sel.graphics_key.motion_vectors &&
        !instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    if (!sdata.Deserialize(ar, legacy)) {
        return false;
    }
    // v5 did not save the generated clip-discard fragment shader. Only this ambiguous
    // legacy variant needs rebuilding; ordinary v5 pipelines remain usable.
    if (legacy && profile.needs_clip_distance_emulation &&
        !sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)]) {
        return false;
    }

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, hash) ||
            u32(sel.infos[stage_idx]->sw_stage) != stage_idx) {
            return false;
        }
    }

    if (graphics_pipelines.contains(sel.graphics_key)) {
        return true;
    }
    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    graphics_pipelines.emplace(sel.graphics_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 expected_hash) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, fetch, spec, perm_idx, expected_hash)) {
        return false;
    }

    std::vector<u32> spv;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
        fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
    if (spv.size() < 5 || spv[0] != 0x07230203) {
        return false;
    }
    // Reject truncated instructions before passing cached SPIR-V to the driver.
    for (size_t word = 5; word < spv.size();) {
        const u32 count = spv[word] >> 16;
        if (!count || count > spv.size() - word) {
            return false;
        }
        word += count;
    }

    auto it = program_cache.find(program->info.pgm_hash);
    Program* target = it == program_cache.end() ? program.get() : it->second.get();
    vk::ShaderModule module;
    if (perm_idx < target->modules.size() && target->modules[perm_idx].module) {
        // The persisted index is part of the pipeline key. Equal specializations at other
        // indices are not conflicts: equality deliberately ignores some unused bindings.
        if (!(target->modules[perm_idx].spec == spec)) {
            return false;
        }
        module = target->modules[perm_idx].module;
    } else {
        const auto [result, compiled] = instance.GetDevice().createShaderModule(
            {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        module = compiled;
    }
    // Rebind before moving: a temporary Program may have supplied this permutation's Info.
    // Never retain a pointer to that temporary in a cached StageSpecialization.
    spec.info = &target->info;
    sel.runtime_infos[stage] = spec.runtime_info;
    if (fetch) {
        sel.fetch_shader = std::move(fetch);
    }
    target->InsertPermut(module, std::move(spec), perm_idx);
    if (!target->info_template) {
        target->info_template = std::make_unique<Shader::Info>(target->info);
    }
    sel.infos[stage] = &target->info;
    sel.modules[stage] = module;
    if (it == program_cache.end()) {
        const auto hash = program->info.pgm_hash;
        program_cache.emplace(hash, std::move(program));
    }
    return true;
}

void PipelineCache::WarmUp() {
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress = {};
    }
    const auto complete = [] {
        std::scoped_lock lock{progress_mutex};
        precache_progress.rejected = precache_progress.total - precache_progress.loaded;
        precache_progress.complete = true;
    };
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        complete();
        return;
    }

    Storage::DataBase::Instance().Open();
    if (!Storage::DataBase::Instance().IsOpened()) {
        complete();
        return;
    }
    auto& database = Storage::DataBase::Instance();
    const auto names = database.ListBlobs(Storage::BlobType::PipelineKey);
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress.total = static_cast<u32>(names.size());
    }

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        // Without a profile the remaining records have no proven shader-feature compatibility.
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }

    const auto* regs = sel.regs;
    u32 damaged = 0;
    // Sequential by design: selection, programs, SRT walker allocation and archive reads are
    // shared. A worker pool needs isolated reconstruction state before it can be safe.
    for (const auto& name : names) {
        bool loaded = false;
        sel = {.regs = regs}; // Clear partial state after rejection, including runtime/fetch.
        try {
            std::vector<u8> data;
            database.Load(Storage::BlobType::PipelineKey, name, data);
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            if ((version == 5 || version == Serialization::PipelineKeyVersion) && is_compute <= 1) {
                loaded = is_compute ? LoadComputePipeline(ar)
                                    : LoadGraphicsPipeline(ar, version == 5);
            }
        } catch (const std::exception& error) {
            ++damaged;
            LOG_WARNING(Render, "Skipping pipeline {}: {}", name, error.what());
        }
        std::scoped_lock lock{progress_mutex};
        loaded ? ++precache_progress.loaded : ++precache_progress.rejected;
    }
    sel = {.regs = regs};
    if (damaged) {
        // A failed reconstruction may have inserted earlier stages of the damaged record.
        // 0.4 discards those too, while retaining the fork's preload progress reporting.
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VkShaderModule> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) continue;
            for (const auto& permutation : program->modules) {
                if (permutation.module) modules.insert(VkShaderModule(permutation.module));
            }
        }
        for (const auto module : modules) instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
        program_cache.clear();
        database.Clear();
        database.FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        database.Save(Storage::BlobType::ShaderProfile, "profile", std::move(profile_data));
        std::scoped_lock lock{progress_mutex};
        precache_progress.loaded = 0;
    }
    const auto status = GetPrecacheProgress();
    LOG_INFO(Render, "Preloaded {} of {} pipelines ({} rejected)", status.loaded, status.total,
             status.rejected);
    database.FinishPreload();
    complete();
}

void PipelineCache::Sync() {
    std::scoped_lock lock{native_cache_mutex};
    try {
        SaveNativeCache();
        Storage::DataBase::Instance().Close();
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "Pipeline cache shutdown save failed: {}", error.what());
    }
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        walker_func = RegisterWalkerCode(code, walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        if (fetch_data_size != sizeof(Gcn::FetchShaderData)) {
            return false;
        }
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
