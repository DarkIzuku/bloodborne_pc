// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual SPIR-V backend without opening a window or loading the game.
// Output is validated by spirv-val (see docs/upscaler.md).
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <setjmp.h>
#include <sirit/sirit.h>
#include <unordered_map>
#include <unordered_set>
#include "common/hash.h"
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

static void CheckMetadata() {
    using namespace Shader;
    const auto load = [](bool motion, u32 version) {
        Info original{};
        original.hw_stage = HwStage::Vertex;
        original.sw_stage = SwStage::Vertex;
        original.pgm_hash = 0x123456;
        StageSpecialization spec{};
        spec.info = &original;
        spec.runtime_info.Initialize(original.hw_stage, original.sw_stage);
        spec.runtime_info.hw.vs.motion_vectors = motion;
        const auto hash = HashCombine(original.pgm_hash, u64{2});
        Serialization::Archive output;
        Serialization::Writer writer{output};
        writer.Write(u32{7}); // Unchanged metadata schema.
        writer.Write(version);
        writer.Write(hash);
        writer.Write(size_t{2});
        spec.Serialize(output);
        original.Serialize(output);
        Serialization::Archive input{output.TakeOff()};
        Info restored{};
        StageSpecialization restored_spec{};
        restored_spec.info = &restored;
        std::optional<Gcn::FetchShaderData> fetch;
        size_t index{};
        const bool loaded = Vulkan::LoadShaderMeta(input, restored, fetch, restored_spec, index, hash);
        if (loaded) {
            assert(index == 2 && restored.pgm_hash == original.pgm_hash);
            assert(restored_spec.runtime_info == spec.runtime_info);
            assert(restored_spec.info == &restored);
        }
        return loaded;
    };
    assert(!load(true, 7)); // Only baked-address motion binaries must be invalidated.
    assert(load(true, 8));  // Restores without ObjectMotion or any Vulkan allocation.
    assert(load(false, 7));
    assert(!load(false, 8));
    assert(!load(true, 99));
}

static void CheckMotionInterface(const std::vector<u32>& code, bool vertex_motion) {
    using namespace Shader;
    std::unordered_map<u32, u32> sets, bindings, storage;
    std::unordered_set<u32> int64_types;
    unsigned atomic_stores = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        assert(count && count <= code.size() - i);
        const auto op = static_cast<spv::Op>(code[i] & 0xffff);
        if (op == spv::Op::OpTypeInt && code[i + 2] == 64) {
            int64_types.insert(code[i + 1]);
        }
        if (op == spv::Op::OpDecorate && count == 4) {
            if (code[i + 2] == u32(spv::Decoration::DescriptorSet)) {
                sets[code[i + 1]] = code[i + 3];
            }
            if (code[i + 2] == u32(spv::Decoration::Binding)) {
                bindings[code[i + 1]] = code[i + 3];
            }
        }
        if (op == spv::Op::OpVariable) {
            storage[code[i + 2]] = code[i + 3];
        }
        if (op == spv::Op::OpConstant && int64_types.contains(code[i + 1])) {
            // This minimal shader needs only small offsets/strides, never allocation addresses.
            assert(count == 5 && code[i + 4] == 0 && code[i + 3] <= 32);
        }
        atomic_stores += op == spv::Op::OpAtomicExchange;
        i += count;
    }
    unsigned motion_descriptors = 0;
    for (const auto& [id, set] : sets) {
        if (set == MotionVectors::DescriptorSet) {
            ++motion_descriptors;
            assert(bindings.at(id) == MotionVectors::AddressBinding);
            assert(storage.at(id) == u32(spv::StorageClass::Uniform));
        }
    }
    assert(motion_descriptors == (vertex_motion ? 1u : 0u));
    assert(atomic_stores == (vertex_motion ? 4u : 0u));
    static_assert(sizeof(PushData) == 128);
}

int main(int argc, char** argv) {
    using namespace Shader;
    CheckMetadata();
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    for (bool vertex : {true, false}) {
        for (bool motion : {false, true}) {
            Info info{};
            info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
            info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            if (vertex) {
                runtime.hw.vs.motion_vectors = motion;
            } else {
                runtime.hw.fs.motion_vectors = motion;
                runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
            }
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            IR::IREmitter ir(block);
            const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
            for (u32 component = 0; component < 4; ++component) {
                info.stores.Set(output, component);
                if (!vertex && component < 2) {
                    info.loads.Set(IR::Attribute::FragCoord, component);
                    ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
                } else {
                    ir.SetAttribute(output, ir.Imm32(component == 3 ? 1.0f : 0.0f), component);
                }
            }
            ir.Epilogue();
            IR::Program program(info);
            program.blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block},
                                           .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            Backend::Bindings bindings{};
            const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            CheckMotionInterface(code, vertex && motion);
            const auto path = dir / (std::string(vertex ? "vertex" : "fragment") +
                                      (motion ? "-motion.spv" : "-plain.spv"));
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            if (!out) { return 1; }
        }
    }
}
