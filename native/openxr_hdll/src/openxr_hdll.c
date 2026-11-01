// openxr.hdll: HashLink native library for the Northgard VR mod.
//
// Phase 0b: xr_smoke() is called once per frame from hxd.System.mainLoop.
// Phase 0c: desktop camera-freedom test. The patched bytecode reports the
//   game camera (xr_main_cam, called from Game.render) and wraps
//   h3d.Camera.update: the prologue asks xr_cam_active(); if on, bytecode
//   passes pos/target to xr_cam_compute() and writes back xr_out_*; before
//   returning it restores the game's own pos from xr_saved_*. All field
//   access is in bytecode; gameplay state is never altered.
//
// Hotkeys (global in this test build):
//   F5 / F6       VR world scale smaller / larger (game units per metre)
//   F7            toggle stereo / flat floating screen
//   F8            toggle the floating UI panel (VR)
//   F9            toggle camera override
//   F10           toggle auto-orbit (slow continuous yaw)
//   Numpad 4/6    yaw  -/+          Numpad 8/2  pitch up/down
//   Numpad +/-    zoom in/out       Numpad 5    reset offsets
//
// Writes NorthgardVR_log.txt next to Northgard.exe (shared read, so it can be
// inspected while the game runs).

// windows.h must come before hl.h (hl.h defines _GUID as a signature macro).
#include <windows.h>
#define HL_NAME(n) n
#include <hl.h>
#include <math.h>
#include <tlhelp32.h>
#include "xr_theater.h"
#include "xr_uiwin.h"
#include <share.h>
#include <stdarg.h>

// ---------------------------------------------------------------- logging
static FILE *g_log;

static void log_open(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') n--;
    path[n] = 0;
    strcat_s(path, MAX_PATH, "NorthgardVR_log.txt");
    g_log = _fsopen(path, "w", _SH_DENYNO);
}

void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static void log_openxr_runtime(void) {
    char buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                     RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS)
        logf_("Active OpenXR runtime: %s", buf);
    else
        logf_("No active OpenXR runtime found in registry");
}

// ------------------------------------------- HashLink runtime (from libhl.dll)
// Only hl_add_root is needed (to keep the registered camera pointer valid).
// All field access is done in bytecode by the patch: the runtime's by-name
// lookups (hl_dyn_get*) throw on some fields with this HashLink build.
typedef void (*root_fn)(void *);
static root_fn p_add_root;
static int g_hl_ok;

static void hl_api_init(void) {
    HMODULE m = GetModuleHandleA("libhl.dll");
    if (!m) { logf_("ERROR: libhl.dll not found in process"); return; }
    p_add_root = (root_fn)GetProcAddress(m, "hl_add_root");
    if (!p_add_root) { logf_("ERROR: hl_add_root not exported"); return; }
    g_hl_ok = 1;
}

typedef struct { double x, y, z; } v3;


// ---------------------------------------------------------- camera test
static int g_override, g_auto_orbit;
static double g_yaw, g_pitch, g_zoom = 1.0; // offsets applied to the game camera
static double g_world_scale = 60.0;           // VR: game units per real metre (F5/F6, sticks, two-hand)

static const double DEG = 3.14159265358979323846 / 180.0;

