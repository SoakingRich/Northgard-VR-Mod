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
    h_allocated, h_children, h_tip, h_x, h_y, h_posChanged;

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
    h_x = p_hash("x");
    h_y = p_hash("y");
    h_posChanged = p_hash("posChanged");
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

// The object's class or one of its parent classes is `name`
// (hl_type_obj: nfields, nproto, nbindings, name @16, super @24).
static int is_a(void *obj, const char *name) {
    if (!obj) return 0;
    unsigned char *t = *(unsigned char **)obj;
    for (int depth = 0; t && depth < 16; depth++) {
        unsigned char *tobj = *(unsigned char **)(t + 8);
        if (!tobj) return 0;
        const unsigned short *nm = *(const unsigned short **)(tobj + 16);
        int i = 0;
        while (nm && name[i] && nm[i] == (unsigned char)name[i]) i++;
        if (nm && !name[i] && !nm[i]) return 1;
        t = *(unsigned char **)(tobj + 24);
    }
    return 0;
}
// 0 nothing selected, 1 placing a building, 2 units selected, 3 something else (building, zone...)
static int g_sel_mode;
int xrw_sel_mode(void) { return g_sel_mode; }

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
typedef struct { double x0, y0, x1, y1; int any, fs, n; } bounds;
static double g_wsw, g_wsh; // screen size for the full-screen test
static int g_dbg_flows;
static int g_span_skip;            // also leave out flows spanning the full width or height (layout containers)
static const char *g_skip_cls;     // leave out this class's subtree // log every flow rect (once, for a new full-screen window)
static void add_flow(bounds *b, void *o) {
    int off;
    if (!field(o, h_cw, K_F64, &off)) return; // not a flow: no size of its own
    double cw = getd(o, h_cw) * getd(o, h_matA), ch = getd(o, h_ch) * getd(o, h_matD);
    if (cw <= 0 || ch <= 0) return;
    // a full-screen flow is a backdrop (Pause dims the whole screen): leave it out so
    // the rect is the window's actual content
    double x = getd(o, h_absX), y = getd(o, h_absY);
    if (g_dbg_flows) { char cn[64]; class_name(o, cn, sizeof(cn)); logf_("  flow %s (%.0f,%.0f %.0fx%.0f)", cn, x, y, cw, ch); }
    if (g_wsw > 0 && cw >= g_wsw * 0.9 && ch >= g_wsh * 0.9) { b->fs = 1; return; }
    if (g_span_skip && g_wsw > 0 && (cw >= g_wsw * 0.9 || ch >= g_wsh * 0.9)) return;
    b->n++;
    if (!b->any) { b->x0 = x; b->y0 = y; b->x1 = x + cw; b->y1 = y + ch; b->any = 1; return; }
    if (x < b->x0) b->x0 = x;
    if (y < b->y0) b->y0 = y;
    if (x + cw > b->x1) b->x1 = x + cw;
    if (y + ch > b->y1) b->y1 = y + ch;
}
static void walk(bounds *b, void *o, int depth, int *budget) {
    if (!o || depth > 10 || --*budget < 0 || !getb(o, h_visible)) return;
    if (g_skip_cls) { char cn[64]; class_name(o, cn, sizeof(cn)); if (!strcmp(cn, g_skip_cls)) return; }
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
    e->x = e->y = e->w = e->h = 0; e->fs = 0;
    if (!root || !getb(root, h_allocated) || !shown(root)) return 0;
    bounds b = {0};
    int budget = 600;
    g_wsw = sw; g_wsh = sh;
    walk(&b, root, 0, &budget);
    e->fs = b.fs;
    e->nflows = b.n;
    if (!b.any) return 0;
    if (sw > 0) { if (b.x0 < 0) b.x0 = 0; if (b.y0 < 0) b.y0 = 0; if (b.x1 > sw) b.x1 = sw; if (b.y1 > sh) b.y1 = sh; }
    e->x = (int)(b.x0 + 0.5);
    e->y = (int)(b.y0 + 0.5);
    e->w = (int)(b.x1 - b.x0 + 0.5);
    e->h = (int)(b.y1 - b.y0 + 0.5);
    return e->w > 2 && e->h > 2;
}

int xrt_rt_size(int *w, int *h);
int xrt_tip_on_hand(void); // xr_theater.c: the laser is on a left-hand panel

