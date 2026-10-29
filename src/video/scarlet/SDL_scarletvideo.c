/* Native Scarlet SWS/ScarletUI backend. Copyright (c) 2026 Scarlet contributors.
 * Altered SDL platform implementation; distributed under SDL's zlib license. */
#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"
#include "../SDL_vulkan_internal.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "../../events/scancodes_linux.h"
#include <sws_client.h>
#include <scarlet_ui_window.h>

struct SDL_WindowData {
    Uint32 id;
    ScarletUiFrame *frame;
    SwsSceneGeometry geometry;
    int pointer_x, pointer_y;
    Uint32 frame_buttons, client_buttons;
};

static bool checked(int result, const char *operation)
{
    return result >= 0 || SDL_SetError("Scarlet %s failed (%d)", operation, result);
}

static Uint32 frame_flags(SDL_Window *window, Uint32 state)
{
    return (!(window->flags & SDL_WINDOW_BORDERLESS) && !(state & (4 | 16)) ? SCARLET_UI_FRAME_DECORATED : 0) |
           (window->flags & SDL_WINDOW_RESIZABLE ? SCARLET_UI_FRAME_RESIZABLE : 0);
}

static int logical(Uint32 pixels, Uint32 scale)
{
    return (int)(((Uint64)pixels * 1000 + scale / 2) / scale);
}

static Uint32 physical(int pixels, Uint32 scale)
{
    return (Uint32)(((Uint64)pixels * scale + 999) / 1000);
}

static bool refresh(SDL_Window *window, bool notify)
{
    SDL_WindowData *data = window->internal;
    SwsSceneGeometry geometry;
    if (!checked(sws_scene_geometry(data->id, &geometry), "window geometry") || !geometry.scale_milli)
        return false;
    if (!checked(scarlet_ui_frame_configure(data->frame, window->title ? window->title : "",
                 geometry.width, geometry.height, geometry.scale_milli, frame_flags(window, geometry.state)), "frame configuration"))
        return false;
    if (scarlet_ui_frame_dirty(data->frame)) {
        ScarletUiFrameView view;
        SwsRect clips[256];
        if (!checked(scarlet_ui_frame_render(data->frame, &view), "frame rendering")) return false;
        if (view.clip_count > SDL_arraysize(clips)) return SDL_SetError("Scarlet frame clip count exceeds limit");
        for (size_t i = 0; i < view.clip_count; i++)
            clips[i] = (SwsRect){ view.clip[i].x, view.clip[i].y, view.clip[i].width, view.clip[i].height };
        SwsSceneFrame frame = { view.width, view.height, view.stride,
            { view.insets.left, view.insets.top, view.insets.right, view.insets.bottom },
            view.pixels, view.bytes, clips, view.clip_count };
        if (!checked(sws_scene_frame(data->id, &frame), "frame composition")) return false;
        if (geometry.width < view.insets.left + view.insets.right || geometry.height < view.insets.top + view.insets.bottom)
            return SDL_SetError("Scarlet frame exceeds window dimensions");
        geometry.content = (SwsRect){ view.insets.left, view.insets.top,
            geometry.width - view.insets.left - view.insets.right,
            geometry.height - view.insets.top - view.insets.bottom };
    }
    bool pixels_changed = data->geometry.content.width != geometry.content.width || data->geometry.content.height != geometry.content.height;
    data->geometry = geometry;
    if (notify) {
        SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, logical(geometry.content.width, geometry.scale_milli),
                            logical(geometry.content.height, geometry.scale_milli));
        if (pixels_changed) SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, geometry.content.width, geometry.content.height);
    }
    return true;
}

