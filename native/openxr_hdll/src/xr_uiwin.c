// HUD element rects read from the game's own UI objects.
//
// hlpatch injects `xr_ui_windows(this)` at the start of ui.BaseUI.update. Every
// few calls we walk BaseUI.windows (an Array<ui.Window>) and record, per
// window, its class name and the on-screen rect of its root element:
//   absX/absY (top-left, scene px), calculatedWidth/Height * matA/matD.
// The VR panels use these instead of hand-measured cut-outs, so they follow the
// game at any resolution, UI scale, clan, language or selection.
//
// Fields are read straight from object memory, located through libhl's own
// per-class field tables (hl_get_obj_rt + hl_lookup_find), so a missing or
// differently typed field just reads as 0/NULL instead of raising an exception.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "xr_uiwin.h"

// Minimal HashLink runtime layouts (hl.h, x64).
typedef struct hlt { int kind; void *u; void **vobj_proto; unsigned *mark_bits; } hlt;
typedef struct hl_lookup { hlt *t; int hashed_name; int field_index; } hl_lookup;
typedef struct hl_rt {
    hlt *t;
    int nfields, nproto, size, nmethods, nbindings;
    unsigned char pad_size, largest_field, hasPtr;
    void **methods;
    int *fields_indexes;
    void *bindings;
    struct hl_rt *parent;
    void *toStringFun, *compareFun, *castFun, *getFieldFun;
    int nlookup, ninterfaces;
    hl_lookup *lookup;
    int *interfaces;
} hl_rt;
enum { K_I32 = 3, K_F64 = 6, K_BOOL = 7, K_OBJ = 11, K_ARRAY = 12 };

typedef hl_rt *(*getrt_fn)(hlt *t);
typedef hl_lookup *(*lfind_fn)(hl_lookup *l, int size, int hash);
typedef int (*hash_fn)(const char *s);

void logf_(const char *fmt, ...);

static getrt_fn p_getrt;
static lfind_fn p_lfind;
static hash_fn p_hash;
static int g_api = 0; // 0 = not tried, 1 = ok, -1 = unavailable

static int h_windows, h_length, h_array, h_root, h_visible, h_parent, h_absX, h_absY, h_matA, h_matD, h_cw, h_ch,
    h_allocated, h_children, h_tip;

static int api_init(void) {
    if (g_api) return g_api > 0;
    g_api = -1;
    HMODULE m = GetModuleHandleA("libhl.dll");
    if (!m) return 0;
    p_getrt = (getrt_fn)GetProcAddress(m, "hl_get_obj_rt");
    p_lfind = (lfind_fn)GetProcAddress(m, "hl_lookup_find");
    p_hash = (hash_fn)GetProcAddress(m, "hl_hash_utf8");
    if (!p_getrt || !p_lfind || !p_hash) {
        logf_("UI windows: libhl API missing; HUD rects from the game are off");
        return 0;
    }
    h_windows = p_hash("windows");
    h_length = p_hash("length");
    h_array = p_hash("array");
    h_root = p_hash("root");
    h_visible = p_hash("visible");
    h_parent = p_hash("parent");
    h_absX = p_hash("absX");
    h_absY = p_hash("absY");
    h_matA = p_hash("matA");
    h_matD = p_hash("matD");
    h_cw = p_hash("calculatedWidth");
    h_ch = p_hash("calculatedHeight");
    h_allocated = p_hash("allocated");
    h_children = p_hash("children");
    h_tip = p_hash("tip");
    g_api = 1;
    return 1;
}

// Field of a HashLink object (class or any parent class): byte offset and kind.
// Returns 0 if the object isn't a class instance or has no such field. Never
// raises a HashLink exception.
static int field(void *o, int hash, int kind, int *off) {
    if (!o) return 0;
    hlt *t = *(hlt **)o;
    if (!t || t->kind != K_OBJ) return 0;
    for (hl_rt *rt = p_getrt(t); rt; rt = rt->parent) {
        hl_lookup *l = p_lfind(rt->lookup, rt->nlookup, hash);
        if (l) {
            if (l->field_index <= 0 || !l->t || l->t->kind != kind) return 0;
            *off = l->field_index;
            return 1;
        }
    }
    return 0;
}
static void *getp(void *o, int h, int kind) { int off; return field(o, h, kind, &off) ? *(void **)((char *)o + off) : NULL; }
static double getd(void *o, int h) { int off; return field(o, h, K_F64, &off) ? *(double *)((char *)o + off) : 0; }
static int geti(void *o, int h) { int off; return field(o, h, K_I32, &off) ? *(int *)((char *)o + off) : 0; }
static int getb(void *o, int h) { int off; return field(o, h, K_BOOL, &off) ? *(unsigned char *)((char *)o + off) : 0; }

// HashLink object -> class name (hl_type.obj->name, UTF-16) as ASCII.
static void class_name(void *obj, char *out, int n) {
    out[0] = 0;
    if (!obj) return;
    // vdynamic { hl_type *t; ... }, hl_type { int kind; union { hl_type_obj *obj; ... } ... },
    // hl_type_obj { int nfields, nproto, nbindings; const uchar *name; ... }
    unsigned char *t = *(unsigned char **)obj;
    if (!t) return;
    unsigned char *tobj = *(unsigned char **)(t + 8);
    if (!tobj) return;
    const unsigned short *name = *(const unsigned short **)(tobj + 16);
    if (!name) return;
    int i = 0;
    for (; i < n - 1 && name[i]; i++) out[i] = name[i] < 128 ? (char)name[i] : '?';
    out[i] = 0;
}

