#ifndef HT2_GFX_SHIM_COMMON_H
#define HT2_GFX_SHIM_COMMON_H
/*
 * Replaces LongRoad2's src/shim/shim_common.h for the vendored gfx code.
 *
 * In LongRoad2 these functions are entered straight from the game's x86 code,
 * so WINAPI meant stdcall + stack realignment. Here nothing in src/gfx/ is
 * ever entered from guest code: the guest calls the shim-ABI bridge in
 * src/shims/ddraw7.c, which calls these as plain C functions. So WINAPI is
 * empty, on 32 and 64 bits alike.
 */
#include <stddef.h>
#include <stdint.h>

#define WINAPI

typedef void (*shim_fn_t)(void);

#endif
