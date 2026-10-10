// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost engine/menu_memory.cpp: room for the native option pages.
#include "menu_memory.h"
#include <cstdio>
#include <cstring>
namespace BbEngine::MenuMemory {
namespace {
constexpr std::size_t block=0x1f5a465,page=0x1f5a479,menu=0x4736d40+2*0x80+9*8;
constexpr std::uint32_t stock_block=18<<20,stock_page=34<<20;
constexpr std::uint64_t stock_menu=82<<20;
}
bool Compatible(const std::uint8_t* image,std::size_t size) {
    if (size<block+5 || size<page+5 || size<menu+8) return false;
    std::uint32_t b=0,p=0; std::uint64_t m=0;
    std::memcpy(&b,image+block+1,4); std::memcpy(&p,image+page+1,4); std::memcpy(&m,image+menu,8);
    return image[block]==0xba && image[page]==0xba && b==stock_block && p==stock_page && m==stock_menu;
}
bool Install(std::uint8_t* image,std::size_t size) {
    // The loader still owns an RW image. Check all sites before touching any.
    if (!Compatible(image,size)) return false;
    const std::uint32_t b=stock_block+(32<<20),p=stock_page+(64<<20);
    const std::uint64_t m=stock_menu+(96<<20);
    std::memcpy(image+block+1,&b,4); std::memcpy(image+page+1,&p,4); std::memcpy(image+menu,&m,8);
    std::puts("Engine: Scaleform menu heaps 50/98 MiB, MENU 178 MiB (bbhost native option pages)");
    return true;
}
}