static bool video_init(SDL_VideoDevice *_this)
{
    SwsDisplay display;
    Uint32 scale;
    if (scarlet_ui_frame_abi() != 1) return SDL_SetError("Unsupported ScarletUI window ABI");
    if (!checked(sws_get_display(&display), "display query") || !checked(sws_get_output_scale(&scale), "display scale") || !scale)
        return false;
    SDL_DisplayMode mode;
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_BGRA8888;
    mode.w = display.width;
    mode.h = display.height;
    mode.pixel_density = scale / 1000.0f;
    if (!SDL_AddBasicVideoDisplay(&mode)) return false;
    SDL_AddKeyboard(SDL_DEFAULT_KEYBOARD_ID, "SWS keyboard");
    return true;
}

static void video_quit(SDL_VideoDevice *_this) { SDL_RemoveKeyboard(SDL_DEFAULT_KEYBOARD_ID); }

static bool create_window(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props)
{
    if (window->flags & SDL_WINDOW_OPENGL) return SDL_SetError("Scarlet SDL supports Vulkan windows; OpenGL is unavailable");
    SDL_WindowData *data = SDL_calloc(1, sizeof(*data));
    if (!data) return false;
    Uint32 scale;
    ScarletUiInsets insets;
    if (!checked(sws_get_output_scale(&scale), "output scale") || !scale ||
        !checked(scarlet_ui_frame_insets(scale, frame_flags(window, 0), &insets), "frame insets")) goto failure;
    data->frame = scarlet_ui_frame_create();
    if (!data->frame) goto failure;
    if (!checked(sws_scene_create(window->title ? window->title : "", physical(window->w, scale) + insets.left + insets.right,
        physical(window->h, scale) + insets.top + insets.bottom, !!(window->flags & SDL_WINDOW_RESIZABLE), &data->id), "window creation")) goto failure;
    window->internal = data;
    if (!refresh(window, false)) goto failure;
    window->x = window->y = 0;
    return true;
failure:
    if (data->id) sws_window_destroy(data->id);
    if (data->frame) scarlet_ui_frame_destroy(data->frame);
    SDL_free(data);
    window->internal = NULL;
    return false;
}

static void destroy_window(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data = window->internal;
    if (!data) return;
    checked(sws_window_destroy(data->id), "window destruction");
    scarlet_ui_frame_destroy(data->frame);
    SDL_free(data);
    window->internal = NULL;
}

