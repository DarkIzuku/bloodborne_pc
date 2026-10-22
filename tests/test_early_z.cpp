// SPDX-License-Identifier: GPL-2.0-or-later
// Inspect actual emitted SPIR-V and specialization round trips, not a duplicate policy.
#include "test_assert.h"
#include <sirit/sirit.h>
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int main(int argc,char** argv) {
    using namespace Shader;
    const std::filesystem::path dir=argc>1 ? argv[1] : "early-z-shaders";
    std::filesystem::create_directories(dir);
    unsigned cases=0;
    for(bool requested:{false,true}) for(bool clips:{false,true})
    for(bool depth:{false,true}) for(bool stencil:{false,true})
    for(bool motion:{false,true}) for(bool discard:{false,true}) {
        Info info{};
        info.hw_stage=HwStage::Fragment;info.sw_stage=SwStage::Fragment;
        RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);
        auto& fs=runtime.hw.fs;
        fs.depth_before_shader=requested;fs.clip_distance_emulation=clips;
        fs.num_inputs=clips ? 1 : 0; // The lowered clip-distance vector is the last input.
        fs.motion_vectors=motion;fs.color_buffers[0].num_format=AmdGpu::NumberFormat::Float;
        Info other_info{};
        StageSpecialization spec{};spec.info=&info;spec.runtime_info=runtime;
        Serialization::Archive ar;spec.Serialize(ar);
        Serialization::Archive input{ar.TakeOff()};
        StageSpecialization restored{};restored.info=&other_info;
        assert(restored.Deserialize(input));assert(restored.runtime_info==runtime);
        auto flipped=runtime;flipped.hw.fs.depth_before_shader=!requested;
        assert(flipped!=runtime); // Module specialization must distinguish both variants.

        Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
        for(u32 c=0;c<4;++c) {
            info.stores.Set(IR::Attribute::RenderTarget0,c);
            ir.SetAttribute(IR::Attribute::RenderTarget0,ir.Imm32(c==3?1.f:0.5f),c);
        }
        if(depth) info.stores.Set(IR::Attribute::Depth,0);
        if(stencil) info.stores.Set(IR::Attribute::StencilRef,0);
        info.has_discard=discard;
        if(discard) ir.Discard();
        ir.Epilogue();
        IR::Program program(info);program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
        Profile profile{};profile.supported_spirv=0x00010600;profile.support_int64=true;
        profile.supports_shader_stencil_export=true;
        Backend::Bindings bindings{};
        const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
        unsigned early=0;
        for(size_t i=5;i<code.size();) {
            const auto words=code[i]>>16;assert(words&&words<=code.size()-i);
            if(spv::Op(code[i]&0xffff)==spv::Op::OpExecutionMode&&
               code[i+2]==u32(spv::ExecutionMode::EarlyFragmentTests)) ++early;
            i+=words;
        }
        assert(early==unsigned(requested&&!clips&&!depth&&!stencil));
        const auto path=dir/("case-"+std::to_string(cases++)+".spv");
        std::ofstream output(path,std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32));
        assert(output.good());
    }
    std::printf("PASS: %u depth-ordering shader variants and specialization round trips\n",cases);
}
