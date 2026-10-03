/* Experimental Scarlet Native timer backend. */
#include "../../SDL_internal.h"
#include "SDL_timer.h"
#include <time.h>

static SDL_bool ticks_started;
static Uint64 ticks_origin;
void SDL_TicksInit(void)
{
    if (!ticks_started) {
        ticks_origin = SDL_GetPerformanceCounter();
        ticks_started = SDL_TRUE;
    }
}
void SDL_TicksQuit(void) { ticks_started = SDL_FALSE; }

Uint64 SDL_GetPerformanceCounter(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        SDL_SetError("Scarlet monotonic clock unavailable");
        return 0;
    }
    return (Uint64)now.tv_sec * 1000000 + (Uint64)now.tv_nsec / 1000;
}
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }
Uint64 SDL_GetTicks64(void)
{
    SDL_TicksInit();
    return (SDL_GetPerformanceCounter() - ticks_origin) / 1000;
}
void SDL_Delay(Uint32 ms)
{
    struct timespec delay;
    delay.tv_sec = ms / 1000;
    delay.tv_nsec = (long)(ms % 1000) * 1000000;
    if (nanosleep(&delay, NULL) != 0) SDL_SetError("Scarlet sleep failed");
}
