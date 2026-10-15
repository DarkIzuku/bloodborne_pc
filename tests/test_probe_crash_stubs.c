// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only crash preflight uses the real loader/runtime/handler. Graphics and
// networking are unavailable, never silently emulated by this test executable.
#include "gpu/bbgpu.h"
#include "gpu/shim/input/input.h"
#include <stdio.h>
#include <stdlib.h>

static void unavailable(void) {
    fputs("CPU crash fixture cannot initialize graphics or networking\n", stderr);
    abort();
}
void bbgpu_register_kernel(void) {} // Synthetic boot files have no kernel imports.
uintptr_t bbgpu_resolve(const char *name) { (void)name; return 0; }
void bbgpu_start_online(void) { unavailable(); }
int bbgpu_init(const BbGpuConfig *config) { (void)config; unavailable(); return -1; }
int bbgpu_handle_fault(void *context, void *address) {
    (void)context; (void)address; unavailable(); return 0;
}
void bbgpu_dump_guest_writes(void *context) { (void)context; unavailable(); }
int bbgpu_text_input_begin(const char *initial, const char *prompt) {
    (void)initial; (void)prompt; unavailable(); return 0;
}
int bbgpu_text_input_poll(char *output, uint64_t size) {
    (void)output; (void)size; unavailable(); return 0;
}
int bbgpu_overlay_captures_input(void) { unavailable(); return 0; }
void bbgpu_patch_image(unsigned char *image, uint64_t size) {
    (void)image; (void)size; unavailable();
}
unsigned bbgpu_symbol_count(void) { unavailable(); return 0; }
int bbgpu_pc_input(BbPcPad *pad, const bool *keys, int count, int present) {
    (void)pad; (void)keys; (void)count; (void)present; unavailable(); return 0;
}
int vulkan_smoke(void) { unavailable(); return 1; }
