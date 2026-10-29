# Scarlet

The native AArch64 backend uses SWS scene windows and ScarletUI window ABI 1
for window frames. Vulkan surfaces use `VK_SGFX_scarlet_surface` from the
native Vulkan loader. This port does not use the Linux syscall ABI.

Configure with a cross toolchain setting `CMAKE_SYSTEM_NAME=Scarlet`, the
matching Scarlet SDK headers and native ELF compiler. Set
`SCARLET_SWS_INCLUDE_DIR` and `SCARLET_UI_INCLUDE_DIR` to the SWS and ScarletUI
header directories. `SCARLET_SWS_LIBRARY` and `SCARLET_UI_LIBRARY` select the
native libraries and default to `sws_client_c` and `scarlet_ui_window`.

The current validated configuration builds a position independent static
archive, with pthreads and Vulkan enabled. Disable audio, GPU, SDL renderer,
OpenGL, OpenGL ES, joystick, haptics, sensor, camera and dialog subsystems.
Disable `SDL_ARMNEON` while the SDK lacks the full floating point environment
API. `LIBC_HAS_LROUND=OFF` and `LIBC_HAS_LROUNDF=OFF` select SDL's portable
fallbacks when the hosting executable does not export those functions.
Shared packaging must link the native SWS and ScarletUI libraries and provide
the actual SDK C/math import closure.

Implemented video operations include content size and scale queries, window
frames, border changes, resize, visibility, focus, window state changes,
keyboard and pointer events, pointer locking, and Vulkan surface creation.
Text input currently translates the US keyboard layout; IME composition and
keyboard layout discovery are not implemented. Audio and OpenGL contexts are
not supplied by this backend. Unsupported thread priorities return an error.

Acceptance exercises an SDL window, ScarletUI borders, a real Vulkan
swapchain, frame presentation, resize, keyboard and text events, and teardown
from native LWJGL on an AArch64 HVF guest. A successful library load alone is
not a graphics acceptance result.