static void set_title(SDL_VideoDevice *_this, SDL_Window *window)
{
    if (checked(sws_window_title(window->internal->id, window->title), "window title")) refresh(window, false);
}
static void set_size(SDL_VideoDevice *_this, SDL_Window *window)
{
    Uint32 scale = window->internal->geometry.scale_milli;
    if (checked(sws_window_resize(window->internal->id, physical(window->pending.w, scale), physical(window->pending.h, scale)), "window resize")) refresh(window, true);
}
static void pixel_size(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h)
{
    refresh(window, false);
    *w = window->internal->geometry.content.width;
    *h = window->internal->geometry.content.height;
}
static float content_scale(SDL_VideoDevice *_this, SDL_Window *window)
{ return window->internal->geometry.scale_milli / 1000.0f; }
static void show(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_SHOW), "show"); }
static void hide(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_HIDE), "hide"); }
static void raise_window(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_RAISE), "raise"); }
static void maximize(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_MAXIMIZE), "maximize"); }
static void restore(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_RESTORE), "restore"); }
static void minimize(SDL_VideoDevice *_this, SDL_Window *window) { checked(sws_window_action(window->internal->id, SWS_WINDOW_HIDE), "minimize"); }
static bool borders(SDL_VideoDevice *_this, SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    if (!refresh(window, false)) return false;
    SwsSceneGeometry g = window->internal->geometry;
    *top = logical(g.content.y, g.scale_milli);
    *left = logical(g.content.x, g.scale_milli);
    *bottom = logical(g.height - g.content.y - g.content.height, g.scale_milli);
    *right = logical(g.width - g.content.x - g.content.width, g.scale_milli);
    return true;
}
static void bordered(SDL_VideoDevice *_this, SDL_Window *window, bool enabled)
{
    SDL_WindowData *data = window->internal;
    ScarletUiInsets insets;
    if (!checked(scarlet_ui_frame_insets(data->geometry.scale_milli, frame_flags(window, data->geometry.state), &insets), "frame insets")) return;
    if (checked(sws_scene_resize(data->id, data->geometry.content.width + insets.left + insets.right,
                                data->geometry.content.height + insets.top + insets.bottom), "frame resize")) refresh(window, true);
}
static void resizable(SDL_VideoDevice *_this, SDL_Window *window, bool enabled)
{
    if (checked(sws_window_resizable(window->internal->id, enabled), "window resizability")) refresh(window, false);
}
static bool text_start(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props) { return true; }
static bool text_stop(SDL_VideoDevice *_this, SDL_Window *window) { return true; }
static void frame_pointer(SDL_VideoDevice *_this, SDL_Window *window, Uint32 kind)
{
    SDL_WindowData *data = window->internal;
    Uint32 actions = 0;
    if (!checked(scarlet_ui_frame_pointer(data->frame, kind, data->pointer_x, data->pointer_y, &actions), "frame pointer")) return;
    if (actions & SCARLET_UI_FRAME_CLOSE) SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
    if (actions & SCARLET_UI_FRAME_MOVE) checked(sws_window_action(data->id, SWS_WINDOW_MOVE), "frame move");
    if (actions & SCARLET_UI_FRAME_MINIMIZE) minimize(_this, window);
    if (actions & SCARLET_UI_FRAME_TOGGLE_MAXIMIZE) {
        if (data->geometry.state & 2) restore(_this, window); else maximize(_this, window);
    }
    refresh(window, false);
}
static SDL_FullscreenResult fullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp op)
{
    if (!checked(sws_window_fullscreen(window->internal->id, op != SDL_FULLSCREEN_OP_LEAVE), "fullscreen")) return SDL_FULLSCREEN_FAILED;
    refresh(window, true);
    return SDL_FULLSCREEN_SUCCEEDED;
}
static bool relative_mouse(bool enabled)
{
    SDL_Window *window = SDL_GetMouseFocus();
    return window && checked(sws_window_pointer_lock(window->internal->id, enabled), "relative pointer");
}

