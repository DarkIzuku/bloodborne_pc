// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/menu_memory.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
int main() {
    constexpr std::size_t b=0x1f5a465,p=0x1f5a479,m=0x4736d40+2*0x80+9*8;
    std::vector<std::uint8_t> image(m+8,0);
    const std::uint32_t block=18<<20,page=34<<20;
    const std::uint64_t menu=82<<20;
    image[b]=image[p]=0xba;
    std::memcpy(image.data()+b+1,&block,4);
    std::memcpy(image.data()+p+1,&page,4);
    std::memcpy(image.data()+m,&menu,8);
    assert(!BbEngine::MenuMemory::Install(image.data(),image.size()-1));
    image[p]=0x90; // A competing patch: reject the whole change, not only this heap.
    assert(!BbEngine::MenuMemory::Install(image.data(),image.size()));
    assert(!std::memcmp(image.data()+b+1,&block,4) && !std::memcmp(image.data()+m,&menu,8));
    image[p]=0xba;
    assert(BbEngine::MenuMemory::Install(image.data(),image.size()));
    std::uint32_t result=0; std::uint64_t total=0;
    std::memcpy(&result,image.data()+b+1,4); assert(result==(50u<<20));
    std::memcpy(&result,image.data()+p+1,4); assert(result==(98u<<20));
    std::memcpy(&total,image.data()+m,8); assert(total==(178ull<<20));
    assert(!BbEngine::MenuMemory::Install(image.data(),image.size())); // no double enlargement
    std::puts("PASS: native menu heaps: bounds, competing patch, coherent budget, no double apply");
}
