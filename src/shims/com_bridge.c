/*
 * com_bridge.c - proxies, guest vtables, and gfx_mem.h on win32hle's heap.
 * See com_bridge.h for the design.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "com_bridge.h"
#include "../gfx/gfx_mem.h"

/* ---------------------------------------------------------------- gfx_mem.h */
void *gfx_guest_alloc(size_t n) {
    if (n > 0xFFFFFFF0u) return NULL;
    return hle_heap_alloc((uint32_t)(n ? n : 1), 1);
}
void     gfx_guest_free(void *p)       { if (p) hle_heap_free(p); }
uint32_t gfx_guest_va(const void *p)   { return hle_va(p); }
void    *gfx_guest_ptr(uint32_t va)    { return hle_ptr(va); }

/* ---------------------------------------------------------------- interfaces */
static struct {
    const char *iface;
    char **names;            /* "iface::slot", resolved to shim VAs for the vtable */
    int n;
    uint32_t vtbl;           /* guest VA, 0 until first com_wrap */
} g_kinds[CK_COUNT];

void com_define(com_kind_t kind, const char *iface, const win32hle_shim *slots, int nslots) {
    win32hle_shim *reg = calloc((size_t)nslots + 1, sizeof *reg);
    g_kinds[kind].iface = iface;
    g_kinds[kind].names = calloc((size_t)nslots, sizeof(char *));
    g_kinds[kind].n = nslots;
    for (int i = 0; i < nslots; i++) {
        size_t len = strlen(iface) + strlen(slots[i].name) + 3;
        char *full = malloc(len);
        snprintf(full, len, "%s::%s", iface, slots[i].name);
        g_kinds[kind].names[i] = full;
        reg[i].name = full;
        reg[i].fn = slots[i].fn;
    }
    win32hle_register(reg);        /* keeps the pointers: reg and the names live for the run */
}

static uint32_t vtbl_of(com_kind_t kind) {
    if (!g_kinds[kind].vtbl) {
        int n = g_kinds[kind].n;
        if (!n) hle_fatal("[com] interface kind %d used before com_define", kind);
        uint32_t *v = hle_alloc(4u * (uint32_t)n);
        for (int i = 0; i < n; i++) {
            v[i] = hle_resolve(g_kinds[kind].names[i]);
            if (!v[i]) hle_fatal("[com] %s did not register", g_kinds[kind].names[i]);
        }
        g_kinds[kind].vtbl = hle_va(v);
    }
    return g_kinds[kind].vtbl;
}

/* ---------------------------------------------------------------- proxies
 * A proxy in guest memory is { vtbl, 'HT2C', kind }. Two open-addressing
 * tables (linear probing, tombstones) find one from the other: by proxy VA,
 * for every call's `this`, and by (host object, kind), for every wrap. */
#define PROXY_MAGIC 0x43325448u   /* 'HT2C' */
typedef struct { uint32_t proxy; int kind; const void *native; } entry_t;
#define TOMB ((uint32_t)1)        /* a deleted slot (no proxy VA is ever 1) */

static entry_t *g_by_proxy, *g_by_native;
static size_t g_cap, g_used;      /* both tables share size and fill */

static size_t h_proxy(uint32_t va)              { return (va * 2654435761u) >> 3; }
static size_t h_native(const void *p, int kind) { uintptr_t x = (uintptr_t)p ^ (uintptr_t)kind;
                                                   return (size_t)((x * 0x9E3779B97F4A7C15ull) >> 17); }

