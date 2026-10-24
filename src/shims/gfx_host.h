/*
 * gfx_host.h - the SDL2 + GLES window under src/gfx/, wired into win32hle:
 * opened on the game's first display-mode change (or primary surface), its
 * SDL events fed to win32hle's message queue through the pump hook.
 */
#ifndef HT2_GFX_HOST_H
#define HT2_GFX_HOST_H

/* Open the window at w x h (0 x 0: 640x480) if it is not open yet, and make
 * w x h the display mode the guest's mouse coordinates are in. */
void gfx_host_open(int w, int h);

#endif
