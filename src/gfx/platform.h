#ifndef HT2_GFX_PLATFORM_H
#define HT2_GFX_PLATFORM_H
/* The two LongRoad2 src/platform/ calls the vendored ddraw.c makes,
 * answered by src/shims/gfx_host.c on top of win32hle. */
void platform_get_screen_size(int *out_w, int *out_h);
void platform_perf_report_for_frame(double *out_wait_ms, long *out_wait_calls);
#endif