static int game_focused(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static int g_dump_request;
static int pressed(int vk) { // edge-triggered
    static SHORT prev[256];
    SHORT st = GetAsyncKeyState(vk);
    SHORT now = st & 0x8000;
    int edge = (now && !prev[vk]) || (st & 1); // bit 0: pressed since last query (catches quick taps)
    prev[vk] = now;
    return edge;
}
static int held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void log_offsets(const char *why) {
    logf_("camtest %s: override=%d auto=%d yaw=%+.1f pitch=%+.1f zoom=%.2f", why, g_override,
          g_auto_orbit, g_yaw / DEG, g_pitch / DEG, g_zoom);
}

static void poll_keys(double dt) {
    // Test build: hotkeys work regardless of focus (remote-control tools keep
    // a different window in the foreground). The VR build will use controllers.
    int focused = 1;
    (void)game_focused;
    if (pressed(VK_F9) && focused) { g_override = !g_override; log_offsets("F9"); }
    if (pressed(VK_F10) && focused) { g_auto_orbit = !g_auto_orbit; log_offsets("F10"); }
    if (pressed(VK_F5) && focused) { g_world_scale *= 0.8; logf_("world scale %.2f units/m", g_world_scale); }
    if (pressed(VK_F6) && focused) { g_world_scale *= 1.25; logf_("world scale %.2f units/m", g_world_scale); }
    if (pressed(VK_F7) && focused) logf_("stereo %s", xr_stereo_toggle() ? "on" : "off (flat screen)");
    if (pressed(VK_F8) && focused) logf_("UI panel %s", xr_ui_toggle() ? "on" : "off");
    if (pressed(VK_F11) && focused) { g_dump_request = 1; logf_("F11: stack dump requested"); }
    if (pressed(VK_NUMPAD5) && focused) { g_yaw = g_pitch = 0; g_zoom = 1; log_offsets("reset"); }
    if (!focused) return;
    double rot = 60.0 * DEG * dt; // 60 deg/s
    int changed = 0;
    if (held(VK_NUMPAD4)) { g_yaw -= rot; changed = 1; }
    if (held(VK_NUMPAD6)) { g_yaw += rot; changed = 1; }
    if (held(VK_NUMPAD8)) { g_pitch += rot; changed = 1; }
    if (held(VK_NUMPAD2)) { g_pitch -= rot; changed = 1; }
    if (held(VK_ADD)) { g_zoom *= pow(0.5, dt); changed = 1; }
    if (held(VK_SUBTRACT)) { g_zoom *= pow(2.0, dt); changed = 1; }
    if (g_auto_orbit) g_yaw += 20.0 * DEG * dt;
    static double since_log;
    since_log += dt;
    if (changed && since_log > 0.5) { log_offsets("keys"); since_log = 0; }
}

// ------------------------------------------------------ camera override
// The patched bytecode only calls these for the game's main camera
// (Game.s3d.camera, identified in bytecode), so no identity checks here.
static long long g_frames;
static long g_n_cam;           // main-camera updates since last stats line
static v3 g_last_pos, g_last_tgt;

// ------------------------------------------------------------ stack dump
// Scans the native stack for return addresses inside HashLink JIT code and
// resolves them with hl_resolve_symbol (from libhl) to "Class.method(file:line)".
typedef uchar *(*resolve_fn)(void *addr, uchar *out, int *outSize);

static void dump_hl_stack(const char *why) {
    resolve_fn resolve = (resolve_fn)GetProcAddress(GetModuleHandleA("libhl.dll"), "hl_resolve_symbol");
    if (!resolve) { logf_("stack dump: hl_resolve_symbol missing"); return; }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void **sp = (void **)&tib; // approximately the current stack pointer
    void **top = (void **)tib->StackBase;
    logf_("stack dump (%s):", why);
    int shown = 0;
    void *last = NULL;
    for (void **p = sp; p < top && shown < 60; p++) {
        void *a = *p;
        if (!a || a == last) continue;
        uchar buf[512];
        int size = 511;
        uchar *r = resolve(a, buf, &size);
        if (!r) continue;
        char out[512];
        int n = 0;
        for (int i = 0; i < size && r[i] && n < 510; i++) out[n++] = r[i] < 128 ? (char)r[i] : '?';
        out[n] = 0;
        logf_("  #%d %s", shown++, out);
        last = a;
    }
}

// Diagnostics: entry counters for Game.onUpdate, Game.render,
// CameraController.update, Scene.render.
static long g_ping[4];
HL_PRIM void HL_NAME(xr_ping_0)(void) { g_ping[0]++; }
HL_PRIM void HL_NAME(xr_ping_1)(void) { g_ping[1]++; }
HL_PRIM void HL_NAME(xr_ping_2)(void) { g_ping[2]++; }
HL_PRIM void HL_NAME(xr_ping_3)(void) {
    g_ping[3]++;
    if (g_dump_request) { g_dump_request = 0; dump_hl_stack("F11 at Scene.render"); }
}

// ------------------------------------------------- camera hook (Phase 0c/2)
// The patched h3d.Camera.update calls these for the main camera and the render
// camera only. Values are exchanged in this fixed order:
//   pos.xyz, target.xyz, up.xyz, fovY, screenRatio, zoom, viewX, viewY
enum { NV = 14 };
static double g_cam_in[NV], g_cam_out[NV];
static int g_out_i, g_saved_i, g_cam_pending;

static long long g_cam_frame = -1000; // last frame the map camera was updated

// Stereo only makes sense while the game's map camera is live (not in menus).
int xr_game_camera_live(void) { return g_frames - g_cam_frame < 10; }

// Pointer (controller ray) state, set by dio_frame.
static int g_ptr_active;
static float g_ptr_pos[3], g_ptr_quat[4];
static int g_mark_main, g_cur_is_main;

// Table for the eye mask (xr_theater.c): x, y, z, yaw, radius. 0 if not placed.
int dio_table_info(float out[5]);

// Called (by the patch) right before xr_cam_active when the camera being
// updated is the game's main camera (not the render camera).
HL_PRIM void HL_NAME(xr_mark_main)(void) { g_mark_main = 1; }

static int overview_on(void);
HL_PRIM bool HL_NAME(xr_cam_active)(void) {
    g_n_cam++;
    g_cam_frame = g_frames;
    g_cur_is_main = g_mark_main;
    g_mark_main = 0;
    float p[3], q[4], t[4];
    return g_override || xr_current_eye(p, q, t) || (g_cur_is_main && overview_on());
}

typedef struct { double x, y, z; } d3;
static d3 d3_add(d3 a, d3 b) { d3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
static d3 d3_scale(d3 a, double s) { d3 r = {a.x * s, a.y * s, a.z * s}; return r; }
// rotate v by unit quaternion (x, y, z, w)
static d3 quat_rot(const float q[4], d3 v) {
    double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    double tx = 2 * (qy * v.z - qz * v.y), ty = 2 * (qz * v.x - qx * v.z), tz = 2 * (qx * v.y - qy * v.x);
    d3 r = {v.x + qw * tx + (qy * tz - qz * ty), v.y + qw * ty + (qz * tx - qx * tz), v.z + qw * tz + (qx * ty - qy * tx)};
    return r;
}

// ------------------------------------------------------------ diorama
// The map sits on a virtual table in the play space. Table frame: origin at
// the table centre (tab_pos), rotated by tab_yaw about +Y, units in metres.
// A table-local point v maps into the game world as
//     W = C + S * M(v),   M(v) = R*v.x + U*v.y - F*v.z
// where C = game camera target + dio_off (the map point at the table centre),
// S = g_world_scale (game units per metre), F = (cos th, sin th, 0) is the
// world direction that appears "away from you" on the table, U = +Z and
// R = U x F (Heaps builds right = up x forward, so M is a reflection; the
// rendering is consistent with it).
static int dio_placed, dio_theta_set;
// Round table; the map is cut off at its rim. Left stick left/right resizes
// it (saved in NorthgardVR_settings.txt). Its near edge stays 10 cm in front
// of you, so it grows away from you.
static double g_table_r = 0.75;
static int g_table_r_loaded, g_table_r_dirty;
static double g_table_r_idle;
#define TABLE_MIN_R 0.35
#define TABLE_MAX_R 1.6
#define TABLE_AHEAD_M (g_table_r + 0.10)

static void settings_path(char *out, size_t n) {
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
    while (len > 0 && out[len - 1] != '\\') len--;
    out[len] = 0;
    strcat_s(out, n, "NorthgardVR_settings.txt");
}
static int g_open_map = 1; // 1 = no table mask: the map runs on past the table (settings "open_map 0" brings it back)
int dio_open_map(void) { return g_open_map; }
void dio_set_open_map(int on) { g_open_map = on != 0; }
static void load_settings(void) {
    char p[MAX_PATH], line[128];
    settings_path(p, sizeof(p));
    FILE *f = NULL;
    if (fopen_s(&f, p, "rb") || !f) return;
    while (fgets(line, sizeof(line), f)) {
        double v;
        int b;
        if (sscanf_s(line, "table_radius %lf", &v) == 1 && v >= TABLE_MIN_R && v <= TABLE_MAX_R) g_table_r = v;
        if (sscanf_s(line, "open_map %d", &b) == 1) g_open_map = b != 0;
    }
    fclose(f);
    logf_("settings: table radius %.2f m, open map %s", g_table_r, g_open_map ? "on (no table mask)" : "off (round table)");
}
// Rewrites table_radius and keeps every other line of the file.
static void save_settings(void) {
    char p[MAX_PATH], line[256];
    static char keep[4096];
    size_t kn = 0;
    keep[0] = 0;
    settings_path(p, sizeof(p));
    FILE *f = NULL;
    if (!fopen_s(&f, p, "rb") && f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "table_radius", 12)) continue;
            size_t l = strlen(line);
            if (kn + l + 2 >= sizeof(keep)) break;
            memcpy(keep + kn, line, l);
            kn += l;
            if (l && line[l - 1] != '\n') { keep[kn++] = '\r'; keep[kn++] = '\n'; }
            keep[kn] = 0;
        }
        fclose(f);
    }
    if (!strstr(keep, "open_map")) {
        const char *def = g_open_map ? "open_map 1\r\n" : "open_map 0\r\n";
        strcat_s(keep, sizeof(keep), def);
    }
    f = NULL;
    if (fopen_s(&f, p, "wb") || !f) return;
    fprintf(f, "table_radius %.3f\r\n%s", g_table_r, keep);
    fclose(f);
    logf_("settings saved: table radius %.2f m", g_table_r);
}
static double tab_pos[3], tab_yaw, dio_theta, dio_off[3];
static int prev_grip[2], prev_upper[2];
static double prev_hand[2][3];