// Move an h2d.Object (x, y relative to its parent; posChanged makes it re-sync).
static void set_pos(void *o, double x, double y) {
    int ox, oy, op;
    if (!field(o, h_x, K_F64, &ox) || !field(o, h_y, K_F64, &oy)) return;
    *(double *)((char *)o + ox) = x;
    *(double *)((char *)o + oy) = y;
    if (field(o, h_posChanged, K_BOOL, &op)) *((unsigned char *)o + op) = 1;
}

static void *g_last_ui;
// Called just before the UI pass renders (xr_theater.c, xrt_ui_target): the game has
// placed its tooltip next to the mouse by now, i.e. over the hand panel's own crop
// of the UI image (a second copy inside the panel). Move it to the middle of the
// screen, where no panel crops; only the VR tooltip quad shows it.
void xrw_tip_relocate(void) {
    if (!g_last_ui || !xrt_tip_on_hand()) return;
    int tw = 0, th = 0;
    xrt_rt_size(&tw, &th);
    void *tip = getp(g_last_ui, h_tip, K_OBJ);
    if (tip && tw > 0) set_pos(tip, tw * 0.45, th * 0.45);
}

// Diagnostics: log every visible flow under o containing UI point (px,py), with its path.
static void find_at(void *o, int depth, double px, double py, char *path, int *budget) {
    if (!o || depth > 24 || --*budget < 0 || !getb(o, h_visible)) return;
    char cn[64]; class_name(o, cn, sizeof(cn));
    size_t pl = strlen(path);
    if (pl + strlen(cn) + 3 < 1024) { strcat_s(path, 1024, "/"); strcat_s(path, 1024, cn); }
    {   // any object whose origin lies within 160 px left/40 px above of the point
        double x = getd(o, h_absX), y = getd(o, h_absY);
        if (x >= px - 160 && x <= px + 10 && y >= py - 40 && y <= py + 10) logf_("  near-point: %s (%.0f,%.0f) %p", path, x, y, o);
    }
    void *arr = getp(o, h_children, K_OBJ);
    if (arr) {
        int len = geti(arr, h_length);
        unsigned char *va = (unsigned char *)getp(arr, h_array, K_ARRAY);
        if (va) for (int i = 0; i < len; i++) find_at(((void **)(va + 24))[i], depth + 1, px, py, path, budget);
    }
    path[pl] = 0;
}

