#ifndef HT2_GFX_KERNEL32_H
#define HT2_GFX_KERNEL32_H
/* LongRoad2's kernel32 perf counters, folded into ddraw.c's per-frame trace
 * line. win32hle keeps no such counters: src/shims/gfx_host.c reports zeros. */
void kernel32_perf_report_for_frame(double *out_cs_wait_ms, long *out_cs_calls, long *out_cs_contended,
                                    double *out_wfso_wait_ms, long *out_wfso_calls, long *out_wfso_contended);
#endif
