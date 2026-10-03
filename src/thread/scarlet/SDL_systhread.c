/* Experimental Scarlet Native thread backend, using the native C ABI. */
#include "../../SDL_internal.h"
#include "../SDL_systhread.h"
#include <pthread.h>

static void *RunThread(void *data)
{
    SDL_RunThread((SDL_Thread *)data);
    return NULL;
}

int SDL_SYS_CreateThread(SDL_Thread *thread)
{
    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    if (result) return SDL_SetError("pthread_attr_init: %d", result);
    if (thread->stacksize) {
        size_t size = thread->stacksize;
        if (size < PTHREAD_STACK_MIN) size = PTHREAD_STACK_MIN;
        if (size > (size_t)-1 - 4095) {
            pthread_attr_destroy(&attr);
            return SDL_SetError("Thread stack size overflow");
        }
        result = pthread_attr_setstacksize(&attr, (size + 4095) & ~(size_t)4095);
    }
    if (!result) result = pthread_create(&thread->handle, &attr, RunThread, thread);
    pthread_attr_destroy(&attr);
    return result ? SDL_SetError("pthread_create: %d", result) : 0;
}

void SDL_SYS_SetupThread(const char *name) { (void)name; }
SDL_threadID SDL_ThreadID(void) { return (SDL_threadID)pthread_self(); }
int SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    if (priority == SDL_THREAD_PRIORITY_NORMAL) return 0;
    return SDL_Unsupported();
}
void SDL_SYS_WaitThread(SDL_Thread *thread) { pthread_join(thread->handle, NULL); }
void SDL_SYS_DetachThread(SDL_Thread *thread) { pthread_detach(thread->handle); }