static d3 dio_F(void) { d3 f = {cos(dio_theta), sin(dio_theta), 0}; return f; }
static d3 dio_M(d3 v) {
    d3 F = dio_F(), R = {-F.y, F.x, 0}, U = {0, 0, 1};
    return d3_add(d3_add(d3_scale(R, v.x), d3_scale(U, v.y)), d3_scale(F, -v.z));
}
// play space -> table local
static d3 to_table(const double p[3]) {
    double x = p[0] - tab_pos[0], y = p[1] - tab_pos[1], z = p[2] - tab_pos[2];
    double c = cos(tab_yaw), s = sin(tab_yaw);
    // rotate by -yaw about +Y
    d3 r = {c * x - s * z, y, s * x + c * z};
    return r;
}
static void quat_mul(const double a[4], const double b[4], double o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void place_table(const float head[3], const float hq[4], int floor_space) {
    d3 f = quat_rot(hq, (d3){0, 0, -1});
    double fl = sqrt(f.x * f.x + f.z * f.z);
    if (fl < 1e-3) { f.x = 0; f.z = -1; fl = 1; }
    f.x /= fl; f.z /= fl;
    tab_yaw = atan2(-f.x, -f.z);
    if (!g_table_r_loaded) { g_table_r_loaded = 1; load_settings(); }
    tab_pos[0] = head[0] + TABLE_AHEAD_M * f.x;
    tab_pos[2] = head[2] + TABLE_AHEAD_M * f.z;
    tab_pos[1] = floor_space ? 0.85 : head[1] - 0.75;
    dio_placed = 1;
    xrt_set_panel_anchor(head, (float)tab_yaw); // UI panel follows the recentre
    logf_("diorama: table placed at (%.2f %.2f %.2f) yaw %.0f deg (%s)", tab_pos[0], tab_pos[1], tab_pos[2],
          tab_yaw / DEG, floor_space ? "floor space" : "head-relative");
}

static void reset_view(void) {
    dio_off[0] = dio_off[1] = dio_off[2] = 0;
    dio_theta_set = 0;
    g_world_scale = 60.0;
    logf_("diorama: view reset");
}

// ------------------------------------------------------------ pointer
// While a finger rests on the right trigger, the right controller's aim ray
// drives the game's picking. The game's mouse is held at the window centre via
// posted mouse messages; trigger = left click, A = right click.
static HWND g_game_hwnd;
static int g_ptr_lbtn, g_ptr_rbtn, g_ptr_logged;

static BOOL CALLBACK find_game_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    char title[64];
    GetWindowTextA(w, title, sizeof(title));
    if (strcmp(title, "Northgard") != 0) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

static int g_mouse_set, g_mouse_x, g_mouse_y; // pointer position in client px
static double g_ptr_last_use = -10;            // QPC seconds when the VR pointer was last used
static LARGE_INTEGER g_freq;                    // (defined with the frame timer below)
static int g_dragging;                           // left/right button held and moved past the click slop
static float g_press_hit[3], g_cur_hit[3];       // table-plane hits (play space) at press / now
static int g_press_hit_ok, g_cur_hit_ok;
static void post_mouse(UINT msg) {
    if (!g_game_hwnd || !IsWindow(g_game_hwnd)) {
        g_game_hwnd = NULL;
        EnumWindows(find_game_window, (LPARAM)&g_game_hwnd);
        if (!g_game_hwnd) return;
    }
    RECT r;
    GetClientRect(g_game_hwnd, &r);
    int x = (r.right - r.left) / 2, y = (r.bottom - r.top) / 2;
    if (g_mouse_set) { x = g_mouse_x; y = g_mouse_y; }
    // Click slop: while a button is held, keep the mouse where it was pressed
    // until the ray has moved clearly (> 10 px), so hand jitter doesn't turn a
    // click into a drag (the build menu cards cancel on any move). Unchanged
    // positions aren't re-sent either.
    static int last_x = -1, last_y = -1, press_x, press_y;
#define dragging g_dragging
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) { press_x = last_x >= 0 ? last_x : x; press_y = last_y >= 0 ? last_y : y; dragging = 0; x = press_x; y = press_y; }
    if (msg == WM_MOUSEMOVE) {
        if (g_ptr_lbtn || g_ptr_rbtn) {
            int dx = x - press_x, dy = y - press_y;
            if (!dragging && dx * dx + dy * dy <= 100) return;
            dragging = 1;
        }
        if (x == last_x && y == last_y) return;
    }
    if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) {
        if (!dragging) { x = press_x; y = press_y; }
        dragging = 0;
    }
    last_x = x;
    last_y = y;
    LPARAM pos = MAKELPARAM(x, y);
    WPARAM keys = (g_ptr_lbtn ? MK_LBUTTON : 0) | (g_ptr_rbtn ? MK_RBUTTON : 0);
    PostMessageA(g_game_hwnd, msg, keys, pos);
}

static void log_pointer_hit(const char *what);
// The game's window asks Windows for WM_MOUSELEAVE (TrackMouseEvent). With the
// VR laser driving the game's mouse, the real cursor is somewhere else, so
// Windows answers every posted mouse move with a "mouse left the window"
// right away: the game then sees hover -> leave -> hover ... (hover sounds
// replay, tooltips never get to open). While the laser is in use, those leave
// messages are dropped.
static WNDPROC g_game_wndproc;
static long g_leaves_dropped;
static LRESULT CALLBACK game_wndproc_hook(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE) {
        LARGE_INTEGER qn;
        QueryPerformanceCounter(&qn);
        double now = g_freq.QuadPart ? (double)qn.QuadPart / (double)g_freq.QuadPart : 0;
        if (now - g_ptr_last_use < 2.0) {
            if (g_leaves_dropped++ == 0) logf_("mouse: dropping WM_MOUSELEAVE while the VR pointer is in use");
            return 0;
        }
    }
    return CallWindowProcW(g_game_wndproc, w, msg, wp, lp);
}
static void hook_game_wndproc(HWND w) {
    if (g_game_wndproc || !w) return;
    g_game_wndproc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)game_wndproc_hook);
    logf_("mouse: game window procedure hooked (%s)", g_game_wndproc ? "ok" : "failed");
}