static void insert_raw(entry_t e) {
    size_t m = g_cap - 1, i;
    for (i = h_proxy(e.proxy) & m; g_by_proxy[i].proxy > TOMB; i = (i + 1) & m) {}
    g_by_proxy[i] = e;
    for (i = h_native(e.native, e.kind) & m; g_by_native[i].proxy > TOMB; i = (i + 1) & m) {}
    g_by_native[i] = e;
}
static void grow(void) {
    entry_t *op = g_by_proxy;
    size_t oc = g_cap;
    g_cap = oc ? oc * 2 : 1024;
    g_by_proxy = calloc(g_cap, sizeof(entry_t));
    g_by_native = calloc(g_cap, sizeof(entry_t));
    g_used = 0;
    for (size_t i = 0; i < oc; i++) if (op[i].proxy > TOMB) { insert_raw(op[i]); g_used++; }
    free(op);
}
static entry_t *find_proxy(uint32_t va) {
    if (!g_cap) return NULL;
    size_t m = g_cap - 1;
    for (size_t i = h_proxy(va) & m; g_by_proxy[i].proxy; i = (i + 1) & m)
        if (g_by_proxy[i].proxy == va) return &g_by_proxy[i];
    return NULL;
}
static entry_t *find_native(const void *p, int kind) {
    if (!g_cap) return NULL;
    size_t m = g_cap - 1;
    for (size_t i = h_native(p, kind) & m; g_by_native[i].proxy; i = (i + 1) & m)
        if (g_by_native[i].proxy > TOMB && g_by_native[i].native == p && g_by_native[i].kind == kind)
            return &g_by_native[i];
    return NULL;
}

uint32_t com_wrap(const void *native, com_kind_t kind) {
    if (!native) return 0;
    entry_t *e = find_native(native, kind);
    if (e) return e->proxy;
    if (2 * (g_used + 1) > g_cap) grow();
    uint32_t *px = hle_alloc(16);
    px[0] = vtbl_of(kind);
    px[1] = PROXY_MAGIC;
    px[2] = (uint32_t)kind;
    entry_t ne = { hle_va(px), kind, native };
    insert_raw(ne);
    g_used++;
    return ne.proxy;
}

void *com_unwrap(uint32_t proxy, com_kind_t kind) {
    if (!proxy) return NULL;
    entry_t *e = find_proxy(proxy);
    if (e && e->kind == (int)kind) return (void *)e->native;
    fprintf(stderr, "[com] %s: 0x%08X is not an %s (it is %s)\n", g_cur_import ? g_cur_import : "?",
            proxy, g_kinds[kind].iface ? g_kinds[kind].iface : "?",
            e ? g_kinds[e->kind].iface : "no object of ours");
    return NULL;
}

void com_out(uint32_t out_va, const void *native, com_kind_t kind) {
    uint32_t *out = hle_ptr(out_va);
    if (out) *out = com_wrap(native, kind);
}

int com_guid_is(uint32_t riid_va, const void *guid16) {
    const void *g = hle_ptr(riid_va);
    return g && memcmp(g, guid16, 16) == 0;
}

/* Every proxy of a host object that is being freed goes, in every kind: the
 * proxy block itself is left in guest memory (a guest still holding the
 * pointer then gets E_POINTER from com_unwrap, not a crash). */
void gfx_native_freed(const void *native) {
    if (!native) return;
    for (int k = 1; k < CK_COUNT; k++) {
        entry_t *n = find_native(native, k);
        if (!n) continue;
        entry_t *p = find_proxy(n->proxy);
        if (p) p->proxy = TOMB;
        n->proxy = TOMB;   /* g_used unchanged: a tombstone still fills a slot until grow() */
    }
}

/* ---------------------------------------------------------------- stub slots */
#define FAIL(n) void com_fail##n(void) { \
    fprintf(stderr, "[com] unimplemented: %s\n", g_cur_import ? g_cur_import : "?"); \
    RET(COM_DDERR_GENERIC, (n) + 1); }
#define OK(n) void com_ok##n(void) { RET(0, (n) + 1); }
FAIL(0) FAIL(1) FAIL(2) FAIL(3) FAIL(4) FAIL(5) FAIL(6) FAIL(7)
OK(0) OK(1) OK(2) OK(3) OK(4) OK(5) OK(6) OK(7)

