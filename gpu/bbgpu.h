/* C interface of the GPU library (gpu/): the command processor and Vulkan renderer,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * bbport.ini keeps the window from taking it. */
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbgpu_mouse_captured(void);
/* Asks the window to take the mouse (relative mode, as a click in the game does); the window
 * thread does it. */
void bbgpu_mouse_capture(void);
void bbgpu_mouse_look_enable(int enabled);
/* A module's look at the window's events first (an SDL_Event and the SDL_Window, on the window
 * thread): nonzero takes the event (the port and the game do not see it). Up to 4. */
typedef int (*BbGpuEventFilter)(const void *sdl_event, void *sdl_window);
void bbgpu_add_event_filter(BbGpuEventFilter filter);
/* A module's menu pointer: nonzero while the game shows a menu the mouse points into. The window
 * then lets the mouse go and shows the cursor, and takes the mouse back after (as it had it).
 * Up to 4; pointer_wanted: any of them says so. */
typedef int (*BbGpuPointerQuery)(void);
void bbgpu_add_pointer_query(BbGpuPointerQuery query);
int bbgpu_pointer_wanted(void);
/* A module's changes to the pad state the game reads (runtime_pad.c, after the host's input and
 * the scripted one): the buttons (PS4 bits); the sticks (lx ly rx ry) as they are. Up to 4. */
typedef void (*BbGpuPadFilter)(uint32_t *buttons, const uint8_t *sticks);
void bbgpu_add_pad_filter(BbGpuPadFilter filter);
void bbgpu_filter_pad(uint32_t *buttons, const uint8_t *sticks);
/* The shown frame's width / height (the display buffer's; the window letterboxes it). */
float bbgpu_frame_aspect(void);
/* A line of text over the game, near the bottom, until called with NULL (any thread). */
void bbgpu_overlay_banner(const char *utf8);
/* Native mouse look: turns the game's camera (radians, applied at its next update); 0 when the
 * camera hook is not in (another game version, BB_NATIVE_MOUSE=0) or the camera does not take the
 * turns now (lock-on, menus): drive the right stick then. */
int bbgpu_camera_look(float yaw, float pitch);
/* 1 while the free camera (the community patch) is on: the mouse drives it through the right stick. */
int bbgpu_free_camera(void);
/* The game's current camera state (its projection parameters at +0x50), 0 until known. */
uint64_t bbgpu_camera_state(void);
/* Shows the trophy banner over the game for a few seconds, with its chime
 * (UTF-8 name; grade 1 bronze, 2 silver, 3 gold, 4 platinum). */
void bbgpu_trophy_popup(const char *name_utf8, int grade);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