// Game window as a normal window sized like one eye (aspect of the headset's
// recommended eye image), at the top-left of its monitor. The game renders the
// eyes at the window's size, so this also sets the eye resolution.
void xr_window_mirror(int rec_w, int rec_h) {
    HWND w = NULL;
    EnumWindows(find_game_window, (LPARAM)&w);
    if (!w || rec_w <= 0 || rec_h <= 0) { logf_("mirror window: game window not found"); return; }
    g_game_hwnd = w;
    hook_game_wndproc(w);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    LONG st = GetWindowLongA(w, GWL_STYLE), ex = GetWindowLongA(w, GWL_EXSTYLE);
    st = (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
    ex &= ~WS_EX_TOPMOST;
    SetWindowLongA(w, GWL_STYLE, st);
    SetWindowLongA(w, GWL_EXSTYLE, ex);
    RECT nc = {0, 0, 100, 100};
    AdjustWindowRectEx(&nc, st, FALSE, ex);
    int frame_w = (nc.right - nc.left) - 100, frame_h = (nc.bottom - nc.top) - 100;
    int wa_w = mi.rcWork.right - mi.rcWork.left, wa_h = mi.rcWork.bottom - mi.rcWork.top;
    int ch = rec_h < wa_h - frame_h ? rec_h : wa_h - frame_h;
    int cw = (int)((double)ch * rec_w / rec_h + 0.5);
    if (cw > wa_w - frame_w) { cw = wa_w - frame_w; ch = (int)((double)cw * rec_h / rec_w + 0.5); }
    SetWindowPos(w, HWND_NOTOPMOST, mi.rcWork.left, mi.rcWork.top, cw + frame_w, ch + frame_h,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    logf_("mirror window: %dx%d client (eye %dx%d)", cw, ch, rec_w, rec_h);
}

// True if the OS cursor is over the game window's client area.
__attribute__((unused)) static int cursor_over_game(void) {
    if (!g_game_hwnd) return 0;
    POINT sp, cp;
    RECT r;
    GetCursorPos(&sp);
    cp = sp;
    ScreenToClient(g_game_hwnd, &cp);
    GetClientRect(g_game_hwnd, &r);
    HWND top = WindowFromPoint(sp);
    return PtInRect(&r, cp) && top && GetAncestor(top, GA_ROOT) == g_game_hwnd;
}

static void pose_to_world(const float pp[3], const float pq[4], d3 *pos, d3 *fwd, d3 *up);
static int project_overview(d3 G, double W, double H, double *px, double *py);
static d3 g_ptr_wpos, g_ptr_wfwd, g_ptr_ground;

// Controller ray -> ground point -> pixel in the overview camera. The game's
// mouse goes there, so its own picking (with the overview camera) finds what
// the controller points at. Returns 0 if the ray misses the visible area.
static int aim_mouse(void) {
    d3 pos, fwd, up;
    pose_to_world(g_ptr_pos, g_ptr_quat, &pos, &fwd, &up);
    g_ptr_wpos = pos;
    g_ptr_wfwd = fwd;
    double gz = g_cam_in[5]; // ground height ~ game camera target height
    if (fwd.z > -1e-4) return 0; // pointing at or above the horizon
    double t = (gz - pos.z) / fwd.z;
    if (t <= 0) return 0;
    d3 G = {pos.x + t * fwd.x, pos.y + t * fwd.y, gz};
    g_ptr_ground = G;
    if (!g_game_hwnd) EnumWindows(find_game_window, (LPARAM)&g_game_hwnd);
    if (!g_game_hwnd) return 0;
    RECT r;
    GetClientRect(g_game_hwnd, &r);
    double px, py;
    if (!project_overview(G, r.right - r.left, r.bottom - r.top, &px, &py)) return 0;
    g_mouse_x = (int)(px + 0.5);
    g_mouse_y = (int)(py + 0.5);
    return 1;
}

// Controller ray on a HUD panel -> the matching UI pixel (as window px; the
// game scales window px to engine px). Returns 0 if no panel is hit.
static int g_on_panel;
static int panel_mouse(void) {
    d3 dir = quat_rot(g_ptr_quat, (d3){0, 0, -1});
    float d[3] = {(float)dir.x, (float)dir.y, (float)dir.z}, ux, uy, dist;
    int hit = xrt_panel_hit(g_ptr_pos, d, &ux, &uy, &dist);
    if (hit != g_on_panel) {
        g_on_panel = hit;
        logf_("pointer %s HUD panel", hit ? "on" : "off");
        if (hit) xrt_haptic(1, 0.15f, 15); // light tick entering a panel
    }
    if (!hit) return 0;
    int rw, rh;
    if (!xrt_rt_size(&rw, &rh)) return 0;
    if (!g_game_hwnd) EnumWindows(find_game_window, (LPARAM)&g_game_hwnd);
    if (!g_game_hwnd) return 0;
    RECT r;
    GetClientRect(g_game_hwnd, &r);
    g_mouse_x = (int)(ux * (r.right - r.left) / rw + 0.5f);
    g_mouse_y = (int)(uy * (r.bottom - r.top) / rh + 0.5f);
    return 1;
}

// Right-hand pointer. Trigger touch (or a partial pull) aims; trigger pull =
// left click, A = right click. Presses are latched and only sent once the
// pointer has been aiming for a few frames, so the game has hovered along the
// controller ray first (a quick tap from rest would otherwise click wherever
// the desktop view pointed).
#define PTR_SETTLE_FRAMES 3
static void pointer_frame(const xr_input *in) {
    static int frames, lpend, rpend;
    static float last_trig;
    if (fabsf(in->trigger[1] - last_trig) > 0.25f) {
        logf_("trigger %.2f (touch %d, aim valid %d)", in->trigger[1], in->trigger_touch[1], in->aim_valid[1]);
        last_trig = in->trigger[1];
    }
    if (xrt_options_input(in)) { // VR options menu (left stick click) took the laser this frame
        g_ptr_active = 0;
        return;
    }
    { // Escape (close Lore and other windows, cancel a building): left menu button or right stick click
        static int prev_esc;
        int esc = in->menu[0] || in->stick_click[1];
        // down and up go out on different frames: the game's key polling misses a
        // press whose down and up arrive within one frame
        LPARAM sc = (LPARAM)MapVirtualKeyA(VK_ESCAPE, 0) << 16;
        if (esc && !prev_esc && g_game_hwnd) {
            PostMessageA(g_game_hwnd, WM_KEYDOWN, VK_ESCAPE, sc | 1);
            logf_("escape (controller)");
            xrt_haptic(in->menu[0] ? 0 : 1, 0.3f, 20);
        }
        if (!esc && prev_esc && g_game_hwnd)
            PostMessageA(g_game_hwnd, WM_KEYUP, VK_ESCAPE, sc | 1 | (1u << 30) | (1u << 31));
        prev_esc = esc;
    }
    int want_l = in->trigger[1] > (g_ptr_lbtn ? 0.4f : 0.6f);
    int want_r = in->btn_lower[1];
    if (want_l && !g_ptr_lbtn) lpend = 1;
    if (want_r && !g_ptr_rbtn) rpend = 1;
    int raw = in->aim_valid[1] && (in->trigger_touch[1] || in->trigger[1] > 0.05f || want_r || lpend || rpend ||
                                   g_ptr_lbtn || g_ptr_rbtn);
    // The Quest's trigger-touch sensor flickers while a finger rests on it:
    // keep the pointer on for 0.5 s after the last touch, or the game's mouse
    // jumps away and back (hover sounds re-trigger, the build preview drops).
    LARGE_INTEGER qn;
    QueryPerformanceCounter(&qn);
    double now = (double)qn.QuadPart / (double)g_freq.QuadPart;
    static double last_raw = -10;
    if (raw) last_raw = now;
    g_ptr_last_use = last_raw;
    int active = in->aim_valid[1] && (raw || now - last_raw < 0.5);
    if (active) {
        memcpy(g_ptr_pos, in->aim_pos[1], sizeof(g_ptr_pos));
        memcpy(g_ptr_quat, in->aim_quat[1], sizeof(g_ptr_quat));
        if (!g_ptr_logged) { g_ptr_logged = 1; logf_("pointer: right controller aiming"); }
    }
    if (active != g_ptr_active) logf_("pointer %s", active ? "on" : "off");
    g_ptr_active = active;
    if (!active) {
        frames = lpend = rpend = 0;
        if (g_ptr_lbtn) { g_ptr_lbtn = 0; post_mouse(WM_LBUTTONUP); }
        if (g_ptr_rbtn) { g_ptr_rbtn = 0; post_mouse(WM_RBUTTONUP); }
        // Leave the game's mouse where the laser last was (jumping to the
        // centre re-triggers hover effects).
        return;
    }
    frames++;
    g_mouse_set = panel_mouse() || (overview_on() && aim_mouse());
    { // where the ray meets the table plane (for the drag box drawn in VR)
        d3 dir = quat_rot(g_ptr_quat, (d3){0, 0, -1});
        g_cur_hit_ok = 0;
        if (dir.y < -1e-3) {
            double t = (tab_pos[1] - g_ptr_pos[1]) / dir.y;
            g_cur_hit[0] = (float)(g_ptr_pos[0] + t * dir.x);
            g_cur_hit[1] = (float)tab_pos[1];
            g_cur_hit[2] = (float)(g_ptr_pos[2] + t * dir.z);
            g_cur_hit_ok = t > 0 && !g_on_panel;
        }
    }
    post_mouse(WM_MOUSEMOVE);
    if (frames < PTR_SETTLE_FRAMES) return;
    // Down on the first settled frame after a press; up on a later frame once
    // the button is released (so even a one-frame tap is a full click).
    if (lpend && !g_ptr_lbtn) { lpend = 0; memcpy(g_press_hit, g_cur_hit, sizeof(g_press_hit)); g_press_hit_ok = g_cur_hit_ok; g_ptr_lbtn = 1; post_mouse(WM_LBUTTONDOWN); log_pointer_hit("left click"); xrt_haptic(1, 0.35f, 25); }
    else if (g_ptr_lbtn && !want_l) { g_ptr_lbtn = 0; post_mouse(WM_LBUTTONUP); }
    if (rpend && !g_ptr_rbtn) { rpend = 0; g_ptr_rbtn = 1; post_mouse(WM_RBUTTONDOWN); log_pointer_hit("right click"); xrt_haptic(1, 0.6f, 45); }
    else if (g_ptr_rbtn && !want_r) { g_ptr_rbtn = 0; post_mouse(WM_RBUTTONUP); }
}

// Menus (theater screen): same pointer as in a match, hitting only the screen.
void xr_pointer_frame(const xr_input *in) { pointer_frame(in); }

// Drag box for xr_theater.c: corners on the table (play space) while the
// left button is held and dragging on the table.
int xr_drag_rect(float a[3], float b[3]) {
    if (!g_ptr_lbtn || !g_dragging || !g_press_hit_ok || !g_cur_hit_ok) return 0;
    memcpy(a, g_press_hit, sizeof(g_press_hit));
    memcpy(b, g_cur_hit, sizeof(g_cur_hit));
    return 1;
}

int dio_table_info(float out[5]) {
    if (!dio_placed) return 0;
    out[0] = (float)tab_pos[0]; out[1] = (float)tab_pos[1]; out[2] = (float)tab_pos[2];
    out[3] = (float)tab_yaw; out[4] = (float)g_table_r;
    return 1;
}

// Once per stereo frame, before the eyes are rendered.
void dio_frame(const xr_input *in, const float head[3], const float hq[4], double dt) {
    if (!dio_placed) place_table(head, hq, in->floor_space);
    // B (right): re-place the table in front of you. Y (left): reset the map view.
    if (in->btn_upper[1] && !prev_upper[1]) place_table(head, hq, in->floor_space);
    if (in->btn_upper[0] && !prev_upper[0]) reset_view();
    prev_upper[0] = in->btn_upper[0];
    prev_upper[1] = in->btn_upper[1];
    pointer_frame(in);
    if (!dio_theta_set) { // need the game camera first (set by eye_camera)
        for (int h = 0; h < 2; h++) prev_grip[h] = 0;
        return;
    }

    int g[2];
    d3 hl[2];
    for (int h = 0; h < 2; h++) {
        g[h] = in->hand_valid[h] && in->grip[h] > 0.6f;
        double p[3] = {in->hand_pos[h][0], in->hand_pos[h][1], in->hand_pos[h][2]};
        hl[h] = to_table(p);
        hl[h].y = 0; // pan/rotate/zoom in the table plane
    }
    d3 T = {g_last_tgt.x, g_last_tgt.y, g_last_tgt.z};
    d3 C = {T.x + dio_off[0], T.y + dio_off[1], T.z + dio_off[2]};
    double S = g_world_scale;

    if (g[0] && g[1] && prev_grip[0] && prev_grip[1]) {
        // two hands: zoom by hand spread, rotate by hand twist, pinned at the midpoint
        d3 a0 = to_table(prev_hand[0]), b0 = to_table(prev_hand[1]);
        a0.y = b0.y = 0;
        d3 m0 = d3_scale(d3_add(a0, b0), 0.5), m1 = d3_scale(d3_add(hl[0], hl[1]), 0.5);
        double v0x = b0.x - a0.x, v0z = b0.z - a0.z, v1x = hl[1].x - hl[0].x, v1z = hl[1].z - hl[0].z;
        double l0 = sqrt(v0x * v0x + v0z * v0z), l1 = sqrt(v1x * v1x + v1z * v1z);
        if (l0 > 0.02 && l1 > 0.02) {
            d3 W = d3_add(C, d3_scale(dio_M(m0), S));
            S *= l0 / l1;
            dio_theta += atan2(-v1z, v1x) - atan2(-v0z, v0x);
            C = d3_add(W, d3_scale(dio_M(m1), -S));
        }
    } else {
        for (int h = 0; h < 2; h++) {
            if (g[h] && prev_grip[h] && !g[1 - h]) { // one hand: drag the map
                d3 p0 = to_table(prev_hand[h]);
                p0.y = 0;
                d3 dh = {hl[h].x - p0.x, 0, hl[h].z - p0.z};
                C = d3_add(C, d3_scale(dio_M(dh), -S));
            }
        }
    }
    // Right stick: rotate (x) and zoom (y, up = closer). Left stick y: table height.
    double rx = in->stick[1][0], ry = in->stick[1][1], ly = in->stick[0][1];
    if (fabs(rx) > 0.2) dio_theta -= rx * 1.2 * dt;
    if (fabs(ry) > 0.2) S *= exp(-ry * 1.2 * dt);
    if (fabs(ly) > 0.2) tab_pos[1] += ly * 0.4 * dt;
    // Left stick x: table size. Grow away from you (near edge stays put).
    double lxs = in->stick[0][0];
    if (fabs(lxs) > 0.3) {
        double nr = g_table_r * exp(lxs * 0.5 * dt);
        if (nr < TABLE_MIN_R) nr = TABLE_MIN_R;
        if (nr > TABLE_MAX_R) nr = TABLE_MAX_R;
        double dr = nr - g_table_r, c = cos(tab_yaw), sn = sin(tab_yaw);
        tab_pos[0] -= sn * dr; // table forward (away from you) = (-sin yaw, 0, -cos yaw)
        tab_pos[2] -= c * dr;
        g_table_r = nr;
        g_table_r_dirty = 1;
        g_table_r_idle = 0;
    } else if (g_table_r_dirty && (g_table_r_idle += dt) > 1.0) {
        g_table_r_dirty = 0;
        save_settings();
    }
    if (S < 5) S = 5;
    if (S > 130) S = 130; // further out the camera far plane cuts the map (TODO: raise zFar)
    g_world_scale = S;
    dio_off[0] = C.x - T.x;
    dio_off[1] = C.y - T.y;
    dio_off[2] = C.z - T.z;
    for (int h = 0; h < 2; h++) {
        prev_grip[h] = g[h];
        for (int k = 0; k < 3; k++) prev_hand[h][k] = in->hand_pos[h][k];
    }
}

// Play-space pose -> world position, forward and up (through the table).
static void pose_to_world(const float pp[3], const float pq[4], d3 *pos, d3 *fwd, d3 *up) {
    d3 P = {g_cam_in[0], g_cam_in[1], g_cam_in[2]};
    d3 T = {g_cam_in[3], g_cam_in[4], g_cam_in[5]};
    if (!dio_theta_set) { // start with the same heading as the game camera
        double fx = T.x - P.x, fy = T.y - P.y;
        dio_theta = (fx * fx + fy * fy > 1e-8) ? atan2(fy, fx) : M_PI / 2;
        dio_theta_set = 1;
        logf_("diorama: map heading %.0f deg, scale %.0f units/m", dio_theta / DEG, g_world_scale);
    }
    d3 C = {T.x + dio_off[0], T.y + dio_off[1], T.z + dio_off[2]};
    double p[3] = {pp[0], pp[1], pp[2]};
    d3 local = to_table(p);
    // orientation relative to the table: conj(q_table) * q
    double qt[4] = {0, -sin(tab_yaw / 2), 0, cos(tab_yaw / 2)};
    double qe[4] = {pq[0], pq[1], pq[2], pq[3]}, ql[4];
    quat_mul(qt, qe, ql);
    float qlf[4] = {(float)ql[0], (float)ql[1], (float)ql[2], (float)ql[3]};
    *pos = d3_add(C, d3_scale(dio_M(local), g_world_scale));
    *fwd = dio_M(quat_rot(qlf, (d3){0, 0, -1}));
    *up = dio_M(quat_rot(qlf, (d3){0, 1, 0}));
}

// Eye camera for the current eye: play-space eye pose -> table local -> world,
// with the eye's asymmetric projection.
static void eye_camera(const float ep[3], const float eq[4], const float tn[4]) {
    d3 eye, fwd, up;
    pose_to_world(ep, eq, &eye, &fwd, &up);
    d3 tgt = d3_add(eye, d3_scale(fwd, 10.0));
    double tl = tn[0], tr = tn[1], tu = tn[2], td = tn[3];
    double m11 = 2 / (tr - tl), m22 = 2 / (tu - td);
    double o[NV] = {eye.x, eye.y, eye.z, tgt.x, tgt.y, tgt.z, up.x, up.y, up.z,
                    2 * atan(1 / m22) * 180 / 3.14159265358979323846, // fovY (degrees)
                    m22 / m11,                                        // screenRatio
                    1.0,                                              // zoom
                    -(tr + tl) / (tr - tl),                           // viewX (off-centre)
                    -(tu + td) / (tu - td)};                          // viewY
    memcpy(g_cam_out, o, sizeof(o));
}

// Overview camera. While VR stereo runs, the game's main camera looks straight
// down on the area around the table centre. The game decides what to draw
// (units, buildings) and what can be picked from this camera, so it has to
// cover everything you can see on the table. The controller pointer moves the
// game's mouse to where its ray meets the ground in this view.
#define OV_RADIUS_M (g_table_r + 0.1) // table area covered by the overview
#define OV_SAFE_FRAC 0.46 // table radius -> this fraction of the screen's smaller half-size
#define OV_HEIGHT 70.0    // camera height (game units); the game hides units far from the camera
static double g_ov[NV];
static int g_ov_valid;
static int overview_on(void) { return dio_theta_set && xrt_stereo_on(); }

static void overview_camera(void) {
    d3 C = {g_cam_in[3] + dio_off[0], g_cam_in[4] + dio_off[1], g_cam_in[5] + dio_off[2]};
    double R = OV_RADIUS_M * g_world_scale;
    // The table must land in the middle of the screen, inside the area the
    // HUD never covers: otherwise the game sees the mouse over (hidden) UI
    // and hides the building preview / sends clicks to the UI. And the camera
    // stays at a normal height (wider view instead) so zooming out doesn't
    // move it so far away that the game stops drawing units and buildings.
    double ratio = g_cam_in[10] > 0 ? g_cam_in[10] : 1.0; // width / height
    double rfrac = OV_SAFE_FRAC * (ratio < 1 ? ratio : 1);
    double H = OV_HEIGHT, t = R / (H * rfrac), fov = 2 * atan(t) / DEG;
    if (fov > 150) { fov = 150; H = R / (tan(75 * DEG) * rfrac); }
    d3 F = dio_F(); // screen up = away from you on the table
    double o[NV] = {C.x, C.y, C.z + H, C.x, C.y, C.z, F.x, F.y, 0, fov, ratio, 1.0, 0, 0};
    memcpy(g_ov, o, sizeof(o));
    memcpy(g_cam_out, o, sizeof(o));
    g_ov_valid = 1;
}

static d3 d3_sub(d3 a, d3 b) { d3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static double d3_dot(d3 a, d3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static d3 d3_cross(d3 a, d3 b) { d3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; return r; }
static d3 d3_norm(d3 a) { double l = sqrt(d3_dot(a, a)); return l > 0 ? d3_scale(a, 1 / l) : a; }

// World point -> client pixel through the overview camera, using Heaps'
// conventions (makeCameraMatrix: az = forward, ax = up x az, ay = az x ax;
// makeFrustumMatrix: m22 = zoom / tan(fovY/2), m11 = m22 / screenRatio).
static int project_overview(d3 G, double W, double Hp, double *px, double *py) {
    if (!g_ov_valid) return 0;
    d3 pos = {g_ov[0], g_ov[1], g_ov[2]}, tgt = {g_ov[3], g_ov[4], g_ov[5]}, up = {g_ov[6], g_ov[7], g_ov[8]};
    d3 az = d3_norm(d3_sub(tgt, pos));
    d3 ax = d3_norm(d3_cross(up, az));
    d3 ay = d3_cross(az, ax);
    d3 d = d3_sub(G, pos);
    double xv = d3_dot(ax, d), yv = d3_dot(ay, d), zv = d3_dot(az, d);
    if (zv <= 1e-6) return 0;
    double m22 = g_ov[11] / tan(g_ov[9] * 0.5 * DEG), m11 = m22 / g_ov[10];
    double nx = xv * m11 / zv, ny = yv * m22 / zv;
    if (fabs(nx) > 1 || fabs(ny) > 1) return 0;
    *px = (nx + 1) * 0.5 * W;
    *py = (1 - ny) * 0.5 * Hp;
    return 1;
}

static void log_pointer_hit(const char *what) {
    d3 P = g_ptr_wpos, F = g_ptr_wfwd;
    logf_("pointer %s: ray from (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) hits ground at (%.1f %.1f); "
          "table centre (%.1f %.1f) scale %.0f; mouse %s (%d %d)",
          what, P.x, P.y, P.z, F.x, F.y, F.z, g_ptr_ground.x, g_ptr_ground.y,
          g_cam_in[3] + dio_off[0], g_cam_in[4] + dio_off[1], g_world_scale,
          g_mouse_set ? (g_on_panel ? "on HUD panel at" : "at") : "OFF-VIEW, centre", g_mouse_x, g_mouse_y);
}

// Orbit test (F9): move pos around the target.
static void orbit_camera(void) {
    double px = g_cam_in[0], py = g_cam_in[1], pz = g_cam_in[2], tx = g_cam_in[3], ty = g_cam_in[4], tz = g_cam_in[5];
    double ox = px - tx, oy = py - ty, oz = pz - tz;
    double r = sqrt(ox * ox + oy * oy + oz * oz);
    if (r < 1e-6) return;
    double yaw = atan2(oy, ox) + g_yaw;
    double pitch = asin(oz / r) + g_pitch;
    if (pitch > 89.0 * DEG) pitch = 89.0 * DEG;
    if (pitch < -10.0 * DEG) pitch = -10.0 * DEG;
    r *= g_zoom;
    g_cam_out[0] = tx + r * cos(pitch) * cos(yaw);
    g_cam_out[1] = ty + r * cos(pitch) * sin(yaw);
    g_cam_out[2] = tz + r * sin(pitch);
}

HL_PRIM void HL_NAME(xr_cam_compute)(double a0, double a1, double a2, double a3, double a4, double a5, double a6,
                                     double a7, double a8, double a9, double a10, double a11, double a12, double a13) {
    double in[NV] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13};
    memcpy(g_cam_in, in, sizeof(in));
    memcpy(g_cam_out, in, sizeof(in));
    g_last_pos.x = a0; g_last_pos.y = a1; g_last_pos.z = a2;
    g_last_tgt.x = a3; g_last_tgt.y = a4; g_last_tgt.z = a5;
    static int logged;
    if (!logged) {
        logged = 1;
        logf_("main camera: pos(%.2f %.2f %.2f) target(%.2f %.2f %.2f) up(%.2f %.2f %.2f) fovY=%.2f ratio=%.3f "
              "zoom=%.2f view(%.3f %.3f)", a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    }
    float ep[3], eq[4], tn[4];
    if (xr_current_eye(ep, eq, tn)) eye_camera(ep, eq, tn);
    else if (g_cur_is_main && overview_on()) overview_camera();
    else if (g_override) orbit_camera();
    g_out_i = g_saved_i = 0;
    g_cam_pending = 1;
}
HL_PRIM double HL_NAME(xr_out_next)(void) { return g_out_i < NV ? g_cam_out[g_out_i++] : 0; }

// h3d.Camera.update before return: restore the game's own values?
HL_PRIM bool HL_NAME(xr_cam_restore)(void) {
    int r = g_cam_pending;
    g_cam_pending = 0;
    g_saved_i = 0;
    return r;
}
HL_PRIM double HL_NAME(xr_saved_next)(void) { return g_saved_i < NV ? g_cam_in[g_saved_i++] : 0; }

// Phase 2 frame flow (implemented in xr_theater.c)
HL_PRIM bool HL_NAME(xr_stereo_begin)(void) { return xrt_stereo_begin() != 0; }
HL_PRIM void HL_NAME(xr_eye_done)(void *backbuffer) { xrt_eye_done(backbuffer); }
HL_PRIM bool HL_NAME(xr_ui_begin)(void *backbuffer) { return xrt_ui_begin(backbuffer) != 0; }
HL_PRIM bool HL_NAME(xr_draw_3d)(void) { return xrt_draw_3d() != 0; }
HL_PRIM bool HL_NAME(xr_draw_2d)(void) { return xrt_draw_2d() != 0; }

// ------------------------------------------------------- process diagnostics
static void log_processes(void) {
    logf_("this process: pid=%lu cmdline=%s", GetCurrentProcessId(), GetCommandLineA());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (_stricmp(pe.szExeFile, "Northgard.exe") == 0)
            logf_("  Northgard.exe pid=%lu parent=%lu%s", pe.th32ProcessID, pe.th32ParentProcessID,
                  pe.th32ProcessID == GetCurrentProcessId() ? "  (this)" : "");
    CloseHandle(snap);
}

// Close any other Northgard.exe (stray vanilla copies or instances stuck on the
// exit screen) so tests always run against this patched instance.
// The Meta XR Simulator app is started by the game process, so Steam counts it
// as part of the game and shows "Stopping" until it closes. Close it whenever
// the game exits or restarts (and leftovers at startup).
static void kill_simulators(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "MetaXRSimulator.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        BOOL done = h && TerminateProcess(h, 0);
        if (h) CloseHandle(h);
        logf_("  closed MetaXRSimulator.exe pid=%lu: %s", pe.th32ProcessID, done ? "ok" : "failed");
    }
    CloseHandle(snap);
}

// End this game (and the simulator) right now.
static void quit_now(const char *why) {
    logf_("quitting: %s", why);
    kill_simulators();
    TerminateProcess(GetCurrentProcess(), 0);
}

static void kill_other_instances(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "Northgard.exe") != 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        BOOL done = h && TerminateProcess(h, 0);
        if (h) CloseHandle(h);
        logf_("  closed stray Northgard.exe pid=%lu: %s", pe.th32ProcessID, done ? "ok" : "failed");
    }
    CloseHandle(snap);
}

static BOOL CALLBACK log_window(HWND w, LPARAM unused) {
    (void)unused;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char title[128];
    RECT r;
    GetWindowTextA(w, title, sizeof(title));
    GetWindowRect(w, &r);
    logf_("  window %p \"%s\" %ldx%ld at (%ld,%ld) visible=%d foreground=%d", (void *)w, title, r.right - r.left,
          r.bottom - r.top, r.left, r.top, IsWindowVisible(w), w == GetForegroundWindow());
    return TRUE;
}
static void log_windows(void) { EnumWindows(log_window, 0); }

// ------------------------------------------------------------ self-restart
// Deploys drop NorthgardVR_restart.flag next to Northgard.exe. The running game
// then starts a fresh copy of itself with the same command line (so it loads
// the newly deployed vrboot.dat / DLL) and exits; the new copy also closes any
// leftover instances at startup. steam_appid.txt in the game folder lets a
// directly started copy log in to Steam.
static void exe_dir_file(const char *name, char *out, size_t n) {
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
    while (len > 0 && out[len - 1] != '\\') len--;
    out[len] = 0;
    strcat_s(out, n, name);
}

static void check_restart_flag(void) {
    char flag[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH];
    // NorthgardVR_quit.flag: close the game (and the simulator).
    exe_dir_file("NorthgardVR_quit.flag", flag, sizeof(flag));
    WIN32_FILE_ATTRIBUTE_DATA qa;
    if (GetFileAttributesExA(flag, GetFileExInfoStandard, &qa)) {
        DeleteFileA(flag);
        FILETIME cr, ex_, kt_, ut_;
        // a quit flag left from before this game started (game wasn't running) is ignored
        if (GetProcessTimes(GetCurrentProcess(), &cr, &ex_, &kt_, &ut_) && CompareFileTime(&qa.ftLastWriteTime, &cr) < 0)
            logf_("stale quit flag ignored");
        else
            quit_now("quit flag");
    }
    exe_dir_file("NorthgardVR_restart.flag", flag, sizeof(flag));
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(flag, GetFileExInfoStandard, &fa)) return;
    DeleteFileA(flag);
    // A flag written before this process started is left over from a deploy
    // while the game wasn't running: ignore it.
    FILETIME created, ex, kt, ut;
    if (GetProcessTimes(GetCurrentProcess(), &created, &ex, &kt, &ut) &&
        CompareFileTime(&fa.ftLastWriteTime, &created) < 0) {
        logf_("stale restart flag ignored");
        return;
    }
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    exe_dir_file("", dir, sizeof(dir));
    char cmd[2 * MAX_PATH];
    sprintf_s(cmd, sizeof(cmd), "\"%s\" vrboot.dat", exe);
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    logf_("restart flag found: starting a fresh instance and exiting");
    kill_simulators();
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        TerminateProcess(GetCurrentProcess(), 0);
    } else {
        logf_("restart failed (%lu)", GetLastError());
    }
}

