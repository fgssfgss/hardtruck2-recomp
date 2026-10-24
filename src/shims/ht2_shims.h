/*
 * ht2_shims.h - the imports king.exe needs that pcrecomp's win32hle does not
 * answer (scripts/import_coverage.py --missing lists them).
 *
 * Same ABI as win32hle (win32hle.h): a shim is `void fn(void)`, g_esp points at
 * [ret][arg0][arg1]..., args come from A32/APTR/ASTR, RET(n)/RETP pop them.
 * Name lookup is first-match and these register before win32hle's, so a name
 * here REPLACES win32hle's -- that is how DirectDraw 7 / Direct3D 7 (ported
 * from LongRoad2) take over from win32hle's DirectDraw 1-3.
 */
#ifndef HT2_SHIMS_H
#define HT2_SHIMS_H
#include "win32hle.h"
#include "com_bridge.h"


extern const win32hle_shim ht2_shims_misc[];    /* misc.c: small USER32/GDI32/KERNEL32 gaps */

void ht2_shims_register(void);

#endif
