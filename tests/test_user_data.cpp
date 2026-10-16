// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/info.h"
#include "test_assert.h"
#include <cstring>
#include <limits>

int main() {
    Shader::Info info;
    std::array<u32, 4> data{};
    info.user_data = data;
    assert(info.UdPointerNull(0));
    assert(info.UdPointerNull(3)); // no second pointer dword
    assert(info.UdPointerNull(std::numeric_limits<u32>::max()));
    assert(!info.UdPointerNull(Shader::IR::NumScalarRegs)); // direct registers
    assert(info.ReadUdReg<u32>(0, 100) == 0);
    data[0] = 0xffff;
    assert(info.UdPointerNull(0));
    data[0] = 0; data[1] = 0xabcd0000; // tags do not make a null pointer readable
    assert(info.UdPointerNull(0));
    std::array<u32, 4> table{11, 22, 33, 44};
    const auto pointer = reinterpret_cast<uintptr_t>(table.data());
    std::memcpy(data.data(), &pointer, sizeof(pointer));
    assert(!info.UdPointerNull(0));
    assert(info.ReadUdReg<u32>(0, 2) == 33);
    // Stage-B snapshots must take precedence over the mutable stage's data.
    const std::array<u32, 2> missing{};
    Shader::Info::ud_snapshots[0] = {&info, missing.data(), 2, nullptr, 0, 0};
    Shader::Info::num_ud_snapshots = 1;
    assert(info.UdPointerNull(0));
    assert(info.ReadUdReg<u32>(0, 0) == 0);
    Shader::Info::num_ud_snapshots = 0;
    assert(info.ReadUdReg<u32>(0, 2) == 33);
    std::puts("PASS: null/incomplete/tagged descriptor tables, valid reads and stage snapshots");
}