// ------------------------------------------------------------- frame tick
static LARGE_INTEGER g_freq, g_tLast, g_tPrev;

// Watchdog: if the frame loop stops for 45 s (a hang), log it and end the
// process, so Steam doesn't sit on "Stopping" and a relaunch works.
static volatile LONG64 g_wd_frames;
static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    LONG64 last = -1;
    int stalled = 0;
    for (;;) {
        Sleep(5000);
        LONG64 f = g_wd_frames;
        stalled = (f == last) ? stalled + 5 : 0;
        last = f;
        if (stalled >= 45) {
            logf_("WATCHDOG: no frame for %d s (frame %lld); ending the process", stalled, (long long)f);
            quit_now("watchdog");
        }
    }
    return 0;
}

HL_PRIM void HL_NAME(xr_smoke)(void) {
    g_wd_frames++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_frames == 0) {
        log_open();
        QueryPerformanceFrequency(&g_freq);
        g_tLast = g_tPrev = now;
        logf_("NorthgardVR openxr.hdll loaded; xr_smoke called from hxd.System.mainLoop");
        CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        log_openxr_runtime();
        hl_api_init();
        log_processes();
        kill_other_instances();
        kill_simulators(); // leftovers from an earlier run
        logf_("camtest ready: F9 override, F10 auto-orbit, Numpad 4/6 yaw, 8/2 pitch, +/- zoom, 5 reset");
    }
    double dt = (double)(now.QuadPart - g_tPrev.QuadPart) / g_freq.QuadPart;
    g_tPrev = now;
    if (dt > 0.1) dt = 0.1;
    poll_keys(dt);
    if (g_frames % 30 == 0) check_restart_flag();
    g_frames++;
    if (g_frames % 600 == 0) {
        double span = (double)(now.QuadPart - g_tLast.QuadPart) / g_freq.QuadPart;
        logf_("frame %lld  (%.1f fps)  main cam upd %ld  pos(%.1f %.1f %.1f) target(%.1f %.1f %.1f)  "
              "calls: Game.onUpdate %ld  Game.render %ld  CamCtrl.update %ld  Scene.render %ld",
              g_frames, 600.0 / span, g_n_cam, g_last_pos.x, g_last_pos.y, g_last_pos.z, g_last_tgt.x,
              g_last_tgt.y, g_last_tgt.z, g_ping[0], g_ping[1], g_ping[2], g_ping[3]);
        g_n_cam = 0;
        memset(g_ping, 0, sizeof(g_ping));
        if (g_frames % 3000 == 0) { log_processes(); log_windows(); }
        g_tLast = now;
    }
}