static xrw_win g_win[XRW_MAX];
static int g_nwin;
static unsigned g_calls;
static char g_sig[2048], g_last_sig[2048];

// The element and every parent visible, and in the scene.
static int shown(void *o) {
    for (int depth = 0; o && depth < 64; depth++) {
        if (!getb(o, h_visible)) return 0;
        o = getp(o, h_parent, K_OBJ);
    }
    return 1;
}

// Union of the on-screen rects of an element and its visible descendant flows
// (some windows keep a small root and place their content in children).
typedef struct { double x0, y0, x1, y1; int any; } bounds;
static void add_flow(bounds *b, void *o) {
    int off;
    if (!field(o, h_cw, K_F64, &off)) return; // not a flow: no size of its own
    double cw = getd(o, h_cw) * getd(o, h_matA), ch = getd(o, h_ch) * getd(o, h_matD);
    if (cw <= 0 || ch <= 0) return;
    double x = getd(o, h_absX), y = getd(o, h_absY);
    if (!b->any) { b->x0 = x; b->y0 = y; b->x1 = x + cw; b->y1 = y + ch; b->any = 1; return; }
    if (x < b->x0) b->x0 = x;
    if (y < b->y0) b->y0 = y;
    if (x + cw > b->x1) b->x1 = x + cw;
    if (y + ch > b->y1) b->y1 = y + ch;
}
static void walk(bounds *b, void *o, int depth, int *budget) {
    if (!o || depth > 10 || --*budget < 0 || !getb(o, h_visible)) return;
    add_flow(b, o);
    void *arr = getp(o, h_children, K_OBJ);
    if (!arr) return;
    int len = geti(arr, h_length);
    unsigned char *va = (unsigned char *)getp(arr, h_array, K_ARRAY);
    if (!va || len <= 0) return;
    void **items = (void **)(va + 24);
    for (int i = 0; i < len; i++) walk(b, items[i], depth + 1, budget);
}
// Rect of a UI element, clipped to the screen; returns visible or not.
static int element_rect(void *root, xrw_win *e, int sw, int sh) {
    e->x = e->y = e->w = e->h = 0;
    if (!root || !getb(root, h_allocated) || !shown(root)) return 0;
    bounds b = {0};
    int budget = 600;
    walk(&b, root, 0, &budget);
    if (!b.any) return 0;
    if (sw > 0) { if (b.x0 < 0) b.x0 = 0; if (b.y0 < 0) b.y0 = 0; if (b.x1 > sw) b.x1 = sw; if (b.y1 > sh) b.y1 = sh; }
    e->x = (int)(b.x0 + 0.5);
    e->y = (int)(b.y0 + 0.5);
    e->w = (int)(b.x1 - b.x0 + 0.5);
    e->h = (int)(b.y1 - b.y0 + 0.5);
    return e->w > 2 && e->h > 2;
}

int xrt_rt_size(int *w, int *h);

__declspec(dllexport) void xr_ui_windows_impl(void *ui) {
    if (!ui || !api_init()) return;
    if (g_calls++ % 4) return; // a few times per second is plenty
    void *arr = getp(ui, h_windows, K_OBJ);
    if (!arr) { g_nwin = 0; return; }
    int len = geti(arr, h_length);
    unsigned char *va = (unsigned char *)getp(arr, h_array, K_ARRAY);
    if (!va || len <= 0) { g_nwin = 0; return; }
    void **items = (void **)(va + 24); // varray header: t, at, size, pad
    int n = 0, sw = 0, sh = 0;
    xrt_rt_size(&sw, &sh);
    g_sig[0] = 0;
    for (int i = 0; i < len && n < XRW_MAX; i++) {
        void *w = items[i];
        if (!w) continue;
        void *root = getp(w, h_root, K_OBJ);
        if (!root) continue;
        xrw_win *e = &g_win[n];
        class_name(w, e->cls, sizeof(e->cls));
        e->vis = element_rect(root, e, sw, sh);
        n++;
        if (e->vis && strlen(g_sig) + strlen(e->cls) + 32 < sizeof(g_sig)) {
            char tmp[128];
            sprintf_s(tmp, sizeof(tmp), "%s(%d,%d %dx%d) ", e->cls, e->x, e->y, e->w, e->h);
            strcat_s(g_sig, sizeof(g_sig), tmp);
        }
    }
    if (n < XRW_MAX) { // the hover tooltip (BaseUI.tip) as a pseudo window "tip"
        xrw_win *e = &g_win[n];
        strcpy_s(e->cls, sizeof(e->cls), "tip");
        e->vis = element_rect(getp(ui, h_tip, K_OBJ), e, sw, sh);
        n++;
    }
    g_nwin = n;
    // log the visible set whenever a window appears, disappears or moves/resizes
    if (strcmp(g_sig, g_last_sig)) {
        static unsigned last_log;
        if (g_calls - last_log > 40 || !last_log) { // at most every ~10 s while things animate
            logf_("UI windows: %s", g_sig[0] ? g_sig : "(none visible)");
            strcpy_s(g_last_sig, sizeof(g_last_sig), g_sig);
            last_log = g_calls;
        }
    }
}

int xrw_windows(const xrw_win **out) {
    *out = g_win;
    return g_nwin;
}
