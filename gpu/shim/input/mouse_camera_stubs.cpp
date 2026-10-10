// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost db5457cbd447f4ef129b58df0a78e0a8819c7033.
// The stubs the mouse camera's hooks put in the game's follow camera
// (engine/mouse_camera.cpp places them; engine/mouse_camera.h): at the free
// camera's store, 0x183ce54, and at the fast turn's two exits. A file of their
// own so that a test can place them in the game's code and drive the same
// step (tests/decomp/follow_camera_test.cpp).
#include "mouse_camera_stubs.h"

extern "C" {
void* g_mouse_camera_host = nullptr;
std::uint64_t g_mouse_camera_resume = 0;
std::uint8_t bb_mouse_camera_free_now = 0;
}

// The two stores the hook displaced, then the host with every register the
// game's code could still need around it - the SysV ones a call may clobber,
// and all sixteen xmm (the thunk uses xmm8-15 as scratch; the camera keeps its
// stick vector in xmm10) - and the new pitch back in xmm0, which the next
// block stores as the return-to-centre reference. The update is not a leaf,
// so nothing of its lives below rsp to be pushed over.
asm(R"(
    .text
    .globl bb_mouse_camera_stub
bb_mouse_camera_stub:
    vmovss %xmm0, 0x140(%r13)
    vextractps $1, %xmm1, 0x144(%r13)
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %rbx
    mov %rsp, %rbx
    and $-16, %rsp
    sub $256, %rsp
    vmovdqu %xmm0, 0(%rsp)
    vmovdqu %xmm1, 16(%rsp)
    vmovdqu %xmm2, 32(%rsp)
    vmovdqu %xmm3, 48(%rsp)
    vmovdqu %xmm4, 64(%rsp)
    vmovdqu %xmm5, 80(%rsp)
    vmovdqu %xmm6, 96(%rsp)
    vmovdqu %xmm7, 112(%rsp)
    vmovdqu %xmm8, 128(%rsp)
    vmovdqu %xmm9, 144(%rsp)
    vmovdqu %xmm10, 160(%rsp)
    vmovdqu %xmm11, 176(%rsp)
    vmovdqu %xmm12, 192(%rsp)
    vmovdqu %xmm13, 208(%rsp)
    vmovdqu %xmm14, 224(%rsp)
    vmovdqu %xmm15, 240(%rsp)
    mov %r13, %rdi
    lea -0x3c4(%rbp), %rsi
    vmovaps %xmm10, %xmm0
    vmovshdup %xmm10, %xmm1
    mov g_mouse_camera_host(%rip), %rax
    call *%rax
    vmovdqu 0(%rsp), %xmm0
    vmovdqu 16(%rsp), %xmm1
    vmovdqu 32(%rsp), %xmm2
    vmovdqu 48(%rsp), %xmm3
    vmovdqu 64(%rsp), %xmm4
    vmovdqu 80(%rsp), %xmm5
    vmovdqu 96(%rsp), %xmm6
    vmovdqu 112(%rsp), %xmm7
    vmovdqu 128(%rsp), %xmm8
    vmovdqu 144(%rsp), %xmm9
    vmovdqu 160(%rsp), %xmm10
    vmovdqu 176(%rsp), %xmm11
    vmovdqu 192(%rsp), %xmm12
    vmovdqu 208(%rsp), %xmm13
    vmovdqu 224(%rsp), %xmm14
    vmovdqu 240(%rsp), %xmm15
    mov %rbx, %rsp
    pop %rbx
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
    vmovss 0x140(%r13), %xmm0
    jmp *g_mouse_camera_resume(%rip)

# The stick's fast turn - a stick held over, ramping up to the high speed -
# is the free camera too, with its own two stores of pitch and a jump to
# 0x183ce67. The stick has the camera there, so the mouse waits, as in DS3;
# the stub only notes that the free camera ran.
    .globl bb_mouse_camera_ramp_stub
bb_mouse_camera_ramp_stub:
    vmovss %xmm0, 0x140(%r13)
    movb $1, bb_mouse_camera_free_now(%rip)
    jmp *g_mouse_camera_resume(%rip)
)");