// Phase 6: render size override and mouse rescaling (see hlpatch patch_rt).
HL_PRIM bool HL_NAME(xr_rt_on)(void) {
    int w, h;
    return xrt_rt_size(&w, &h);
}
HL_PRIM int HL_NAME(xr_rt_w)(void) {
    int w = 0, h = 0;
    xrt_rt_size(&w, &h);
    return w;
}
HL_PRIM int HL_NAME(xr_rt_h)(void) {
    int w = 0, h = 0;
    xrt_rt_size(&w, &h);
    return h;
}
// Window pixels -> engine pixels (the engine renders larger than the window).
static int scale_mouse(int v, int axis) {
    int w, h;
    if (!xrt_rt_size(&w, &h)) return v;
    // While the VR pointer is in use (within 2 s), it owns the game's mouse:
    // real mouse events (cursor resting over the mirror window) would
    // otherwise pull the game's cursor away from the laser.
    {
        LARGE_INTEGER qn;
        QueryPerformanceCounter(&qn);
        double now = (double)qn.QuadPart / (double)g_freq.QuadPart;
        if (g_mouse_set && now - g_ptr_last_use < 2.0) v = axis == 0 ? g_mouse_x : g_mouse_y;
    }
    if (!g_game_hwnd) EnumWindows(find_game_window, (LPARAM)&g_game_hwnd);
    if (!g_game_hwnd) return v;
    RECT r;
    GetClientRect(g_game_hwnd, &r);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw <= 0 || ch <= 0) return v;
    return axis == 0 ? (int)((double)v * w / cw) : (int)((double)v * h / ch);
}
HL_PRIM int HL_NAME(xr_mouse_x)(int x) { return scale_mouse(x, 0); }
HL_PRIM int HL_NAME(xr_mouse_y)(int y) { return scale_mouse(y, 1); }
HL_PRIM bool HL_NAME(xr_vsync_off)(void) {
    static int logged;
    int on = xrt_running();
    if (on && !logged) { logged = 1; logf_("vsync off while VR runs"); }
    return on;
}
DEFINE_PRIM(_BOOL, xr_rt_on, _NO_ARG);
DEFINE_PRIM(_I32, xr_rt_w, _NO_ARG);
DEFINE_PRIM(_I32, xr_rt_h, _NO_ARG);
DEFINE_PRIM(_I32, xr_mouse_x, _I32);
DEFINE_PRIM(_I32, xr_mouse_y, _I32);
DEFINE_PRIM(_BOOL, xr_vsync_off, _NO_ARG);
HL_PRIM bool HL_NAME(xr_ui_capture)(void) { return xrt_ui_capture(); }
HL_PRIM void HL_NAME(xr_ui_target)(void *res) { xrt_ui_target(res); }
DEFINE_PRIM(_BOOL, xr_ui_capture, _NO_ARG);
DEFINE_PRIM(_VOID, xr_ui_target, _ABSTRACT(dx_resource));