int xrt_debug(void);
__declspec(dllexport) void xr_ui_windows_impl(void *ui) {
    if (!ui || !api_init()) return;
    g_last_ui = ui;
    { // NorthgardVR_find.txt "x y": log the UI objects at that point
        static unsigned k;
        if (xrt_debug() && ++k % 20 == 0) {
            char f[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, f, MAX_PATH);
            while (n > 0 && f[n - 1] != '\\') n--;
            f[n] = 0; strcat_s(f, MAX_PATH, "NorthgardVR_find.txt");
            FILE *fp = NULL;
            if (!fopen_s(&fp, f, "rb") && fp) {
                double px = 0, py = 0; fscanf(fp, "%lf %lf", &px, &py); fclose(fp); DeleteFileA(f);
                static int h_r; if (!h_r) h_r = p_hash("root");
                char path[1024] = ""; int budget = 20000;
                logf_("find at %.0f,%.0f:", px, py);
                void *top = getp(ui, h_r, K_OBJ);
                for (int u = 0; u < 8 && top && getp(top, h_parent, K_OBJ); u++) top = getp(top, h_parent, K_OBJ);
                char tn[64]; class_name(top, tn, sizeof(tn)); logf_("search from %s", tn);
                budget = 60000;
                find_at(top, 0, px, py, path, &budget);
                logf_("budget left %d", budget);
                { static int h_c2; if (!h_c2) h_c2 = p_hash("cursor"); void *cu = getp(ui, h_c2, K_OBJ); char cn2[64]; class_name(cu, cn2, sizeof(cn2));
                  logf_("UI.cursor = %s %p at (%.0f,%.0f) visible %d allocated %d", cn2, cu, getd(cu, h_absX), getd(cu, h_absY), getb(cu, h_visible), getb(cu, h_allocated));
                  budget = 2000; path[0] = 0; find_at(cu, 0, getd(cu, h_absX) + 150, getd(cu, h_absY) + 30, path, &budget); }
                logf_("find done");
            }
        }
    }
    { // building placement (diagnostics): where the game puts the preview vs the laser
        static int h_lsp, h_obj, h_z, h_cz; static void *last; static unsigned cnt;
        if (!h_lsp) { h_lsp = p_hash("currentSel"); h_obj = p_hash("drag"); h_cz = p_hash("flags"); h_z = p_hash("z"); }
        void *sp = getp(ui, h_lsp, K_OBJ);
        if (sp != last && xrt_debug()) { (void)cnt;
            void *o = getp(sp, h_obj, K_OBJ);
            double gx, gy; void xr_ptr_ground(double *, double *); xr_ptr_ground(&gx, &gy);
            char cn[96]; class_name(sp, cn, sizeof(cn));
            logf_("place: currentSel %s %p pos (%.1f %.1f %.1f) drag %p at (%.1f %.1f %.1f) flags %x; laser ground (%.1f %.1f)", cn, sp,
                  getd(sp, h_x), getd(sp, h_y), getd(sp, h_z), o, getd(o, h_x), getd(o, h_y), getd(o, h_z), geti(o, h_cz), gx, gy);
            last = sp;
        }
        (void)h_cz;
    }
    { // what the current selection is (button mapping in openxr_hdll.c)
        static int h_cs; if (!h_cs) h_cs = p_hash("currentSel");
        void *sel = getp(ui, h_cs, K_OBJ);
        char cn[96]; class_name(sel, cn, sizeof(cn));
        g_sel_mode = !sel ? 0 : !strcmp(cn, "ent.selecter.SelectPlace") ? 1 : (!strcmp(cn, "ent.selecter.SelectGroup") || is_a(sel, "ent.Unit")) ? 2 : 3;
        static int last = -1; static char lastc[96];
        if (g_sel_mode != last || strcmp(cn, lastc)) {
            logf_("selection: %s (%s)", sel ? cn : "none", (const char *[]){"nothing", "build preview", "units", "other"}[g_sel_mode]);
            last = g_sel_mode; strcpy_s(lastc, sizeof(lastc), cn);
        }
    }
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
        static int dbgn;
        static int dbgb;
        g_dbg_flows = xrt_debug() && ((strstr(e->cls, "Pause") && dbgn < 1) || (!strcmp(e->cls, "ui.win.BuildWindow") && dbgb < 1));
        e->vis = element_rect(root, e, sw, sh);
        if (g_dbg_flows && e->vis) { if (strstr(e->cls, "Pause")) dbgn++; else dbgb++; logf_("(flows above: %s)", e->cls); }
        g_dbg_flows = 0;
        n++;
        if (e->vis && strlen(g_sig) + strlen(e->cls) + 32 < sizeof(g_sig)) {
            char tmp[128];
            sprintf_s(tmp, sizeof(tmp), "%s(%d,%d %dx%d) ", e->cls, e->x, e->y, e->w, e->h);
            strcat_s(g_sig, sizeof(g_sig), tmp);
        }
    }
    if (n + 1 < XRW_MAX) { // the pause/escape menu (UI.escMenu, not a ui.Window): pseudo windows
        // "escnav" (the button column) and "escpage" (Options, Save/Load... pages, without the
        // full-width/height layout containers around them)
        static int h_esc, h_content, h_nav;
        if (!h_esc) { h_esc = p_hash("escMenu"); h_content = p_hash("content"); h_nav = p_hash("navigation"); }
        void *em = getp(ui, h_esc, K_OBJ);
        void *ec = em ? getp(em, h_content, K_OBJ) : NULL, *en = em ? getp(em, h_nav, K_OBJ) : NULL;
        xrw_win *e = &g_win[n++];
        strcpy_s(e->cls, sizeof(e->cls), "escnav");
        e->vis = element_rect(en, e, sw, sh);
        e = &g_win[n++];
        strcpy_s(e->cls, sizeof(e->cls), "escpage");
        g_span_skip = 1; g_skip_cls = "ui.menus.SubMenu";
        static int lastw, lasth, dbgp;
        e->vis = element_rect(ec, e, sw, sh);
        if (e->vis && (e->w != lastw || e->h != lasth) && dbgp < 4 && xrt_debug()) { // log the flows of each new page layout
            dbgp++; g_dbg_flows = 1; element_rect(ec, e, sw, sh); g_dbg_flows = 0;
            logf_("(flows above: escpage %d,%d %dx%d; escnav %d,%d %dx%d)", e->x, e->y, e->w, e->h,
                  g_win[n - 2].x, g_win[n - 2].y, g_win[n - 2].w, g_win[n - 2].h);
        }
        if (e->vis) { lastw = e->w; lasth = e->h; }
        g_span_skip = 0; g_skip_cls = NULL;
    }
    if (n < XRW_MAX) { // the label next to the mouse cursor over the map (UI.cursor: costs to
        // convert a villager, "can't build here"...) as a pseudo window "cursor"
        static int h_cursor;
        if (!h_cursor) h_cursor = p_hash("cursor");
        xrw_win *e = &g_win[n++];
        strcpy_s(e->cls, sizeof(e->cls), "cursor");
        // The cost/info label shown next to the mouse over the map (e.g. "1 Warrior 30" when villagers
        // are selected and the mouse is on a training building) is an h2d.ObjectFollower pinned to
        // the building, holding an h2d.ScaleGrid box. Shown in VR as a panel above the laser.
        // Only while UI.cursor is up (the game's hover-cursor state), so other followers stay out.
        void *cur = getp(ui, h_cursor, K_OBJ);
        e->vis = 0;
        if (cur && getb(cur, h_visible)) {
            static int h_w, h_h, h_r2;
            if (!h_w) { h_w = p_hash("width"); h_h = p_hash("height"); h_r2 = p_hash("root"); }
            void *top = getp(ui, h_r2, K_OBJ);
            for (int u = 0; u < 8 && top && getp(top, h_parent, K_OBJ); u++) top = getp(top, h_parent, K_OBJ);
            bounds b = {0};
            // iterative walk: stack of (object, inside-follower)
            void *stk[512]; unsigned char inf[512]; int sp = 0, budget = 6000;
            if (top) { stk[sp] = top; inf[sp++] = 0; }
            while (sp > 0 && --budget > 0) {
                void *o = stk[--sp]; int in = inf[sp];
                if (!o || !getb(o, h_visible)) continue;
                char cn[64]; class_name(o, cn, sizeof(cn));
                if (!strcmp(cn, "h2d.ObjectFollower")) in = 1;
                if (in && !strcmp(cn, "h2d.ScaleGrid")) {
                    double w = getd(o, h_w) * getd(o, h_matA), hh = getd(o, h_h) * getd(o, h_matD), x = getd(o, h_absX), y = getd(o, h_absY);
                    if (w > 4 && hh > 4 && w < 800 && hh < 400) {
                        if (!b.any) { b.x0 = x; b.y0 = y; b.x1 = x + w; b.y1 = y + hh; b.any = 1; }
                        else { if (x < b.x0) b.x0 = x; if (y < b.y0) b.y0 = y; if (x + w > b.x1) b.x1 = x + w; if (y + hh > b.y1) b.y1 = y + hh; }
                    }
                    continue;
                }
                void *arr = getp(o, h_children, K_OBJ);
                if (!arr) continue;
                int len = geti(arr, h_length);
                unsigned char *va = (unsigned char *)getp(arr, h_array, K_ARRAY);
                if (!va) continue;
                for (int i = 0; i < len && sp < 512; i++) { stk[sp] = ((void **)(va + 24))[i]; inf[sp++] = (unsigned char)in; }
            }
            if (b.any) {
                e->x = (int)b.x0; e->y = (int)b.y0; e->w = (int)(b.x1 - b.x0 + 0.5); e->h = (int)(b.y1 - b.y0 + 0.5);
                e->vis = e->w > 4 && e->h > 4;
            }
            static int lastvis = -1;
            if (e->vis != lastvis) { lastvis = e->vis; logf_("cursor label %s (%d,%d %dx%d)", e->vis ? "shown" : "hidden", e->x, e->y, e->w, e->h); }
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

// ------------------------------------------------------------ game events
// hlpatch passes every GameEvent (EventManager.dispatchEvent) here. We turn it
// into "Name" + its parameters as text, log each event kind the first few
// times (so the available events can be discovered), and hand it to the VR
// notification rules (xr_theater.c).
typedef struct { const unsigned short *name; int nparams; hlt **params; int size; unsigned char hasptr; int *offsets; } hl_econ;
typedef struct { const unsigned short *name; int nconstructs; hl_econ *constructs; void **global_value; } hl_tenum;
enum { K_ENUM = 18, K_NULL = 19, K_BYTES = 8, K_F32 = 5, K_I64 = 4, K_UI8 = 1, K_UI16 = 2, K_DYN = 9 };
static int h_name_, h_id_, h_kind_;

static void u16cpy(char *out, int n, const unsigned short *s) {
    int i = 0;
    if (s) for (; i < n - 1 && s[i]; i++) out[i] = s[i] < 128 ? (char)s[i] : '?';
    out[i] = 0;
}
static void hl_string(void *str, char *out, int n) { // String object: { t, bytes(uchar*), length }
    out[0] = 0;
    if (!str) return;
    const unsigned short *b = *(const unsigned short **)((char *)str + 8);
    int len = *(int *)((char *)str + 16);
    int i = 0;
    for (; b && i < len && i < n - 1; i++) out[i] = b[i] < 128 ? (char)b[i] : '?';
    out[i] = 0;
}
static int is_string(void *o) {
    char c[32];
    class_name(o, c, sizeof(c));
    return !strcmp(c, "String");
}
static void enum_name(void *e, char *out, int n) {
    out[0] = 0;
    if (!e) return;
    hlt *t = *(hlt **)e;
    if (!t || t->kind != K_ENUM) return;
    hl_tenum *te = (hl_tenum *)t->u;
    int idx = *(int *)((char *)e + 8);
    if (te && idx >= 0 && idx < te->nconstructs) u16cpy(out, n, te->constructs[idx].name);
}
// Game mode (Game.inst.state.gameType, reached through UI.game): logged on
// change and handed to xr_theater.c (multiplayer warning, Ranked block).
void xrt_game_mode(const char *mode); // xr_theater.c
// Terrain height (game units) at world x,y: map.World.getZ, nearest sample
// (zMap int per cell, height in bits 8..15, 0..255 -> 0..24 units).
static void *g_world;
int xrw_terrain_z(double x, double y, double *z) {
    static int h_zmap, h_data, h_inv, h_gw, h_len, h_bytes;
    if (!g_world || !api_init()) return 0;
    if (!h_zmap) { h_zmap = p_hash("zMap"); h_data = p_hash("data"); h_inv = p_hash("invPixelSize"); h_gw = p_hash("genWidth"); h_len = p_hash("length"); h_bytes = p_hash("bytes"); }
    void *zm = getp(g_world, h_zmap, K_OBJ), *data = getp(g_world, h_data, K_OBJ);
    if (!zm || !data) return 0;
    double inv = getd(data, h_inv);
    int gw = geti(g_world, h_gw), len = geti(zm, h_len);
    int *mem = (int *)getp(zm, h_bytes, K_BYTES);
    if (!mem || gw < 4 || inv <= 0) return 0;
    double fx = x * inv, fy = y * inv;
    fx = fx < 1 ? 1 : fx > gw - 2 ? gw - 2 : fx;
    fy = fy < 1 ? 1 : fy > gw - 2 ? gw - 2 : fy;
    int i = (int)(fx + 0.5) + (int)(fy + 0.5) * gw;
    if (i < 0 || i >= len) return 0;
    *z = ((mem[i] >> 8) & 255) / 255.0 * 24.0;
    return 1;
}
__declspec(dllexport) void xr_game_impl(void *game) {
    { static int h_w; if (!h_w && api_init()) h_w = p_hash("world"); if (h_w) g_world = getp(game, h_w, K_OBJ); }
    static unsigned k;
    if (!game || !api_init() || k++ % 30) return; // twice a second is plenty
    static int h_state, h_gt;
    static char last[64] = "?";
    if (!h_state) { h_state = p_hash("state"); h_gt = p_hash("gameType"); }
    void *st = getp(game, h_state, K_OBJ);
    char name[64] = "";
    void *ev = st ? getp(st, h_gt, K_ENUM) : NULL;
    if (ev) enum_name(ev, name, sizeof(name));
    static int listed;
    if (ev && !listed) { // all game modes, once (names for the multiplayer / Ranked rules)
        listed = 1;
        hl_tenum *te = (hl_tenum *)(*(hlt **)ev)->u;
        char all[1024] = "", one[48];
        for (int i = 0; te && i < te->nconstructs; i++) {
            u16cpy(one, sizeof(one), te->constructs[i].name);
            if (strlen(all) + strlen(one) + 2 < sizeof(all)) { strcat_s(all, sizeof(all), one); strcat_s(all, sizeof(all), " "); }
        }
        logf_("game modes: %s", all);
    }
    static int last_st = -1;
    static void *last_ev;
    { // debug: NorthgardVR_mode.txt overrides the mode name (Standard / Ranked / empty), to test the rules
        int xrt_debug(void);
        static char fake[32]; static int have; static unsigned kk;
        if (xrt_debug() && kk++ % 4 == 0) {
            char f[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, f, MAX_PATH);
            while (n > 0 && f[n - 1] != '\\') n--;
            f[n] = 0; strcat_s(f, MAX_PATH, "NorthgardVR_mode.txt");
            FILE *fp = NULL;
            if (!fopen_s(&fp, f, "rb") && fp) { have = 1; fake[0] = 0; fscanf(fp, "%31s", fake); fclose(fp); }
            else have = 0;
        }
        if (have) strcpy_s(name, sizeof(name), fake);
    }
    if (strcmp(name, last) || (st != NULL) != last_st || ev != last_ev) {
        last_ev = ev;
        last_st = st != NULL;

        int off; int has = st && field(st, h_gt, K_ENUM, &off);
        logf_("game mode: '%s'%s", name[0] ? name : "single player", st && !has ? " (gameType field missing)" : "");
        strcpy_s(last, sizeof(last), name);
        xrt_game_mode(name);
    }
}

// One parameter as short text.
static void param_text(hlt *pt, void *slot, char *out, int n) {
    out[0] = 0;
    if (!pt) return;
    int k = pt->kind;
    if (k == K_NULL) { // nullable: boxed value
        void *box = *(void **)slot;
        if (!box) { strcpy_s(out, n, "null"); return; }
        hlt *inner = (hlt *)pt->u;
        param_text(inner, (char *)box + 8, out, n);
        return;
    }
    switch (k) {
    case K_I32: sprintf_s(out, n, "%d", *(int *)slot); return;
    case K_UI8: sprintf_s(out, n, "%d", *(unsigned char *)slot); return;
    case K_UI16: sprintf_s(out, n, "%d", *(unsigned short *)slot); return;
    case K_I64: sprintf_s(out, n, "%lld", *(long long *)slot); return;
    case K_F64: sprintf_s(out, n, "%g", *(double *)slot); return;
    case K_F32: sprintf_s(out, n, "%g", *(float *)slot); return;
    case K_BOOL: strcpy_s(out, n, *(unsigned char *)slot ? "true" : "false"); return;
    case K_ENUM: enum_name(*(void **)slot, out, n); return;
    case K_OBJ: {
        void *o = *(void **)slot;
        if (!o) { strcpy_s(out, n, "null"); return; }
        if (is_string(o)) { hl_string(o, out, n); return; }
        char cls[64], extra[96] = "";
        class_name(o, cls, sizeof(cls));
        const char *shortc = strrchr(cls, '.') ? strrchr(cls, '.') + 1 : cls;
        int off;
        if (field(o, h_name_, K_OBJ, &off)) { char t_[80]; hl_string(*(void **)((char *)o + off), t_, sizeof(t_)); if (t_[0]) sprintf_s(extra, sizeof(extra), " %s", t_); }
        if (!extra[0] && field(o, h_kind_, K_ENUM, &off)) { char t_[80]; enum_name(*(void **)((char *)o + off), t_, sizeof(t_)); if (t_[0]) sprintf_s(extra, sizeof(extra), " %s", t_); }
        if (!extra[0] && field(o, h_kind_, K_OBJ, &off)) { char t_[80]; hl_string(*(void **)((char *)o + off), t_, sizeof(t_)); if (t_[0]) sprintf_s(extra, sizeof(extra), " %s", t_); }
        if (field(o, h_id_, K_I32, &off)) sprintf_s(out, n, "%s#%d%s", shortc, *(int *)((char *)o + off), extra);
        else sprintf_s(out, n, "%s%s", shortc, extra);
        return;
    }
    default: strcpy_s(out, n, "?"); return;
    }
}

void vr_game_event(const char *name, char params[][96], int np); // xr_theater.c

__declspec(dllexport) void xr_game_event_impl(void *ev) {
    if (!ev || !api_init()) return;
    if (!h_name_) { h_name_ = p_hash("name"); h_id_ = p_hash("id"); h_kind_ = p_hash("kind"); }
    hlt *t = *(hlt **)ev;
    if (!t || t->kind != K_ENUM) return;
    hl_tenum *te = (hl_tenum *)t->u;
    int idx = *(int *)((char *)ev + 8);
    if (!te || idx < 0 || idx >= te->nconstructs) return;
    hl_econ *c = &te->constructs[idx];
    char name[64];
    u16cpy(name, sizeof(name), c->name);
    static char params[6][96];
    int np = c->nparams < 6 ? c->nparams : 6;
    for (int i = 0; i < np; i++) param_text(c->params[i], (char *)ev + c->offsets[i], params[i], 96);
    vr_game_event(name, params, np);
}

// ------------------------------------------------------------ weather
// Called after every map.Weather.update: hide or show the weather particles'
// root object (VR options menu: "Weather particles").
int xrt_weather_on(void); // xr_theater.c
__declspec(dllexport) void xr_weather_impl(void *w) {
    if (!w || !api_init()) return;
    static int h_wroot, h_wvis;
    if (!h_wroot) { h_wroot = p_hash("root"); h_wvis = p_hash("visible"); }
    void *root = getp(w, h_wroot, K_OBJ);
    int off;
    if (root && field(root, h_wvis, K_BOOL, &off)) {
        unsigned char want = (unsigned char)(xrt_weather_on() != 0);
        static int logged;
        if (!logged) { logged = 1; logf_("weather particles %s in VR", want ? "shown" : "hidden"); }
        *((unsigned char *)root + off) = want;
    }
}

// ------------------------------------------------------------ game errors
// End of ui.ExceptionReportEnvironment's constructor (the game's "a problem occurred"
// screen): write the exception text and stack to our log, so a report has the cause.
void xrt_game_error(void); // xr_theater.c
__declspec(dllexport) void xr_game_error_impl(void *env) {
    if (!env || !api_init()) return;
    static int h_msg, h_stack;
    if (!h_msg) { h_msg = p_hash("message"); h_stack = p_hash("stack"); }
    static char buf[8192];
    hl_string(getp(env, h_msg, K_OBJ), buf, sizeof(buf));
    logf_("GAME ERROR: %s", buf[0] ? buf : "(no message)");
    hl_string(getp(env, h_stack, K_OBJ), buf, sizeof(buf));
    for (char *line = buf, *next; line && *line; line = next) { // one log line per stack line
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (*line && *line != '\r') logf_("GAME ERROR:   %s", line);
    }
    xrt_game_error();
}

// Debug tooling: NorthgardVR_throw.flag in the game folder makes the next Game.onUpdate
// (or the next UI update, in menus) throw a test exception, to exercise the game's error
// screen in VR.
typedef void (*throw_fn)(void *);
typedef void *(*strbytes_fn)(const unsigned short *, ...);
void xr_debug_throw_check(void) {
    if (!xrt_debug()) return;
    static unsigned k;
    if (++k % 30) return;
    char f[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, f, MAX_PATH);
    while (n > 0 && f[n - 1] != '\\') n--;
    f[n] = 0; strcat_s(f, MAX_PATH, "NorthgardVR_throw.flag");
    if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(f);
    HMODULE m = GetModuleHandleA("libhl.dll");
    throw_fn thr = m ? (throw_fn)GetProcAddress(m, "hl_throw") : NULL;
    strbytes_fn sb = m ? (strbytes_fn)GetProcAddress(m, "hl_alloc_strbytes") : NULL;
    if (!thr || !sb) { logf_("debug: test exception unavailable"); return; }
    logf_("debug: throwing a test exception");
    static const unsigned short msg[] = {'N','o','r','t','h','g','a','r','d','V','R',' ','t','e','s','t',' ','e','x','c','e','p','t','i','o','n',0};
    thr(sb(msg));
}
