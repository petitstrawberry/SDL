/* Experimental native Scarlet image presentation. SDL zlib license. */
#ifndef SDL_scarlet_h_
#define SDL_scarlet_h_
#include "SDL_video.h"
#include "begin_code.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Video-thread only; valid only with the native scarlet driver. The borrowed
 * handle is a Scarlet SGFX shared BGRA8 image exported by
 * vkGetImageScarletHandleSGFX, never a Linux fd. SWS duplicates it.
 * Finish producer GPU writes before Present. WaitGpuRelease must succeed before
 * overwriting/reusing the image or detaching it. Timeout preserves ownership.
 * Slots 0 and 1 form a double buffer: SWS releases the previous image only
 * after its replacement is displayed. A displayed image cannot be detached.
 * Keep images alive until detached or RetireGpuImages succeeds; this closes the
 * SWS window and confirms retirement before SDL_DestroyWindow. On timeout keep
 * the producer resources alive and retry retirement. Resolve resize/backend-loss
 * by retiring the window. SDL_WINDOW_VULKAN/KHR_surface and SDL_RenderCopy GPU acceleration
 * are not implemented by this API. The usual software surface works detached. */
extern DECLSPEC int SDLCALL SDL_ScarletAttachGpuImage(SDL_Window *window, Uint32 slot, int handle, Uint32 width, Uint32 height);
extern DECLSPEC int SDLCALL SDL_ScarletPresentGpuImage(SDL_Window *window, Uint32 slot);
extern DECLSPEC int SDLCALL SDL_ScarletWaitGpuRelease(SDL_Window *window, Uint32 slot, Uint32 timeout_ms);
extern DECLSPEC int SDLCALL SDL_ScarletDetachGpuImage(SDL_Window *window, Uint32 slot);
extern DECLSPEC int SDLCALL SDL_ScarletRetireGpuImages(SDL_Window *window, Uint32 timeout_ms);
#ifdef __cplusplus
}
#endif
#include "close_code.h"
#endif