// Phase 1: called with the game's back buffer right before every present.
HL_PRIM void HL_NAME(xr_frame)(void *backbuffer) { xr_theater_frame(backbuffer); }
HL_PRIM void HL_NAME(xr_ui_windows)(vdynamic *ui) { xr_ui_windows_impl(ui); }
DEFINE_PRIM(_VOID, xr_ui_windows, _DYN);

DEFINE_PRIM(_VOID, xr_smoke, _NO_ARG);
DEFINE_PRIM(_VOID, xr_frame, _ABSTRACT(dx_resource));
DEFINE_PRIM(_VOID, xr_ping_0, _NO_ARG);
DEFINE_PRIM(_VOID, xr_ping_1, _NO_ARG);
DEFINE_PRIM(_VOID, xr_ping_2, _NO_ARG);
DEFINE_PRIM(_VOID, xr_ping_3, _NO_ARG);
DEFINE_PRIM(_BOOL, xr_cam_active, _NO_ARG);
DEFINE_PRIM(_VOID, xr_mark_main, _NO_ARG);
DEFINE_PRIM(_VOID, xr_cam_compute, _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64 _F64);
DEFINE_PRIM(_F64, xr_out_next, _NO_ARG);
DEFINE_PRIM(_BOOL, xr_cam_restore, _NO_ARG);
DEFINE_PRIM(_F64, xr_saved_next, _NO_ARG);
DEFINE_PRIM(_BOOL, xr_stereo_begin, _NO_ARG);
DEFINE_PRIM(_VOID, xr_eye_done, _ABSTRACT(dx_resource));
DEFINE_PRIM(_BOOL, xr_ui_begin, _ABSTRACT(dx_resource));
DEFINE_PRIM(_BOOL, xr_draw_3d, _NO_ARG);
DEFINE_PRIM(_BOOL, xr_draw_2d, _NO_ARG);
