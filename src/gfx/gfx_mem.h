#ifndef HT2_GFX_MEM_H
#define HT2_GFX_MEM_H
/*
 * Memory the guest is handed an address of (hardtruck2-recomp addition).
 *
 * LongRoad2 ran the game in-process with guest VA == host address, so a
 * calloc'd surface buffer could be returned from Lock as-is. Here the guest
 * lives in win32hle's guest memory (one 4 GB block on a 64-bit host), so
 * every buffer whose address the guest ever sees -- a surface's pixels
 * (Lock / GetSurfaceDesc lpSurface), a vertex buffer's storage (Lock) --
 * comes from these, and crosses to the guest as a 32-bit VA.
 *
 * Implemented in src/shims/ddraw7.c on win32hle's heap (zeroed, like calloc).
 */
#include <stddef.h>
#include <stdint.h>

void    *gfx_guest_alloc(size_t n);           /* zeroed; NULL on failure */
void     gfx_guest_free(void *p);             /* NULL is fine */
uint32_t gfx_guest_va(const void *p);         /* host pointer -> guest VA (fatal if not guest memory) */
void    *gfx_guest_ptr(uint32_t va);          /* guest VA -> host pointer (0 -> NULL) */

/* A COM object handed out to the guest (surface, palette, ddraw tear-off,
 * device, ...) is about to be freed: the bridge drops its guest-side proxy,
 * so the freed host address can never be looked up through a stale one.
 * Called from every Release that frees. */
void     gfx_native_freed(const void *native);


#endif