static void pump_events(SDL_VideoDevice *_this)
{
    SwsEvent event;
    int result;
    while ((result = sws_poll_event(&event)) > 0) {
        SDL_Window *window;
        for (window = _this->windows; window; window = window->next)
            if (window->internal && window->internal->id == event.window_id) break;
        if (!window) continue;
        SDL_WindowData *data = window->internal;
        Uint64 now = SDL_GetTicksNS();
        if (event.kind == SWS_EVENT_CONFIGURE || event.kind == SWS_EVENT_SCALE || event.kind == SWS_EVENT_STATE) {
            refresh(window, true);
            if (event.kind == SWS_EVENT_STATE) {
                bool minimized = !!(event.value & 1), maximized = !!(event.value & 2);
                if (minimized != !!(window->flags & SDL_WINDOW_MINIMIZED))
                    SDL_SendWindowEvent(window, minimized ? SDL_EVENT_WINDOW_MINIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
                if (maximized != !!(window->flags & SDL_WINDOW_MAXIMIZED))
                    SDL_SendWindowEvent(window, maximized ? SDL_EVENT_WINDOW_MAXIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
            }
        } else if (event.kind == SWS_EVENT_CLOSE_REQUESTED || event.kind == SWS_EVENT_DESTROYED) {
            SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
        } else if (event.kind == SWS_EVENT_FOCUS) {
            if (!event.value) {
                int x = data->pointer_x, y = data->pointer_y;
                data->pointer_x = data->pointer_y = -1;
                frame_pointer(_this, window, SCARLET_UI_POINTER_MOVE);
                frame_pointer(_this, window, SCARLET_UI_POINTER_RELEASE);
                data->pointer_x = x; data->pointer_y = y;
                data->frame_buttons = data->client_buttons = 0;
            }
            SDL_SetKeyboardFocus(event.value ? window : NULL);
            SDL_SetMouseFocus(event.value ? window : NULL);
        } else if (event.kind == SWS_EVENT_INPUT) {
            if (event.type == 1 && event.code >= 272 && event.code <= 276) {
                static const Uint8 buttons[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2 };
                Uint32 bit = 1u << (event.code - 272);
                SwsRect r = data->geometry.content;
                bool in_content = data->pointer_x >= (int)r.x && data->pointer_y >= (int)r.y &&
                    data->pointer_x < (int)(r.x + r.width) && data->pointer_y < (int)(r.y + r.height);
                if (event.value) {
                    if (data->frame_buttons || (!data->client_buttons && !in_content && !SDL_GetWindowRelativeMouseMode(window))) {
                        data->frame_buttons |= bit;
                        if (event.code == 272) frame_pointer(_this, window, SCARLET_UI_POINTER_PRESS);
                        continue;
                    }
                    data->client_buttons |= bit;
                } else {
                    if (data->frame_buttons & bit) {
                        data->frame_buttons &= ~bit;
                        if (event.code == 272) frame_pointer(_this, window, SCARLET_UI_POINTER_RELEASE);
                        continue;
                    }
                    if (!(data->client_buttons & bit)) continue;
                    data->client_buttons &= ~bit;
                }
                SDL_SendMouseButton(now, window, SDL_DEFAULT_MOUSE_ID, buttons[event.code - 272], event.value != 0);
            } else if (event.type == 1 && event.code < SDL_arraysize(linux_scancode_table)) {
                SDL_Scancode scan = linux_scancode_table[event.code];
                SDL_SendKeyboardKey(now, SDL_DEFAULT_KEYBOARD_ID, event.code, scan, event.value != 0);
                if (event.value && SDL_TextInputActive(window)) {
                    SDL_Keycode key = SDL_GetKeyFromScancode(scan, SDL_GetModState(), false);
                    if (key >= 32 && key <= 0x10ffff) {
                        char text[8];
                        *SDL_UCS4ToUTF8(key, text) = 0;
                        SDL_SendKeyboardText(text);
                    }
                }
            } else if (event.type == 3 && event.code <= 1) {
                if (event.code == 0) data->pointer_x = event.value; else data->pointer_y = event.value;
                frame_pointer(_this, window, SCARLET_UI_POINTER_MOVE);
                SwsRect r = data->geometry.content;
                SDL_SendMouseMotion(now, window, SDL_DEFAULT_MOUSE_ID, false,
                    (data->pointer_x - (int)r.x) * 1000.0f / data->geometry.scale_milli,
                    (data->pointer_y - (int)r.y) * 1000.0f / data->geometry.scale_milli);
            } else if (event.type == 2 && event.code <= 1) {
                SDL_SendMouseMotion(now, window, SDL_DEFAULT_MOUSE_ID, true, event.code == 0 ? event.value : 0, event.code == 1 ? event.value : 0);
            } else if (event.type == 2 && event.code == 8) {
                SDL_SendMouseWheel(now, window, SDL_DEFAULT_MOUSE_ID, 0, event.value, SDL_MOUSEWHEEL_NORMAL);
            }
        }
    }
    if (result < 0) SDL_SetError("Scarlet event poll failed (%d)", result);
}

#ifdef SDL_VIDEO_VULKAN
typedef struct ScarletSurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    Uint32 window;
} ScarletSurfaceInfo;
typedef VkResult (VKAPI_PTR *CreateScarletSurface)(VkInstance, const ScarletSurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef VkBool32 (VKAPI_PTR *ScarletPresentationSupport)(VkPhysicalDevice, Uint32);
static bool load_vulkan(SDL_VideoDevice *_this, const char *path)
{
    SDL_SharedObject *library = SDL_LoadObject(path ? path : "/lib/libvulkan.so.1");
    if (!library) return false;
    PFN_vkGetInstanceProcAddr proc = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(library, "vkGetInstanceProcAddr");
    if (!proc) { SDL_UnloadObject(library); return false; }
    _this->vulkan_config.loader_handle = library;
    _this->vulkan_config.vkGetInstanceProcAddr = (void *)proc;
    return true;
}
static void unload_vulkan(SDL_VideoDevice *_this)
{
    SDL_UnloadObject(_this->vulkan_config.loader_handle);
    _this->vulkan_config.loader_handle = NULL;
    _this->vulkan_config.vkGetInstanceProcAddr = NULL;
}
static const char * const *extensions(SDL_VideoDevice *_this, Uint32 *count)
{
    static const char *names[] = { "VK_KHR_surface", "VK_SGFX_scarlet_surface" };
    *count = SDL_arraysize(names);
    return names;
}
static bool create_surface(SDL_VideoDevice *_this, SDL_Window *window, VkInstance instance, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    PFN_vkGetInstanceProcAddr proc = (PFN_vkGetInstanceProcAddr)_this->vulkan_config.vkGetInstanceProcAddr;
    CreateScarletSurface create = (CreateScarletSurface)proc(instance, "vkCreateScarletSurfaceSGFX");
    if (!create) return SDL_SetError("Vulkan instance does not enable VK_SGFX_scarlet_surface");
    ScarletSurfaceInfo info = { (VkStructureType)0x7f530001, NULL, 0, window->internal->id };
    VkResult result = create(instance, &info, allocator, surface);
    return result == VK_SUCCESS || SDL_SetError("Scarlet Vulkan surface creation failed (%d)", result);
}
static void destroy_surface(SDL_VideoDevice *_this, VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator)
{ SDL_Vulkan_DestroySurface_Internal(_this->vulkan_config.vkGetInstanceProcAddr, instance, surface, allocator); }
static bool presentation(SDL_VideoDevice *_this, VkInstance instance, VkPhysicalDevice physical, Uint32 queue)
{
    PFN_vkGetInstanceProcAddr proc = (PFN_vkGetInstanceProcAddr)_this->vulkan_config.vkGetInstanceProcAddr;
    ScarletPresentationSupport support = (ScarletPresentationSupport)proc(instance, "vkGetPhysicalDeviceScarletPresentationSupportSGFX");
    return support && support(physical, queue);
}
#endif

static void delete_device(SDL_VideoDevice *device) { SDL_free(device); }
static SDL_VideoDevice *create_device(void)
{
    SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->VideoInit = video_init;
    device->VideoQuit = video_quit;
    device->CreateSDLWindow = create_window;
    device->DestroyWindow = destroy_window;
    device->SetWindowTitle = set_title;
    device->SetWindowSize = set_size;
    device->GetWindowSizeInPixels = pixel_size;
    device->GetWindowContentScale = content_scale;
    device->GetWindowBordersSize = borders;
    device->ShowWindow = show;
    device->HideWindow = hide;
    device->RaiseWindow = raise_window;
    device->MaximizeWindow = maximize;
    device->MinimizeWindow = minimize;
    device->RestoreWindow = restore;
    device->SetWindowBordered = bordered;
    device->SetWindowResizable = resizable;
    device->StartTextInput = text_start;
    device->StopTextInput = text_stop;
    device->SetWindowFullscreen = fullscreen;
    device->PumpEvents = pump_events;
    device->free = delete_device;
#ifdef SDL_VIDEO_VULKAN
    device->Vulkan_LoadLibrary = load_vulkan;
    device->Vulkan_UnloadLibrary = unload_vulkan;
    device->Vulkan_GetInstanceExtensions = extensions;
    device->Vulkan_CreateSurface = create_surface;
    device->Vulkan_DestroySurface = destroy_surface;
    device->Vulkan_GetPresentationSupport = presentation;
#endif
    SDL_GetMouse()->SetRelativeMouseMode = relative_mouse;
    return device;
}
VideoBootStrap SCARLET_bootstrap = { "scarlet", "Scarlet SWS/ScarletUI", create_device, NULL, false };
