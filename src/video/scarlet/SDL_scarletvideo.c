/* Experimental Scarlet Native SWS backend. SDL's zlib license applies.
 * Uses the native object/socket transport, including shared-memory BGRA frames.
 * Input codes follow the SWS protocol's evdev vocabulary; no Linux ABI calls. */
#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "../../events/scancodes_linux.h"
#include <sws_client.h>
#include "SDL_scarlet.h"
#include "SDL_timer.h"

typedef struct {
    SwsBuffer identity;
    Uint32 width, height;
    Uint64 serial;
    SDL_bool attached, pending, failed;
} ScarletGpuImage;
typedef struct {
    Uint32 id;
    SDL_Surface *frame;
    int mouse_x, mouse_y;
    SDL_bool mouse_pending;
    ScarletGpuImage gpu[2];
    Uint32 gpu_generation;
    SDL_bool retired;
} ScarletWindow;

static int SWS_Error(const char *operation, int result)
{
    return SDL_SetError("Scarlet SWS %s failed: %d", operation, result);
}
static ScarletWindow *Scarlet_GpuWindow(SDL_Window *window)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    SDL_Window *candidate;
    const char *driver = SDL_GetCurrentVideoDriver();
    if (device && driver && SDL_strcmp(driver, "scarlet") == 0) {
        for (candidate = device->windows; candidate; candidate = candidate->next)
            if (candidate == window && window->driverdata) {
                ScarletWindow *data = window->driverdata;
                if (data->retired) { SDL_SetError("Scarlet window retired"); return NULL; }
                return data;
            }
    }
    SDL_SetError("Invalid native Scarlet window");
    return NULL;
}
static ScarletGpuImage *Scarlet_GpuSlot(ScarletWindow *data, Uint32 slot)
{
    if (slot >= 2) { SDL_SetError("Scarlet GPU slot must be 0 or 1"); return NULL; }
    return &data->gpu[slot];
}
int SDL_ScarletAttachGpuImage(SDL_Window *window, Uint32 slot, int handle, Uint32 width, Uint32 height)
{
    ScarletWindow *data = Scarlet_GpuWindow(window);
    ScarletGpuImage *image;
    SwsDisplay display;
    int result;
    if (!data || !(image = Scarlet_GpuSlot(data, slot))) return -1;
    if (image->attached) return SDL_SetError("Scarlet GPU image already attached");
    if (handle < 0 || !width || !height || width != (Uint32)window->w || height != (Uint32)window->h)
        return SDL_SetError("Invalid Scarlet GPU image handle or window size");
    result = sws_get_display(&display);
    if (result < 0) return SWS_Error("GPU display", result);
    if (display.compositor_backend != 1 || !(display.capabilities & 1))
        return SDL_SetError("Scarlet compositor has no SGFX shared-image support");
    if (!data->gpu[0].attached && !data->gpu[1].attached) {
        if (data->gpu_generation == SDL_MAX_UINT32) return SDL_SetError("Scarlet GPU generation exhausted");
        ++data->gpu_generation;
    }
    image->identity.window_id = data->id;
    image->identity.buffer_id = slot + 1;
    image->identity.generation = data->gpu_generation;
    image->identity.compositor_epoch = display.compositor_epoch;
    result = sws_gpu_register(image->identity, width, height, handle);
    if (result < 0) return SWS_Error("GPU register", result);
    image->width = width;
    image->height = height;
    image->attached = SDL_TRUE;
    image->pending = image->failed = SDL_FALSE;
    return 0;
}
int SDL_ScarletPresentGpuImage(SDL_Window *window, Uint32 slot)
{
    ScarletWindow *data = Scarlet_GpuWindow(window);
    ScarletGpuImage *image;
    int result;
    if (!data || !(image = Scarlet_GpuSlot(data, slot))) return -1;
    if (!image->attached || image->pending || image->failed)
        return SDL_SetError("Scarlet GPU image unavailable or awaiting release");
    if (image->width != (Uint32)window->w || image->height != (Uint32)window->h)
        return SDL_SetError("Scarlet GPU image size changed; recreate window");
    if (image->serial == SDL_MAX_UINT64) return SDL_SetError("Scarlet GPU serial exhausted");
    result = sws_gpu_commit(image->identity, ++image->serial, image->width, image->height);
    if (result < 0) { image->failed = SDL_TRUE; return SWS_Error("GPU commit", result); }
    image->pending = SDL_TRUE;
    return 0;
}
int SDL_ScarletWaitGpuRelease(SDL_Window *window, Uint32 slot, Uint32 timeout_ms)
{
    ScarletWindow *data = Scarlet_GpuWindow(window);
    ScarletGpuImage *image;
    Uint64 started = SDL_GetTicks64();
    SwsGpuEvent event;
    int result, i;
    if (!data || !(image = Scarlet_GpuSlot(data, slot))) return -1;
    if (!image->attached || image->failed) return SDL_SetError("Scarlet GPU image unavailable");
    do {
        result = sws_gpu_poll(data->id, &event);
        if (result < 0) { image->failed = SDL_TRUE; return SWS_Error("GPU poll", result); }
        if (result > 0) for (i = 0; i < 2; ++i) {
            ScarletGpuImage *current = &data->gpu[i];
            if (!current->attached || event.buffer.window_id != current->identity.window_id) continue;
            /* BACKEND_LOST announces the new epoch, invalidating old images. */
            if (event.kind == SWS_GPU_BACKEND_LOST) current->failed = SDL_TRUE;
            else if (event.buffer.compositor_epoch == current->identity.compositor_epoch &&
                     event.buffer.buffer_id == current->identity.buffer_id &&
                     event.buffer.generation == current->identity.generation) {
                if (event.kind == SWS_GPU_REJECTED) current->failed = SDL_TRUE;
                if (event.kind == SWS_GPU_RELEASED && event.commit_serial == current->serial)
                    current->pending = SDL_FALSE;
            }
        }
        if (image->failed) return SDL_SetError("Scarlet GPU rejected or backend lost; recreate window");
        if (!image->pending) return 0;
        if (!result) SDL_Delay(1);
    } while (SDL_GetTicks64() - started < timeout_ms);
    return SDL_SetError("Scarlet GPU release timeout; image remains in flight");
}
int SDL_ScarletDetachGpuImage(SDL_Window *window, Uint32 slot)
{
    ScarletWindow *data = Scarlet_GpuWindow(window);
    ScarletGpuImage *image;
    int result;
    if (!data || !(image = Scarlet_GpuSlot(data, slot))) return -1;
    if (!image->attached) return 0;
    if (image->pending || image->failed) return SDL_SetError("Scarlet GPU image not released; recreate window");
    result = sws_gpu_destroy(image->identity);
    if (result < 0) return SWS_Error("GPU destroy", result);
    image->attached = SDL_FALSE;
    return 0;
}
int SDL_ScarletRetireGpuImages(SDL_Window *window, Uint32 timeout_ms)
{
    ScarletWindow *data = Scarlet_GpuWindow(window);
    int result;
    if (!data) return -1;
    result = sws_window_destroy_sync(data->id, timeout_ms);
    if (result < 0) return SWS_Error("GPU window retirement", result);
    data->retired = SDL_TRUE;
    return 0;
}
static int Scarlet_VideoInit(_THIS)
{
    SwsDisplay display;
    SDL_DisplayMode mode;
    int result = sws_get_display(&display);
    if (result < 0) return SWS_Error("display", result);
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = (int)display.width;
    mode.h = (int)display.height;
    mode.refresh_rate = 60;
    return SDL_AddBasicVideoDisplay(&mode) < 0 ? -1 : 0;
}
static void Scarlet_VideoQuit(_THIS) { (void)_this; SDL_GetMouse()->SetRelativeMouseMode = NULL; }
static int Scarlet_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    if (mode->w != display->desktop_mode.w || mode->h != display->desktop_mode.h)
        return SDL_Unsupported();
    return 0;
}
static int Scarlet_CreateWindow(_THIS, SDL_Window *window)
{
    ScarletWindow *data;
    Uint32 width, height;
    int result;
    if (window->flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN | SDL_WINDOW_METAL))
        return SDL_Unsupported();
    data = SDL_calloc(1, sizeof(*data));
    if (!data) return SDL_OutOfMemory();
    result = sws_window_create_ex("org.scarlet.sdl2", window->title ? window->title : "SDL2",
                                (Uint32)window->w, (Uint32)window->h,
                                !!(window->flags & SDL_WINDOW_RESIZABLE), &data->id);
    if (result < 0) { SDL_free(data); return SWS_Error("create", result); }
    window->driverdata = data;
    result = sws_window_size(data->id, &width, &height);
    if (result < 0) {
        sws_window_destroy(data->id);
        SDL_free(data);
        window->driverdata = NULL;
        return SWS_Error("size", result);
    }
    window->w = (int)width;
    window->h = (int)height;
    /* SDL creates every window hidden, then shows ordinary windows through
     * FinishWindowCreation. An explicitly hidden window stays hidden in SWS. */
    result = sws_window_action(data->id, SWS_WINDOW_HIDE);
    if (result < 0) {
        sws_window_destroy(data->id);
        SDL_free(data);
        window->driverdata = NULL;
        return SWS_Error("initial hide", result);
    }
    return 0;
}
static void Scarlet_DestroyFramebuffer(_THIS, SDL_Window *window)
{
    ScarletWindow *data = window->driverdata;
    if (data) { SDL_FreeSurface(data->frame); data->frame = NULL; }
}
static void Scarlet_DestroyWindow(_THIS, SDL_Window *window)
{
    ScarletWindow *data = window->driverdata;
    if (!data) return;
    Scarlet_DestroyFramebuffer(_this, window);
    if (!data->retired) sws_window_destroy(data->id);
    SDL_free(data);
    window->driverdata = NULL;
}
static int Scarlet_CreateFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    ScarletWindow *data = window->driverdata;
    if (!data) return SDL_SetError("No Scarlet window");
    Scarlet_DestroyFramebuffer(_this, window);
    data->frame = SDL_CreateRGBSurfaceWithFormat(0, window->w, window->h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!data->frame) return -1;
    *format = data->frame->format->format;
    *pixels = data->frame->pixels;
    *pitch = data->frame->pitch;
    return 0;
}
static int Scarlet_UpdateFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int count)
{
    ScarletWindow *data = window->driverdata;
    SDL_Surface *frame = data ? data->frame : NULL;
    int result;
    (void)rects; (void)count;
    if (!frame) return SDL_SetError("No Scarlet framebuffer");
    if (data->gpu[0].attached || data->gpu[1].attached) return SDL_SetError("Detach Scarlet GPU image before software presentation");
    result = sws_window_present(data->id, frame->pixels, (size_t)frame->pitch * frame->h,
                               (Uint32)frame->w, (Uint32)frame->h, (size_t)frame->pitch);
    return result < 0 ? SWS_Error("present", result) : 0;
}
static void Scarlet_SetWindowSize(_THIS, SDL_Window *window)
{
    ScarletWindow *data = window->driverdata;
    Uint32 width, height;
    int result = sws_window_resize(data->id, (Uint32)window->w, (Uint32)window->h);
    if (result < 0) { SWS_Error("resize", result); return; }
    result = sws_window_size(data->id, &width, &height);
    if (result < 0) { SWS_Error("size", result); return; }
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, (int)width, (int)height);
}
static void Scarlet_SetTitle(_THIS, SDL_Window *window)
{
    ScarletWindow *data = window->driverdata;
    int result = sws_window_title(data->id, window->title ? window->title : "SDL2");
    if (result < 0) SWS_Error("title", result);
}
static void Scarlet_WindowAction(SDL_Window *window, Uint32 action)
{
    ScarletWindow *data = window->driverdata;
    if (!data) return;
    int result = sws_window_action(data->id, action);
    if (result < 0) SWS_Error("window action", result);
}
static void Scarlet_Show(_THIS, SDL_Window *window) { Scarlet_WindowAction(window, SWS_WINDOW_SHOW); }
static void Scarlet_Hide(_THIS, SDL_Window *window) { Scarlet_WindowAction(window, SWS_WINDOW_HIDE); }
static void Scarlet_Raise(_THIS, SDL_Window *window) { Scarlet_WindowAction(window, SWS_WINDOW_RAISE); }
static void Scarlet_Maximize(_THIS, SDL_Window *window) { Scarlet_WindowAction(window, SWS_WINDOW_MAXIMIZE); }
static void Scarlet_Restore(_THIS, SDL_Window *window) { Scarlet_WindowAction(window, SWS_WINDOW_RESTORE); }
static void Scarlet_SetFullscreen(_THIS, SDL_Window *window, SDL_VideoDisplay *display, SDL_bool fullscreen)
{
    ScarletWindow *data = window->driverdata;
    int result = sws_window_fullscreen(data->id, fullscreen);
    (void)display;
    if (result < 0) SWS_Error("fullscreen", result);
}
static int Scarlet_SetRelativeMouseMode(SDL_bool enabled)
{
    SDL_Window *window = SDL_GetKeyboardFocus();
    ScarletWindow *data = window ? window->driverdata : NULL;
    int result;
    if (!data) return enabled ? SDL_SetError("No focused Scarlet window") : 0;
    result = sws_window_pointer_lock(data->id, enabled);
    return result < 0 ? SWS_Error("pointer lock", result) : 0;
}
static void Scarlet_SetMouseGrab(_THIS, SDL_Window *window, SDL_bool grabbed)
{
    ScarletWindow *data = window->driverdata;
    if (!data) return;
    int result = sws_window_pointer_lock(data->id, grabbed || SDL_GetRelativeMouseMode());
    if (result < 0) SWS_Error("pointer grab", result);
}
static SDL_Window *Scarlet_FindWindow(_THIS, Uint32 id)
{
    SDL_Window *window;
    for (window = _this->windows; window; window = window->next) {
        ScarletWindow *data = window->driverdata;
        if (data && data->id == id) return window;
    }
    return NULL;
}
static void Scarlet_Input(SDL_Window *window, ScarletWindow *data, const SwsEvent *event)
{
    if (event->type == 1) { /* EV_KEY, including pointer buttons */
        if (event->code >= 272 && event->code <= 276) {
            static const Uint8 buttons[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE,
                                            SDL_BUTTON_X1, SDL_BUTTON_X2 };
            SDL_SetMouseFocus(window);
            SDL_SendMouseButton(window, 0, event->value ? SDL_PRESSED : SDL_RELEASED, buttons[event->code - 272]);
        } else if (event->code < SDL_arraysize(linux_scancode_table)) {
            SDL_Scancode code = linux_scancode_table[event->code];
            if (code != SDL_SCANCODE_UNKNOWN)
                SDL_SendKeyboardKey(event->value ? SDL_PRESSED : SDL_RELEASED, code);
        }
    } else if (event->type == 2) { /* EV_REL */
        if (event->code <= 1) {
            SDL_SetMouseFocus(window);
            SDL_SendMouseMotion(window, 0, SDL_TRUE, event->code == 0 ? event->value : 0,
                                event->code == 1 ? event->value : 0);
        } else if (event->code == 6 || event->code == 8) {
            SDL_SendMouseWheel(window, 0, event->code == 6 ? (float)event->value : 0,
                               event->code == 8 ? (float)event->value : 0, SDL_MOUSEWHEEL_NORMAL);
        }
    } else if (event->type == 3 && event->code <= 1) { /* EV_ABS, window-local coordinates */
        if (event->code == 0) data->mouse_x = event->value;
        else data->mouse_y = event->value;
        data->mouse_pending = SDL_TRUE;
    } else if (event->type == 0 && event->code == 0 && data->mouse_pending) { /* SYN_REPORT */
        SDL_SetMouseFocus(window);
        SDL_SendMouseMotion(window, 0, SDL_FALSE, data->mouse_x, data->mouse_y);
        data->mouse_pending = SDL_FALSE;
    }
}
static void Scarlet_PumpEvents(_THIS)
{
    SwsEvent event;
    int n;
    for (n = 0; n < 64; ++n) {
        int result = sws_poll_event(&event);
        SDL_Window *window;
        ScarletWindow *data;
        if (result < 0) { SWS_Error("events", result); break; }
        if (!result) break;
        window = Scarlet_FindWindow(_this, event.window_id);
        if (!window) continue;
        data = window->driverdata;
        switch (event.kind) {
        case SWS_EVENT_INPUT: Scarlet_Input(window, data, &event); break;
        case SWS_EVENT_CONFIGURE:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, (int)event.width, (int)event.height);
            break;
        case SWS_EVENT_DESTROYED: SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0); break;
        case SWS_EVENT_FOCUS:
            if (event.value) SDL_SetKeyboardFocus(window);
            else if (SDL_GetKeyboardFocus() == window) SDL_SetKeyboardFocus(NULL);
            break;
        }
    }
}
static void Scarlet_DeleteDevice(SDL_VideoDevice *device) { SDL_free(device); }
static SDL_VideoDevice *Scarlet_CreateDevice(void)
{
    SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    device->VideoInit = Scarlet_VideoInit;
    device->VideoQuit = Scarlet_VideoQuit;
    device->SetDisplayMode = Scarlet_SetDisplayMode;
    device->CreateSDLWindow = Scarlet_CreateWindow;
    device->DestroyWindow = Scarlet_DestroyWindow;
    device->SetWindowSize = Scarlet_SetWindowSize;
    device->SetWindowTitle = Scarlet_SetTitle;
    device->ShowWindow = Scarlet_Show;
    device->HideWindow = Scarlet_Hide;
    device->RaiseWindow = Scarlet_Raise;
    device->MinimizeWindow = Scarlet_Hide;
    device->MaximizeWindow = Scarlet_Maximize;
    device->RestoreWindow = Scarlet_Restore;
    device->SetWindowFullscreen = Scarlet_SetFullscreen;
    device->SetWindowMouseGrab = Scarlet_SetMouseGrab;
    device->CreateWindowFramebuffer = Scarlet_CreateFramebuffer;
    device->UpdateWindowFramebuffer = Scarlet_UpdateFramebuffer;
    device->DestroyWindowFramebuffer = Scarlet_DestroyFramebuffer;
    device->PumpEvents = Scarlet_PumpEvents;
    device->free = Scarlet_DeleteDevice;
    SDL_GetMouse()->SetRelativeMouseMode = Scarlet_SetRelativeMouseMode;
    return device;
}
VideoBootStrap SCARLET_bootstrap = { "scarlet", "Scarlet native SWS", Scarlet_CreateDevice, NULL };
