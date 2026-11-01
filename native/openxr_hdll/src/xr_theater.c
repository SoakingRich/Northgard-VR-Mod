// Phase 1: OpenXR "theater mode".
//
// xr_frame(backbuffer) is called by the patched bytecode once per frame, right
// before the game presents. On the first call we take the game's D3D11 device
// from the back buffer, start an OpenXR session on the active runtime, and from
// then on copy each finished frame into a swapchain shown as a large floating
// quad in front of the user.
//
// The OpenXR runtime is loaded directly (active runtime JSON -> library ->
// xrNegotiateLoaderRuntimeInterface), so no loader DLL has to ship with the mod.
// Any failure just disables VR; the game keeps running on the monitor.

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "xr_theater.h"
#include "xr_uiwin.h"

void logf_(const char *fmt, ...);

// ------------------------------------------------------------------ state
static int g_state;  // 0 = not started, 1 = running, -1 = disabled
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;

static PFN_xrGetInstanceProcAddr xr_gipa;
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
static XrSwapchain g_swap = XR_NULL_HANDLE;
static XrSwapchainImageD3D11KHR g_images[8];
static uint32_t g_nimages;
static int g_sw_w, g_sw_h;   // swapchain size
static int g_cp_w, g_cp_h;   // size of the image last copied in (the game's back buffer)
static int g_rec_w, g_rec_h; // runtime's recommended per-eye size
static int g_max_layers = 16; // runtime's composition layer limit
static DXGI_FORMAT g_sw_fmt;
static XrSessionState g_sess_state = XR_SESSION_STATE_UNKNOWN;

// Phase 2 stereo state
static XrSwapchain g_eye_swap[2];
static XrSwapchainImageD3D11KHR g_eye_images[2][8];
static uint32_t g_eye_nimages[2];
static int g_stereo = 1;        // F7 toggles stereo / flat theater
static int g_frame_open;        // xrBeginFrame called, xrEndFrame pending
static int g_frame_render;      // frame state said shouldRender
static int g_eye = -1;          // 0/1 = rendering that eye, -2 = both eyes done, 2 = UI pass, -1 = none
static XrSwapchain g_ui_swap;
static XrSwapchainImageD3D11KHR g_ui_images[8];
static int g_ui_copied;
static int g_ui_panel = 1;      // F8: show the UI as a floating panel
static long g_stereo_frames;



// Copies the back buffer into the top-left of a swapchain image. Returns 0 if
// it doesn't fit.
static int copy_bb(ID3D11Texture2D *dst, ID3D11Texture2D *bb) {
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(bb, &bd);
    if ((int)bd.Width > g_sw_w || (int)bd.Height > g_sw_h) return 0;
    D3D11_BOX box = {0, 0, 0, bd.Width, bd.Height, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)dst, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
    g_cp_w = (int)bd.Width;
    g_cp_h = (int)bd.Height;
    return 1;
}

static int bb_fits(ID3D11Texture2D *bb) {
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(bb, &bd);
    return (int)bd.Width <= g_sw_w && (int)bd.Height <= g_sw_h;
}

// Diagnostics: sample a texture on the CPU (64x64 grid) and log its average
// colour and alpha, to tell "black image" from "opaque overlay".
static void probe(const char *what, ID3D11Texture2D *src) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st))) { logf_("probe %s: staging failed", what); return; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        double r = 0, g = 0, b = 0, a = 0;
        int opaque = 0, clear = 0, n = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const unsigned char *px = (const unsigned char *)m.pData + (size_t)(y * d.Height / 64) * m.RowPitch +
                                          (size_t)(x * d.Width / 64) * 4;
                r += px[0]; g += px[1]; b += px[2]; a += px[3];
                opaque += px[3] > 240;
                clear += px[3] < 15;
                n++;
            }
        logf_("probe %s: avg rgb(%.0f %.0f %.0f) alpha %.0f; %d%% opaque, %d%% transparent", what, r / n, g / n, b / n,
              a / n, opaque * 100 / n, clear * 100 / n);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}
// Debug: write a texture as raw RGBA (8-byte header: width, height) next to
// the game, for measuring UI element rectangles.
static void dump_texture(ID3D11Texture2D *src, const char *name) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        while (n > 0 && path[n - 1] != '\\') n--;
        path[n] = 0;
        strcat_s(path, MAX_PATH, name);
        FILE *f = NULL;
        if (!fopen_s(&f, path, "wb") && f) {
            fwrite(&d.Width, 4, 1, f);
            fwrite(&d.Height, 4, 1, f);
            for (UINT y = 0; y < d.Height; y++) fwrite((char *)m.pData + (size_t)y * m.RowPitch, 4, d.Width, f);
            fclose(f);
            logf_("dumped %s (%ux%u)", name, d.Width, d.Height);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}
static int g_eye_copied[2];
static XrFrameState g_fs;
static XrView g_views[2];
static int g_running; // between xrBeginSession and xrEndSession

// OpenXR entry points (resolved via xrGetInstanceProcAddr)
#define XR_FUNCS(X)                     \
    X(xrCreateInstance)                 \
    X(xrDestroyInstance)                \
    X(xrGetSystem)                      \
    X(xrCreateSession)                  \
    X(xrDestroySession)                 \
    X(xrBeginSession)                   \
    X(xrEndSession)                     \
    X(xrRequestExitSession)             \
    X(xrPollEvent)                      \
    X(xrCreateReferenceSpace)           \
    X(xrEnumerateSwapchainFormats)      \
    X(xrCreateSwapchain)                \
    X(xrDestroySwapchain)               \
    X(xrEnumerateSwapchainImages)       \
    X(xrAcquireSwapchainImage)          \
    X(xrWaitSwapchainImage)             \
    X(xrReleaseSwapchainImage)          \
    X(xrWaitFrame)                      \
    X(xrBeginFrame)                     \
    X(xrEndFrame)                       \
    X(xrGetInstanceProperties)          \
    X(xrGetSystemProperties)            \
    X(xrLocateViews)                    \
    X(xrStringToPath)                   \
    X(xrCreateActionSet)                \
    X(xrCreateAction)                   \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets)        \
    X(xrSyncActions)                    \
    X(xrGetActionStateBoolean)          \
    X(xrGetActionStateFloat)            \
    X(xrGetActionStateVector2f)         \
    X(xrCreateActionSpace)              \
    X(xrLocateSpace)                    \
    X(xrEnumerateReferenceSpaces)       \
    X(xrEnumerateViewConfigurationViews) \
    X(xrApplyHapticFeedback)
#define DECL(f) static PFN_##f p_##f;
XR_FUNCS(DECL)
static PFN_xrGetD3D11GraphicsRequirementsKHR p_xrGetD3D11GraphicsRequirementsKHR;

int xr_ui_toggle(void) { g_ui_panel = !g_ui_panel; return g_ui_panel; }

// Panels (flat screen, UI) are anchored to the head: eye height and facing
// direction when VR starts, and again on recentre (B). With a floor-level play
// space the origin is on the floor, so origin-relative panels end up too low.
static int g_floor_space;
static XrSpace g_view_space = XR_NULL_HANDLE;
static int g_anchor_set;
static float g_anchor_pos[3], g_anchor_yaw;

void xrt_set_panel_anchor(const float head[3], float yaw) {
    g_anchor_pos[0] = head[0]; g_anchor_pos[1] = head[1]; g_anchor_pos[2] = head[2];
    g_anchor_yaw = yaw;
    g_anchor_set = 1;
}

static void anchor_from_view(XrTime t) {
    if (g_anchor_set || !g_view_space) return;
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(p_xrLocateSpace(g_view_space, g_space, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    // Before tracking settles the runtime can report the origin; in a floor
    // space that would put the panels on the floor. Wait for a real head height.
    if (g_floor_space && loc.pose.position.y < 0.5f) return;
    XrQuaternionf q = loc.pose.orientation;
    // forward = q * (0,0,-1)
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float head[3] = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
    xrt_set_panel_anchor(head, atan2f(-fx, -fz));
    logf_("XR: panels anchored at head height %.2f m", head[1]);
}

// Pose `dist` metres ahead of the anchor, `down` metres below eye height.
static XrPosef panel_pose(float dist, float down) {
    XrPosef p;
    float s = sinf(g_anchor_yaw), c = cosf(g_anchor_yaw);
    // rotate (0, -down, -dist) by yaw about +Y: x = -dist*sin, z = -dist*cos
    p.position.x = g_anchor_pos[0] - dist * s;
    p.position.y = g_anchor_pos[1] - down;
    p.position.z = g_anchor_pos[2] - dist * c;
    p.orientation.x = 0; p.orientation.y = sinf(g_anchor_yaw / 2); p.orientation.z = 0;
    p.orientation.w = cosf(g_anchor_yaw / 2);
    return p;
}

#define XR_CHECK(call)                                                   \
    do {                                                                 \
        XrResult r_ = (call);                                            \
        if (XR_FAILED(r_)) {                                             \
            logf_("XR: %s failed (%d)", #call, (int)r_);                 \
            return 0;                                                    \
        }                                                                \
    } while (0)

// ----------------------------------------------------- runtime discovery
static int ci_contains(const char *s, const char *needle) {
    size_t n = strlen(needle);
    for (; *s; s++)
        if (_strnicmp(s, needle, n) == 0) return 1;
    return 0;
}

// Recursively looks under `dir` for a runtime manifest (*.json whose name
// mentions "simulator" and that contains "library_path").
static int search_manifest(const char *dir, char *out, size_t outsz, int depth) {
    if (depth > 4) return 0;
    char pat[MAX_PATH];
    sprintf_s(pat, MAX_PATH, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char p[MAX_PATH];
        sprintf_s(p, MAX_PATH, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = search_manifest(p, out, outsz, depth + 1);
        } else {
            size_t l = strlen(fd.cFileName);
            if (l > 5 && _stricmp(fd.cFileName + l - 5, ".json") == 0 && ci_contains(fd.cFileName, "simulator")) {
                FILE *f = NULL;
                char b[4096] = {0};
                if (!fopen_s(&f, p, "rb") && f) { fread(b, 1, sizeof(b) - 1, f); fclose(f); }
                if (strstr(b, "\"library_path\"")) { strcpy_s(out, outsz, p); found = 1; }
            }
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// Runtime choice. By default the active runtime (e.g. Virtual Desktop) is
// used when it has a headset; otherwise the Meta XR Simulator, if installed.
// NorthgardVR_runtime.txt next to Northgard.exe can force one: "simulator",
// "headset", or a manifest path.
static int find_simulator_manifest(char *json, size_t jsz) {
    HKEY k;
    int got = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &k) ==
        ERROR_SUCCESS) {
        for (DWORD i = 0; !got; i++) {
            char name[MAX_PATH];
            DWORD nl = MAX_PATH;
            if (RegEnumValueA(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (ci_contains(name, "simulator")) { strcpy_s(json, jsz, name); got = 1; }
        }
        RegCloseKey(k);
    }
    if (!got) got = search_manifest("C:\\Program Files\\MetaXRSimulator", json, jsz, 0);
    return got;
}

static int active_manifest(char *json, size_t jsz) {
    DWORD sz = (DWORD)jsz;
    return RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ, NULL,
                        json, &sz) == ERROR_SUCCESS;
}

// Reads NorthgardVR_runtime.txt; returns "" when absent.
static void runtime_setting(char *want, size_t wsz) {
    char cfg[MAX_PATH];
    want[0] = 0;
    DWORD n = GetModuleFileNameA(NULL, cfg, MAX_PATH);
    while (n > 0 && cfg[n - 1] != '\\') n--;
    cfg[n] = 0;
    strcat_s(cfg, MAX_PATH, "NorthgardVR_runtime.txt");
    FILE *f = NULL;
    if (fopen_s(&f, cfg, "rb") || !f) return;
    size_t r = fread(want, 1, wsz - 1, f);
    fclose(f);
    want[r] = 0;
    while (r > 0 && (want[r - 1] == '\r' || want[r - 1] == '\n' || want[r - 1] == ' ')) want[--r] = 0;
}

// Reads "library_path" from a runtime manifest.
static int find_runtime_library(const char *json, char *out, size_t outsz) {
    FILE *f = NULL;
    if (fopen_s(&f, json, "rb") || !f) { logf_("XR: cannot read %s", json); return 0; }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *k = strstr(buf, "\"library_path\"");
    if (!k) { logf_("XR: library_path missing in %s", json); return 0; }
    char *q1 = strchr(k + 14, '"');
    char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) return 0;
    char lib[MAX_PATH];
    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= sizeof(lib)) return 0;
    memcpy(lib, q1 + 1, len);
    lib[len] = 0;
    // JSON escapes "\\" -> "\"
    char *w = lib;
    for (char *r = lib; *r; r++) {
        if (r[0] == '\\' && r[1] == '\\') r++;
        *w++ = *r;
    }
    *w = 0;
    int absolute = (lib[0] && lib[1] == ':') || (lib[0] == '\\' && lib[1] == '\\');
    if (absolute) {
        strcpy_s(out, outsz, lib);
    } else { // relative to the manifest's folder
        strcpy_s(out, outsz, json);
        char *slash = strrchr(out, '\\');
        if (slash) slash[1] = 0; else out[0] = 0;
        strcat_s(out, outsz, lib);
    }
    return 1;
}

// ------------------------------------------------- optional API layer
// Test mode: if NorthgardVR_operator.txt (next to Northgard.exe) contains the
// folder of the Meta XR Operator layer, the layer is chained in front of the
// runtime, as the OpenXR loader would do. It then serves its MCP tools
// (headset screenshots, poses, ...) on localhost:8720 from inside the game.
static PFN_xrGetInstanceProcAddr g_rt_gipa;
static PFN_xrGetInstanceProcAddr g_layer_gipa;
static PFN_xrCreateApiLayerInstance g_layer_create;
static const char *OPERATOR_LAYER = "XR_APILAYER_METAX_operator";

static void load_operator_layer(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    char cfg[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, cfg, MAX_PATH);
    while (n > 0 && cfg[n - 1] != '\\') n--;
    cfg[n] = 0;
    strcat_s(cfg, MAX_PATH, "NorthgardVR_operator.txt");
    FILE *f = NULL;
    if (fopen_s(&f, cfg, "rb") || !f) return; // not in test mode
    char dir[MAX_PATH] = {0};
    size_t r = fread(dir, 1, sizeof(dir) - 1, f);
    fclose(f);
    while (r > 0 && (dir[r - 1] == '\r' || dir[r - 1] == '\n' || dir[r - 1] == ' ' || dir[r - 1] == '\\')) dir[--r] = 0;
    char dll[MAX_PATH];
    sprintf_s(dll, MAX_PATH, "%s\\XrApiLayer_METAX_operator.dll", dir);
    HMODULE m = LoadLibraryA(dll);
    if (!m) { logf_("XR: operator layer not loaded: %s (%lu)", dll, GetLastError()); return; }
    PFN_xrNegotiateLoaderApiLayerInterface neg =
        (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(m, "xrNegotiateLoaderApiLayerInterface");
    if (!neg) { logf_("XR: operator layer has no negotiate entry point"); return; }
    XrNegotiateLoaderInfo li = {0};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateApiLayerRequest rq = {0};
    rq.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    rq.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    rq.structSize = sizeof(rq);
    XrResult res = neg(&li, OPERATOR_LAYER, &rq);
    if (XR_FAILED(res) || !rq.getInstanceProcAddr || !rq.createApiLayerInstance) {
        logf_("XR: operator layer negotiation failed (%d)", (int)res);
        return;
    }
    g_layer_gipa = rq.getInstanceProcAddr;
    g_layer_create = rq.createApiLayerInstance;
    logf_("XR: TEST MODE: Meta XR Operator layer loaded from %s", dir);
}

// The end of the layer chain: create the instance on the runtime itself.
static XrResult XRAPI_CALL chain_terminator(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *li,
                                            XrInstance *inst) {
    (void)li;
    PFN_xrCreateInstance rt_create = NULL;
    if (XR_FAILED(g_rt_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&rt_create)) || !rt_create)
        return XR_ERROR_INITIALIZATION_FAILED;
    return rt_create(info, inst);
}

static int load_runtime(const char *json) {
    char path[MAX_PATH];
    if (!find_runtime_library(json, path, sizeof(path))) return 0;
    // Altered search path so the runtime's own dependencies load from its folder.
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { logf_("XR: LoadLibrary(%s) failed (%lu)", path, GetLastError()); return 0; }
    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) { logf_("XR: runtime has no xrNegotiateLoaderRuntimeInterface"); return 0; }
    XrNegotiateLoaderInfo li = {0};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr = {0};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) { logf_("XR: runtime negotiation failed (%d)", (int)r); return 0; }
    xr_gipa = g_rt_gipa = rr.getInstanceProcAddr;
    logf_("XR: runtime loaded: %s (interface %u, api %u.%u)", path, rr.runtimeInterfaceVersion,
          XR_VERSION_MAJOR(rr.runtimeApiVersion), XR_VERSION_MINOR(rr.runtimeApiVersion));
    return 1;
}

static int resolve(XrInstance inst) {
#define RES(f) if (XR_FAILED(xr_gipa(inst, #f, (PFN_xrVoidFunction *)&p_##f)) || !p_##f) { logf_("XR: missing %s", #f); return 0; }
    XR_FUNCS(RES)
#undef RES
    if (XR_FAILED(xr_gipa(inst, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction *)&p_xrGetD3D11GraphicsRequirementsKHR)))
        return 0;
    return 1;
}

// ------------------------------------------------------------- input
// Touch controllers: grip (squeeze) value + pose, thumbsticks, A/B/X/Y,
// thumbstick clicks, triggers. Suggested for the Oculus Touch profile (Quest
// through Virtual Desktop reports this profile).
static XrActionSet g_aset;
static XrAction a_grip, a_pose, a_aim, a_stick, a_trigger, a_trigger_touch, a_btn_lower, a_btn_upper, a_stick_click, a_menu, a_haptic;
static XrSpace g_hand_space[2], g_aim_space[2];
static XrPath g_hand_path[2];
static int g_input_ok;

static XrPath path(const char *s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(g_inst, s, &p);
    return p;
}

static int make_action(XrAction *a, XrActionType type, const char *name, const char *label) {
    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    strcpy_s(ai.actionName, XR_MAX_ACTION_NAME_SIZE, name);
    strcpy_s(ai.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, label);
    ai.countSubactionPaths = 2;
    ai.subactionPaths = g_hand_path;
    XR_CHECK(p_xrCreateAction(g_aset, &ai, a));
    return 1;
}

// Short vibration on one controller (amplitude 0..1).
void xrt_haptic(int hand, float amplitude, int ms) {
    if (!g_input_ok || !p_xrApplyHapticFeedback || g_state != 1) return;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = a_haptic;
    hi.subactionPath = g_hand_path[hand];
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = (XrDuration)ms * 1000000;
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    p_xrApplyHapticFeedback(g_sess, &hi, (const XrHapticBaseHeader *)&v);
}

static int setup_input(void) {
    g_hand_path[0] = path("/user/hand/left");
    g_hand_path[1] = path("/user/hand/right");
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "diorama");
    strcpy_s(si.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Diorama");
    XR_CHECK(p_xrCreateActionSet(g_inst, &si, &g_aset));
    if (!make_action(&a_grip, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grab table") ||
        !make_action(&a_pose, XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand pose") ||
        !make_action(&a_aim, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose") ||
        !make_action(&a_trigger_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "Trigger touch") ||
        !make_action(&a_stick, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick") ||
        !make_action(&a_trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") ||
        !make_action(&a_btn_lower, XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_lower", "A / X") ||
        !make_action(&a_btn_upper, XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_upper", "B / Y") ||
        !make_action(&a_stick_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click") ||
        !make_action(&a_menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu button (Escape)") ||
        !make_action(&a_haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration"))
        return 0;
    XrActionSuggestedBinding b[] = {
        {a_grip, path("/user/hand/left/input/squeeze/value")},
        {a_grip, path("/user/hand/right/input/squeeze/value")},
        {a_pose, path("/user/hand/left/input/grip/pose")},
        {a_pose, path("/user/hand/right/input/grip/pose")},
        {a_aim, path("/user/hand/left/input/aim/pose")},
        {a_aim, path("/user/hand/right/input/aim/pose")},
        {a_trigger_touch, path("/user/hand/left/input/trigger/touch")},
        {a_trigger_touch, path("/user/hand/right/input/trigger/touch")},
        {a_stick, path("/user/hand/left/input/thumbstick")},
        {a_stick, path("/user/hand/right/input/thumbstick")},
        {a_trigger, path("/user/hand/left/input/trigger/value")},
        {a_trigger, path("/user/hand/right/input/trigger/value")},
        {a_btn_lower, path("/user/hand/left/input/x/click")},
        {a_btn_lower, path("/user/hand/right/input/a/click")},
        {a_btn_upper, path("/user/hand/left/input/y/click")},
        {a_btn_upper, path("/user/hand/right/input/b/click")},
        {a_stick_click, path("/user/hand/left/input/thumbstick/click")},
        {a_stick_click, path("/user/hand/right/input/thumbstick/click")},
        {a_menu, path("/user/hand/left/input/menu/click")},
        {a_haptic, path("/user/hand/left/output/haptic")},
        {a_haptic, path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = sizeof(b) / sizeof(b[0]);
    XR_CHECK(p_xrSuggestInteractionProfileBindings(g_inst, &sb));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo asi = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.action = a_pose;
        asi.subactionPath = g_hand_path[h];
        asi.poseInActionSpace.orientation.w = 1;
        XR_CHECK(p_xrCreateActionSpace(g_sess, &asi, &g_hand_space[h]));
        asi.action = a_aim;
        XR_CHECK(p_xrCreateActionSpace(g_sess, &asi, &g_aim_space[h]));
    }
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g_aset;
    XR_CHECK(p_xrAttachSessionActionSets(g_sess, &at));
    g_input_ok = 1;
    logf_("XR: controller actions ready (Touch profile)");
    return 1;
}

static float get_float(XrAction a, int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_hand_path[h];
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(p_xrGetActionStateFloat(g_sess, &gi, &st)) && st.isActive ? st.currentState : 0;
}
static int get_bool(XrAction a, int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_hand_path[h];
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_sess, &gi, &st)) && st.isActive && st.currentState;
}

// Read controllers for this frame (poses predicted at display time).
static void read_input(xr_input *in) {
    memset(in, 0, sizeof(*in));
    in->floor_space = g_floor_space;
    if (!g_input_ok) return;
    XrActiveActionSet aas = {g_aset, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &aas;
    if (XR_FAILED(p_xrSyncActions(g_sess, &sync))) return;
    for (int h = 0; h < 2; h++) {
        in->grip[h] = get_float(a_grip, h);
        in->trigger[h] = get_float(a_trigger, h);
        in->btn_lower[h] = get_bool(a_btn_lower, h);
        in->btn_upper[h] = get_bool(a_btn_upper, h);
        in->stick_click[h] = get_bool(a_stick_click, h);
        in->menu[h] = get_bool(a_menu, h);
        in->trigger_touch[h] = get_bool(a_trigger_touch, h);
        XrSpaceLocation al = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(p_xrLocateSpace(g_aim_space[h], g_space, g_fs.predictedDisplayTime, &al)) &&
            (al.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (al.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            in->aim_valid[h] = 1;
            in->aim_pos[h][0] = al.pose.position.x;
            in->aim_pos[h][1] = al.pose.position.y;
            in->aim_pos[h][2] = al.pose.position.z;
            in->aim_quat[h][0] = al.pose.orientation.x;
            in->aim_quat[h][1] = al.pose.orientation.y;
            in->aim_quat[h][2] = al.pose.orientation.z;
            in->aim_quat[h][3] = al.pose.orientation.w;
        }
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a_stick;
        gi.subactionPath = g_hand_path[h];
        XrActionStateVector2f v = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_sess, &gi, &v)) && v.isActive) {
            in->stick[h][0] = v.currentState.x;
            in->stick[h][1] = v.currentState.y;
        }
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(p_xrLocateSpace(g_hand_space[h], g_space, g_fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            in->hand_valid[h] = 1;
            in->hand_pos[h][0] = loc.pose.position.x;
            in->hand_pos[h][1] = loc.pose.position.y;
            in->hand_pos[h][2] = loc.pose.position.z;
        }
    }
}

// ----------------------------------------------------------- start-up
static int pick_format(DXGI_FORMAT bb) {
    int64_t fmts[64];
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateSwapchainFormats(g_sess, 64, &n, fmts));
    // The back buffer holds gamma-encoded colour; an *_SRGB swapchain makes the
    // compositor read it correctly. CopyResource needs the same format family.
    DXGI_FORMAT want[3] = {bb, bb, bb};
    if (bb == DXGI_FORMAT_R8G8B8A8_UNORM) want[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (bb == DXGI_FORMAT_B8G8R8A8_UNORM) want[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (int w = 0; w < 3; w++)
        for (uint32_t i = 0; i < n; i++)
            if (fmts[i] == want[w]) { g_sw_fmt = want[w]; return 1; }
    logf_("XR: no swapchain format compatible with back buffer format %d", (int)bb);
    return 0;
}

// ---- mixed reality (passthrough)
// Settings "mixed_reality 1": the room shows through around the table and
// behind the menus. Two routes, whichever the runtime offers:
//   1 = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND (the runtime blends the camera
//       view behind transparent pixels), 2 = an XR_FB_passthrough layer.
static int g_mr_want, g_has_fb_pt, g_alpha_blend, g_mr;
// 3 = chroma key: the room's place is drawn in one flat colour that Virtual
// Desktop's "VR Passthrough" (Streaming tab, chroma keying) replaces with the
// camera view on the headset. mixed_reality 2 forces this mode.
static unsigned char g_key_rgb[3] = {255, 0, 255};
static float g_key_lin[3] = {1, 0, 1};
static XrSwapchain g_key_swap = XR_NULL_HANDLE;
static XrCompositionLayerQuad g_key_quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
static XrPassthroughFB g_pt = XR_NULL_HANDLE;
static XrPassthroughLayerFB g_pt_layer = XR_NULL_HANDLE;
static XrCompositionLayerPassthroughFB g_pt_comp = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};

static int settings_int(const char *key, int def) {
    char sp_[MAX_PATH], line[128], fmt[64];
    DWORD n_ = GetModuleFileNameA(NULL, sp_, MAX_PATH);
    while (n_ > 0 && sp_[n_ - 1] != '\\') n_--;
    sp_[n_] = 0;
    strcat_s(sp_, MAX_PATH, "NorthgardVR_settings.txt");
    sprintf_s(fmt, sizeof(fmt), "%s %%d", key);
    int v = def;
    FILE *sf = NULL;
    if (!fopen_s(&sf, sp_, "rb") && sf) {
        int t;
        while (fgets(line, sizeof(line), sf))
            if (sscanf_s(line, fmt, &t) == 1) v = t;
        fclose(sf);
    }
    return v;
}

// What the runtime offers for mixed reality (logged on every launch).
static void log_runtime_extensions(void) {
    PFN_xrEnumerateInstanceExtensionProperties en = NULL;
    if (XR_FAILED(xr_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&en)) || !en)
        return;
    uint32_t n = 0;
    if (XR_FAILED(en(NULL, 0, &n, NULL)) || !n) return;
    XrExtensionProperties *ep = (XrExtensionProperties *)calloc(n, sizeof(*ep));
    for (uint32_t i = 0; i < n; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    g_has_fb_pt = 0;
    if (XR_SUCCEEDED(en(NULL, n, &n, ep))) {
        char all[4096] = "";
        for (uint32_t i = 0; i < n; i++) {
            if (!strcmp(ep[i].extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) g_has_fb_pt = 1;
            if (strlen(all) + strlen(ep[i].extensionName) + 2 < sizeof(all)) {
                strcat_s(all, sizeof(all), ep[i].extensionName);
                strcat_s(all, sizeof(all), " ");
            }
        }
        logf_("XR: runtime extensions (%u): %s", n, all);
        logf_("XR: XR_FB_passthrough %s", g_has_fb_pt ? "AVAILABLE" : "not offered");
    }
    free(ep);
}

static void log_blend_modes(void) {
    PFN_xrEnumerateEnvironmentBlendModes eb = NULL;
    if (XR_FAILED(xr_gipa(g_inst, "xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction *)&eb)) || !eb) return;
    XrEnvironmentBlendMode m[8];
    uint32_t n = 0;
    g_alpha_blend = 0;
    if (XR_FAILED(eb(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &n, m))) return;
    char txt[128] = "";
    for (uint32_t i = 0; i < n; i++) {
        const char *nm = m[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE ? "opaque" : m[i] == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE ? "additive"
                       : m[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "alpha_blend" : "?";
        if (m[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) g_alpha_blend = 1;
        strcat_s(txt, sizeof(txt), nm);
        strcat_s(txt, sizeof(txt), " ");
    }
    logf_("XR: environment blend modes: %s", txt);
}

// After the session exists: pick the mixed reality route (if asked for).
static void key_backdrop_init(void);
static void key_backdrop_fill(void);
static void mr_init(void) {
    g_mr = 0;
    if (!g_mr_want) { logf_("XR: mixed reality off (settings: mixed_reality 1 to try it)"); return; }
    if (g_mr_want == 2) {
        g_mr = 3;
        key_backdrop_init();
        logf_("XR: mixed reality ON via chroma key %u,%u,%u (turn on VR Passthrough with this colour in Virtual "
              "Desktop's Streaming tab)", g_key_rgb[0], g_key_rgb[1], g_key_rgb[2]);
        return;
    }
    if (g_alpha_blend) {
        g_mr = 1;
        logf_("XR: mixed reality ON via alpha-blend environment mode");
        return;
    }
    if (g_has_fb_pt) {
        PFN_xrCreatePassthroughFB cp = NULL;
        PFN_xrCreatePassthroughLayerFB cl = NULL;
        PFN_xrPassthroughStartFB st = NULL;
        xr_gipa(g_inst, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&cp);
        xr_gipa(g_inst, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&cl);
        xr_gipa(g_inst, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&st);
        XrPassthroughCreateInfoFB pci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        XrResult r = cp ? cp(g_sess, &pci, &g_pt) : XR_ERROR_FUNCTION_UNSUPPORTED;
        if (XR_SUCCEEDED(r)) {
            XrPassthroughLayerCreateInfoFB lci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = g_pt;
            lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            r = cl ? cl(g_sess, &lci, &g_pt_layer) : XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        if (XR_SUCCEEDED(r)) {
            g_pt_comp.layerHandle = g_pt_layer;
            g_pt_comp.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            g_mr = 2;
            logf_("XR: mixed reality ON via XR_FB_passthrough layer");
        } else {
            logf_("XR: XR_FB_passthrough setup failed (%d); mixed reality off", (int)r);
        }
        return;
    }
    g_mr = 3;
    key_backdrop_init();
    logf_("XR: runtime offers neither alpha-blend nor XR_FB_passthrough: mixed reality via chroma key %u,%u,%u "
          "(turn on VR Passthrough with this colour in Virtual Desktop's Streaming tab)",
          g_key_rgb[0], g_key_rgb[1], g_key_rgb[2]);
}

// Menus have no projection layer, so in chroma-key mode a big key-coloured
// quad sits far behind the menu screen.
static void key_backdrop_fill(void) {
    if (!g_key_swap) return;
    XrSwapchainImageD3D11KHR img[4];
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, img[i].next = NULL;
    p_xrEnumerateSwapchainImages(g_key_swap, 4, &n, (XrSwapchainImageBaseHeader *)img);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (n && XR_SUCCEEDED(p_xrAcquireSwapchainImage(g_key_swap, &ai, &idx)) &&
        XR_SUCCEEDED(p_xrWaitSwapchainImage(g_key_swap, &wi))) {
        unsigned char px[16 * 16 * 4];
        for (int i = 0; i < 256; i++) px[i * 4] = g_key_rgb[0], px[i * 4 + 1] = g_key_rgb[1], px[i * 4 + 2] = g_key_rgb[2], px[i * 4 + 3] = 255;
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)img[idx].texture, 0, NULL, px, 16 * 4, 0);
        p_xrReleaseSwapchainImage(g_key_swap, &ri);
    }
}
static void key_backdrop_init(void) {
    if (g_key_swap) return;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g_sw_fmt;
    ci.sampleCount = 1;
    ci.width = ci.height = 16;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(p_xrCreateSwapchain(g_sess, &ci, &g_key_swap))) { g_key_swap = XR_NULL_HANDLE; return; }
    key_backdrop_fill();
    g_key_quad.space = g_space;
    g_key_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_key_quad.subImage.swapchain = g_key_swap;
    g_key_quad.subImage.imageRect.extent.width = 16;
    g_key_quad.subImage.imageRect.extent.height = 16;
    g_key_quad.size.width = g_key_quad.size.height = 60.0f;
}
static XrEnvironmentBlendMode mr_blend(void) {
    return g_mr == 1 ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}
int xrt_mixed_reality(void) { return g_mr != 0; }

// Creates the instance on one runtime and checks that it has a headset.
static int try_instance(const char *json) {
    if (!load_runtime(json)) return 0;
    if (XR_FAILED(xr_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance))) return 0;
    g_mr_want = settings_int("mixed_reality", 0);
    {
        int k[3] = {settings_int("mixed_reality_key_r", 255), settings_int("mixed_reality_key_g", 0),
                    settings_int("mixed_reality_key_b", 255)};
        for (int i = 0; i < 3; i++) {
            int v = k[i] < 0 ? 0 : k[i] > 255 ? 255 : k[i];
            g_key_rgb[i] = (unsigned char)v;
            double c = v / 255.0; // sRGB -> linear, so the sRGB swapchain stores exactly this byte
            g_key_lin[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
        }
    }
    log_runtime_extensions();
    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_FB_PASSTHROUGH_EXTENSION_NAME};
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "NorthgardVR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Heaps");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = (g_mr_want == 1 && g_has_fb_pt) ? 2 : 1;
    ici.enabledExtensionNames = exts;
    XrResult r = XR_ERROR_INITIALIZATION_FAILED;
    if (g_layer_create) {
        XrApiLayerNextInfo next = {0};
        next.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        next.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        next.structSize = sizeof(next);
        strcpy_s(next.layerName, XR_MAX_API_LAYER_NAME_SIZE, OPERATOR_LAYER);
        next.nextGetInstanceProcAddr = g_rt_gipa;
        next.nextCreateApiLayerInstance = chain_terminator;
        XrApiLayerCreateInfo lci = {0};
        lci.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
        lci.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
        lci.structSize = sizeof(lci);
        lci.nextInfo = &next;
        r = g_layer_create(&ici, &lci, &g_inst);
        if (XR_SUCCEEDED(r)) xr_gipa = g_layer_gipa; // every call now goes through the layer
        else logf_("XR: instance creation through the operator layer failed (%d); continuing without it", (int)r);
    }
    if (XR_FAILED(r)) r = p_xrCreateInstance(&ici, &g_inst);
    if (XR_FAILED(r)) { logf_("XR: xrCreateInstance failed (%d)", (int)r); g_inst = XR_NULL_HANDLE; return 0; }
    if (!resolve(g_inst)) { g_inst = XR_NULL_HANDLE; return 0; }

    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_inst, &ip)))
        logf_("XR: runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
              XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = p_xrGetSystem(g_inst, &sgi, &g_sys);
    if (XR_FAILED(r)) {
        logf_("XR: xrGetSystem failed (%d)", (int)r);
        p_xrDestroyInstance(g_inst);
        g_inst = XR_NULL_HANDLE;
        return 0;
    }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_inst, g_sys, &sp))) {
        logf_("XR: headset \"%s\", max %u layers", sp.systemName, sp.graphicsProperties.maxLayerCount);
        if (sp.graphicsProperties.maxLayerCount > 0) g_max_layers = (int)sp.graphicsProperties.maxLayerCount;
    }
    log_blend_modes();
    return 1;
}

static void pointer_visuals_init(void);
static int start(ID3D11Texture2D *bb) {
    ID3D11Texture2D_GetDevice(bb, &g_dev);
    ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(bb, &bd);
    logf_("XR: game back buffer %ux%u format %d", bd.Width, bd.Height, (int)bd.Format);

    char setting[MAX_PATH], cand[2][MAX_PATH];
    int ncand = 0;
    runtime_setting(setting, sizeof(setting));
    if (_stricmp(setting, "simulator") == 0) {
        if (find_simulator_manifest(cand[ncand], MAX_PATH)) ncand++;
    } else if (_stricmp(setting, "headset") == 0) {
        if (active_manifest(cand[ncand], MAX_PATH)) ncand++;
    } else if (setting[0] && _stricmp(setting, "auto") != 0) {
        strcpy_s(cand[ncand++], MAX_PATH, setting);
    } else { // auto: the headset first, the simulator when no headset is there
        if (active_manifest(cand[ncand], MAX_PATH)) ncand++;
        if (find_simulator_manifest(cand[ncand], MAX_PATH) &&
            (ncand == 0 || _stricmp(cand[ncand], cand[0]) != 0))
            ncand++;
    }
    if (!ncand) { logf_("XR: no OpenXR runtime found"); return 0; }
    load_operator_layer();
    int ok = 0;
    for (int c = 0; c < ncand && !ok; c++) {
        logf_("XR: trying runtime %s", cand[c]);
        ok = try_instance(cand[c]);
        if (!ok && c + 1 < ncand) logf_("XR: no headset on that runtime; falling back to the Meta XR Simulator");
    }
    if (!ok) return 0;

    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(p_xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req)); // required before xrCreateSession

    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    XR_CHECK(p_xrCreateSession(g_inst, &sci, &g_sess));

    // Standing play: prefer STAGE (origin on the floor) so the table can sit
    // at a real height; fall back to LOCAL (origin at the starting head).
    XrReferenceSpaceType spaces[8];
    uint32_t nspaces = 0;
    p_xrEnumerateReferenceSpaces(g_sess, 8, &nspaces, spaces);
    XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (uint32_t i = 0; i < nspaces; i++)
        if (spaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    g_floor_space = rsci.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE;
    rsci.poseInReferenceSpace.orientation.w = 1;
    XR_CHECK(p_xrCreateReferenceSpace(g_sess, &rsci, &g_space));
    logf_("XR: reference space %s", g_floor_space ? "STAGE (floor level)" : "LOCAL");
    XrReferenceSpaceCreateInfo vsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1;
    p_xrCreateReferenceSpace(g_sess, &vsci, &g_view_space);
    if (!setup_input()) logf_("XR: controller input unavailable");

    if (!pick_format(bd.Format)) return 0;
    XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swci.format = g_sw_fmt;
    swci.sampleCount = 1;
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv,
                                                         vcv)) && nv > 0) {
        g_rec_w = (int)vcv[0].recommendedImageRectWidth;
        g_rec_h = (int)vcv[0].recommendedImageRectHeight;
        // Test aid: NorthgardVR_settings.txt "render_size W H" renders at that
        // size instead (e.g. the Quest's 3072 3264 while on the simulator).
        char sp_[MAX_PATH], line[128];
        DWORD n_ = GetModuleFileNameA(NULL, sp_, MAX_PATH);
        while (n_ > 0 && sp_[n_ - 1] != '\\') n_--;
        sp_[n_] = 0;
        strcat_s(sp_, MAX_PATH, "NorthgardVR_settings.txt");
        FILE *sf = NULL;
        if (!fopen_s(&sf, sp_, "rb") && sf) {
            int rw_, rh_;
            while (fgets(line, sizeof(line), sf))
                if (sscanf_s(line, "render_size %d %d", &rw_, &rh_) == 2 && rw_ >= 256 && rh_ >= 256 && rw_ <= 4096 &&
                    rh_ <= 4096) {
                    logf_("XR: render size override %dx%d (recommended %dx%d)", rw_, rh_, g_rec_w, g_rec_h);
                    g_rec_w = rw_;
                    g_rec_h = rh_;
                }
            fclose(sf);
        }
        logf_("XR: recommended eye size %dx%d", g_rec_w, g_rec_h);
    }
    // Big enough for the eye size and for the game window as it is now.
    swci.width = g_sw_w = max(g_rec_w, (int)bd.Width);
    swci.height = g_sw_h = max(g_rec_h, (int)bd.Height);
    g_cp_w = (int)bd.Width;
    g_cp_h = (int)bd.Height;
    swci.faceCount = 1;
    swci.arraySize = 1;
    swci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_sess, &swci, &g_swap));
    for (int i = 0; i < 8; i++) g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_swap, 8, &g_nimages, (XrSwapchainImageBaseHeader *)g_images));
    for (int e = 0; e < 2; e++) {
        XR_CHECK(p_xrCreateSwapchain(g_sess, &swci, &g_eye_swap[e]));
        for (int i = 0; i < 8; i++) g_eye_images[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        XR_CHECK(p_xrEnumerateSwapchainImages(g_eye_swap[e], 8, &g_eye_nimages[e],
                                              (XrSwapchainImageBaseHeader *)g_eye_images[e]));
    }
    uint32_t nui = 0;
    XR_CHECK(p_xrCreateSwapchain(g_sess, &swci, &g_ui_swap));
    for (int i = 0; i < 8; i++) g_ui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_ui_swap, 8, &nui, (XrSwapchainImageBaseHeader *)g_ui_images));
    logf_("XR: session created; swapchains %dx%d format %d (%u images)", g_sw_w, g_sw_h, (int)g_sw_fmt, g_nimages);
    mr_init();
    pointer_visuals_init();
    xr_window_mirror(g_rec_w, g_rec_h);
    return 1;
}

// ------------------------------------------------------------ per frame
static void poll_events(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *e = (XrEventDataSessionStateChanged *)&ev;
            g_sess_state = e->state;
            logf_("XR: session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(p_xrBeginSession(g_sess, &bi))) { g_running = 1; logf_("XR: session running"); }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(g_sess);
                g_running = 0;
                logf_("XR: session stopped");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = 0;
                g_state = -1;
                logf_("XR: session ended by runtime; VR disabled until restart");
            }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// (defined further down)
static XrCompositionLayerQuad g_hit_q[15];
static XrSwapchain g_opt_swap = XR_NULL_HANDLE;
static int g_opt_open, g_opt_dirty = 1, g_opt_hover = -1, g_opt_ready;
static XrCompositionLayerQuad g_opt_quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
static int opt_hit(const float o[3], const float d[3], float *dist, int *row);
// Where the laser last met a HUD panel (for the tooltip panel).
static int g_tip_anchor_ok;
static float g_tip_anchor[3];
static XrQuaternionf g_tip_anchor_q;
static long g_tip_anchor_frame;
static int g_tip_qi = -1; // index of the tooltip quad in the last panel list (not a hit target)
static int g_nhit;
static xr_input g_last_in;
static int g_last_in_ok;
static int pointer_quads(XrCompositionLayerQuad *L, const float head[3], int table);
void xr_pointer_frame(const xr_input *in); // openxr_hdll.c

static int head_position(XrTime t, float out[3]) {
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (!g_view_space || XR_FAILED(p_xrLocateSpace(g_view_space, g_space, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return 0;
    out[0] = loc.pose.position.x; out[1] = loc.pose.position.y; out[2] = loc.pose.position.z;
    return 1;
}

static void submit(ID3D11Texture2D *bb) {
    XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(p_xrWaitFrame(g_sess, &fwi, &fs))) return;
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(p_xrBeginFrame(g_sess, &fbi))) return;

    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad lq[6];
    const XrCompositionLayerBaseHeader *layers[8];
    uint32_t nlayers = 0;

    if (fs.shouldRender && g_mr == 2) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&g_pt_comp;
    if (fs.shouldRender && g_mr == 3 && g_key_swap) {
        anchor_from_view(fs.predictedDisplayTime);
        g_key_quad.pose = panel_pose(6.0f, 0.1f); // 60 m square, 6 m out: fills the view around the menu
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&g_key_quad;
    }
    if (fs.shouldRender) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_SUCCEEDED(p_xrAcquireSwapchainImage(g_swap, &ai, &idx)) &&
            XR_SUCCEEDED(p_xrWaitSwapchainImage(g_swap, &wi))) {
            copy_bb(g_images[idx].texture, bb);
            p_xrReleaseSwapchainImage(g_swap, &ri);

            // A 2 m tall screen, 2.2 m in front of where the user's head was
            // when VR started, slightly below eye level. (Sized by height: the
            // game renders at the headset's near-square eye size.)
            float h = 2.0f, w = h * (float)g_cp_w / (float)g_cp_h;
            quad.layerFlags = 0; // opaque game image
            quad.space = g_space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_swap;
            quad.subImage.imageRect.extent.width = g_cp_w;
            quad.subImage.imageRect.extent.height = g_cp_h;
            anchor_from_view(fs.predictedDisplayTime);
            quad.pose = panel_pose(2.2f, 0.1f);
            quad.size.width = w;
            quad.size.height = h;
            layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quad;

            // Menus: the controller laser points at this screen like at a HUD
            // panel (hover, trigger = click, A = right click).
            g_hit_q[0] = quad;
            g_nhit = 1;
            xr_input in;
            g_fs.predictedDisplayTime = fs.predictedDisplayTime; // read_input locates the controllers at this time
            read_input(&in);
            static int logged;
            if (!logged) {
                logged = 1;
                logf_("XR: menu screen at (%.2f %.2f %.2f) %.2fx%.2f m", quad.pose.position.x, quad.pose.position.y,
                      quad.pose.position.z, quad.size.width, quad.size.height);
            }
            g_last_in = in;
            g_last_in_ok = 1;
            xr_pointer_frame(&in);
            float head[3];
            if (g_opt_open && g_opt_ready) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&g_opt_quad;
            if (head_position(fs.predictedDisplayTime, head)) {
                int nl = pointer_quads(lq, head, 0);
                for (int i = 0; i < nl; i++) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&lq[i];
            }
        }
    }

    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = mr_blend();
    fei.layerCount = nlayers;
    fei.layers = layers;
    XrResult r = p_xrEndFrame(g_sess, &fei);
    static int warned;
    if (XR_FAILED(r) && !warned) { warned = 1; logf_("XR: xrEndFrame failed (%d)", (int)r); }
}


// ------------------------------------------------------------ table mask
// The eyes are copied into their swapchains through a small pixel shader.
// For each pixel the view ray is intersected with the table plane; outside the
// round table it shows a wooden rim and then a dark background, so the map
// ends at the table's edge instead of running off into a black void.
int dio_table_info(float out[5]);
int xr_drag_rect(float a[3], float b[3]);
int dio_open_map(void);

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3D10Blob **, ID3D10Blob **);
static const char MASK_HLSL[] =
    "cbuffer C : register(b0) { float4 eyep; float4 q; float4 tn; float4 tab; float4 misc; float4 drag; float4 drag2; float4 key; };\n"
    "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv;\n"
    "  o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float3 qrot(float4 r, float3 v) { float3 t = 2 * cross(r.xyz, v); return v + r.w * t + cross(r.xyz, t); }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float3 d = normalize(qrot(q, float3(lerp(tn.x, tn.y, i.uv.x), lerp(tn.z, tn.w, i.uv.y), -1)));\n"
    "  float3 bg = lerp(float3(0.012, 0.014, 0.018), float3(0.05, 0.06, 0.075), saturate(d.y * 0.7 + 0.5));\n"
    "  float open = drag2.z, mr = drag2.w;\n"
    "  float4 bg4 = mr > 1.5 ? float4(key.rgb, 1) : mr > 0 ? float4(0, 0, 0, 0) : float4(bg, 1);\n" // mixed reality: the room shows through
    "  if (d.y > -1e-4) return open > 0 ? float4(tex.Sample(smp, i.uv * misc.zw).rgb, 1) : bg4;\n"
    "  float t = (eyep.w - eyep.y) / d.y;\n"
    "  float3 p = eyep.xyz + t * d;\n"
    "  float r = length(float2(p.x - tab.x, p.z - tab.y));\n"
    "  float R = tab.w, fade = misc.x, rim = misc.y;\n"
    "  float3 wood = float3(0.20, 0.10, 0.045) * (0.85 + 0.15 * sin(r * 900));\n"
    "  if (open <= 0 && r > R + rim) return bg4;\n"
    "  if (open <= 0 && r > R) return float4(wood, 1);\n"
    "  float3 col = tex.Sample(smp, i.uv * misc.zw).rgb;\n"
    "  if (drag2.x > 0) {\n" // drag-select box, in table-local coordinates
    "    float dx = p.x - tab.x, dz = p.z - tab.y, c = cos(tab.z), s = sin(tab.z);\n"
    "    float2 l = float2(c * dx - s * dz, s * dx + c * dz);\n"
    "    float2 lo = min(drag.xy, drag.zw), hi = max(drag.xy, drag.zw), b = drag2.yy;\n"
    "    bool inOuter = all(l >= lo - b) && all(l <= hi + b), inInner = all(l > lo + b) && all(l < hi - b);\n"
    "    if (inOuter && !inInner) col = float3(0.30, 0.85, 1.0);\n"
    "    else if (inInner) col = lerp(col, float3(0.30, 0.85, 1.0), 0.12);\n"
    "  }\n"
    "  return open > 0 ? float4(col, 1) : float4(lerp(wood, col, smoothstep(R, R - fade, r)), 1);\n"
    "}\n";

static ID3D11VertexShader *g_mvs;
static ID3D11PixelShader *g_mps;
static ID3D11Buffer *g_mcb;
static ID3D11SamplerState *g_msmp;
static ID3D11Texture2D *g_msrc;
static ID3D11ShaderResourceView *g_msrv;
static ID3D11RenderTargetView *g_mrtv[2][8];
static int g_mask_state; // 0 = not tried, 1 = ready, -1 = unavailable

static int mask_init(DXGI_FORMAT bbfmt) {
    if (g_mask_state) return g_mask_state > 0;
    g_mask_state = -1;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) { logf_("XR: table mask off (no d3dcompiler_47.dll)"); return 0; }
    ID3D10Blob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(compile(MASK_HLSL, sizeof(MASK_HLSL) - 1, "mask", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(compile(MASK_HLSL, sizeof(MASK_HLSL) - 1, "mask", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err))) {
        logf_("XR: table mask shader failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb),
                                               NULL, &g_mvs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb),
                                              NULL, &g_mps)))
        return 0;
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    D3D11_BUFFER_DESC cb = {128, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &cb, NULL, &g_mcb))) return 0;
    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_msmp))) return 0;
    // Source copy of the eye: typeless so it can be read as sRGB (decoded) and
    // written to the sRGB swapchain (encoded) without changing the colours.
    int bgra = bbfmt == DXGI_FORMAT_B8G8R8A8_UNORM || bbfmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = g_sw_w;
    td.Height = g_sw_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_msrc))) return 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC srd = {0};
    srd.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srd.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_msrc, &srd, &g_msrv))) return 0;
    g_mask_state = 1;
    logf_("XR: table mask ready");
    return 1;
}

// Draws eye `e` from the back buffer into swapchain image `idx` with the mask.
// Returns 0 (caller falls back to a plain copy) if the mask can't be used.
static int mask_copy(int e, uint32_t idx, ID3D11Texture2D *dst, ID3D11Texture2D *bb) {
    float tab[5];
    if (!dio_table_info(tab)) return 0;
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(bb, &bd);
    if ((int)bd.Width > g_sw_w || (int)bd.Height > g_sw_h || !mask_init(bd.Format)) return 0;
    if (!g_mrtv[e][idx]) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
        rd.Format = g_sw_fmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)dst, &rd, &g_mrtv[e][idx]))) {
            logf_("XR: table mask: no render target view on the swapchain image; mask off");
            g_mask_state = -1;
            return 0;
        }
    }
    D3D11_BOX box = {0, 0, 0, bd.Width, bd.Height, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_msrc, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);

    XrView *v = &g_views[e];
    float c[32] = {v->pose.position.x, v->pose.position.y, v->pose.position.z, tab[1],
                   v->pose.orientation.x, v->pose.orientation.y, v->pose.orientation.z, v->pose.orientation.w,
                   tanf(v->fov.angleLeft), tanf(v->fov.angleRight), tanf(v->fov.angleUp), tanf(v->fov.angleDown),
                   tab[0], tab[2], tab[3], tab[4],
                   0.03f, 0.035f, (float)bd.Width / g_sw_w, (float)bd.Height / g_sw_h,
                   0, 0, 0, 0, 0, 0.0025f, (float)(dio_open_map() && !g_mr), (float)(g_mr == 3 ? 2 : g_mr != 0),
                   g_key_lin[0], g_key_lin[1], g_key_lin[2], 1};
    float da[3], db[3];
    if (xr_drag_rect(da, db)) { // corners to table-local x/z
        float cy = cosf(tab[3]), sy = sinf(tab[3]);
        float ax = da[0] - tab[0], az = da[2] - tab[2], bx = db[0] - tab[0], bz = db[2] - tab[2];
        c[20] = cy * ax - sy * az; c[21] = sy * ax + cy * az;
        c[22] = cy * bx - sy * bz; c[23] = sy * bx + cy * bz;
        c[24] = 1;
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_mcb, 0, NULL, c, 0, 0);

    // Save what we touch; Heaps caches its bindings and must find them unchanged.
    ID3D11RenderTargetView *ortv[8] = {0};
    ID3D11DepthStencilView *odsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 8, ortv, &odsv);
    UINT nvp = 16;
    D3D11_VIEWPORT ovp[16];
    ID3D11DeviceContext_RSGetViewports(g_ctx, &nvp, ovp);
    ID3D11VertexShader *ovs = NULL;
    ID3D11PixelShader *ops = NULL;
    ID3D11DeviceContext_VSGetShader(g_ctx, &ovs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &ops, NULL, NULL);
    ID3D11InputLayout *oil = NULL;
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &oil);
    D3D11_PRIMITIVE_TOPOLOGY otop;
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &otop);
    ID3D11ShaderResourceView *osrv = NULL;
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &osrv);
    ID3D11SamplerState *osmp = NULL;
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &osmp);
    ID3D11Buffer *ocb = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &ocb);
    ID3D11BlendState *obs = NULL;
    FLOAT obf[4];
    UINT obm;
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &obs, obf, &obm);
    ID3D11DepthStencilState *odss = NULL;
    UINT oref;
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &odss, &oref);
    ID3D11RasterizerState *ors = NULL;
    ID3D11DeviceContext_RSGetState(g_ctx, &ors);

    D3D11_VIEWPORT vp = {0, 0, (FLOAT)bd.Width, (FLOAT)bd.Height, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_mrtv[e][idx], NULL);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_mvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_mps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_msrv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_msmp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_mcb);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);

    ID3D11ShaderResourceView *nul = NULL;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, osrv ? &osrv : &nul);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 8, ortv, odsv);
    ID3D11DeviceContext_RSSetViewports(g_ctx, nvp, ovp);
    ID3D11DeviceContext_VSSetShader(g_ctx, ovs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, ops, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, oil);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, otop);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &osmp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &ocb);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, obs, obf, obm);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, odss, oref);
    ID3D11DeviceContext_RSSetState(g_ctx, ors);
    for (int i = 0; i < 8; i++) if (ortv[i]) ID3D11RenderTargetView_Release(ortv[i]);
    if (odsv) ID3D11DepthStencilView_Release(odsv);
    if (ovs) ID3D11VertexShader_Release(ovs);
    if (ops) ID3D11PixelShader_Release(ops);
    if (oil) ID3D11InputLayout_Release(oil);
    if (osrv) ID3D11ShaderResourceView_Release(osrv);
    if (osmp) ID3D11SamplerState_Release(osmp);
    if (ocb) ID3D11Buffer_Release(ocb);
    if (obs) ID3D11BlendState_Release(obs);
    if (odss) ID3D11DepthStencilState_Release(odss);
    if (ors) ID3D11RasterizerState_Release(ors);
    g_cp_w = (int)bd.Width;
    g_cp_h = (int)bd.Height;
    return 1;
}


// ------------------------------------------------------------ UI layout
// NorthgardVR_ui_layout.txt (next to Northgard.exe) cuts HUD elements out of
// the UI image and places each on its own panel around the table. Re-read
// whenever the file changes. One panel per line:
//   name  anchor  x y w h   px py pz   yaw pitch   width_m
// anchor: two letters, l/c/r then t/m/b: the corner of the UI image the
// rectangle is measured from (x, y = its top-left, in UI pixels, may be
// negative). px py pz: panel centre in table space (metres; x right, y up,
// z towards you; origin = table centre). yaw/pitch in degrees (pitch -90 =
// lying flat, face up). Lines starting with # are ignored.
// Without the file the whole UI shows as one panel.
typedef struct { char name[32]; float ax, ay, x, y, w, h, p[3], yaw, pitch, width; int autobox; char win[192]; int rest; } ui_panel;

// Auto-sized panels. Some HUD elements change size (the build menu and unit
// tallies with the faction/units, the selection summary with what's
// selected), so fixed rectangles clip them or catch their neighbours. Every
// 15 UI frames a few 1-pixel lines of the UI image are copied to the CPU
// (double-buffered, so no stall) and the elements' edges are found from
// their alpha:
//   column near the right edge -> build menu / unit tallies / build button
//   row 40 px above the bottom -> selection summary cards and territory panel
//   columns through those      -> their tops
// Layout lines use @build @tally @sel @terr as the anchor for these.
enum { AB_BUILD, AB_TALLY, AB_SEL, AB_TERR, AB_N };
typedef struct { int x, y, w, h, ok; } uibox;
static uibox g_ab[AB_N];
static ID3D11Texture2D *g_pcol[2], *g_prow[2];
static int g_pW, g_pH, g_pidx, g_pready[2], g_pxs[2][3];
static int g_sel_mid = -1, g_terr_mid = -1;

typedef struct { int s, e; } run_t;
static int alpha_runs(const unsigned char *px, int stride, int n, run_t *out, int max) {
    int k = 0, in = 0, start = 0, gap = 0;
    for (int i = 0; i <= n; i++) {
        int on = i < n && px[(size_t)i * stride + 3] > 128;
        if (on) {
            if (!in) { in = 1; start = i; }
            gap = 0;
        } else if (in && ++gap > 6) { // allow tiny gaps inside a panel
            if (k < max) { out[k].s = start; out[k].e = i - gap; k++; }
            in = 0;
        }
        if (i == n && in && k < max) { out[k].s = start; out[k].e = n - 1; k++; }
    }
    return k;
}

static void probe_parse(int j) {
    D3D11_MAPPED_SUBRESOURCE mc, mr;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_pcol[j], 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mc)))
        return;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_prow[j], 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mr))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_pcol[j], 0);
        return;
    }
    int W = g_pW, H = g_pH;
    run_t r[32];
    const unsigned char *col = (const unsigned char *)mc.pData;
    // right column, from the bottom: build button, unit tallies, build menu
    int n = alpha_runs(col + 0, mc.RowPitch, H, r, 32);
    int found = 0;
    uibox rb[3] = {{0}};
    for (int i = n - 1; i >= 0 && found < 3; i--) {
        if (r[i].s < H / 3) break; // top bar etc.
        rb[found].x = W - 284; rb[found].w = 284;
        rb[found].y = r[i].s; rb[found].h = r[i].e - r[i].s + 1; rb[found].ok = 1;
        found++;
    }
    g_ab[AB_TALLY].ok = 0;
    if (found >= 2) { // tallies + button as one panel
        g_ab[AB_TALLY] = rb[1];
        g_ab[AB_TALLY].h = rb[0].y + rb[0].h - rb[1].y;
    }
    g_ab[AB_BUILD] = found >= 3 ? rb[2] : (uibox){0};
    // bottom row: left HUD | selection cards ... | territory | right column
    const unsigned char *row = (const unsigned char *)mr.pData;
    n = alpha_runs(row, 4, W, r, 32);
    int rcol = -1, first = 0;
    for (int i = 0; i < n; i++) if (r[i].e >= W - 40) rcol = i;
    if (n && r[0].s <= 40) first = 1; // left HUD
    int last = rcol >= 0 ? rcol - 1 : n - 1;
    g_ab[AB_SEL].ok = g_ab[AB_TERR].ok = 0;
    if (last >= first && rcol >= 0 && r[rcol].s - r[last].e < 40 && r[last].s > W / 2) { // territory next to the right column
        g_ab[AB_TERR] = (uibox){r[last].s, 0, r[last].e - r[last].s + 1, 0, 1};
        last--;
    }
    if (last >= first) {
        g_ab[AB_SEL] = (uibox){r[first].s, 0, r[last].e - r[first].s + 1, 0, 1};
    }
    // their tops, from the probe columns used for this copy
    for (int c = 1; c <= 2; c++) {
        uibox *b = &g_ab[c == 1 ? AB_SEL : AB_TERR];
        int x = g_pxs[j][c];
        if (!b->ok || x < b->x || x >= b->x + b->w) { b->ok = 0; continue; } // probe missed: next round
        int m = alpha_runs(col + 4 * c, mc.RowPitch, H, r, 32);
        b->ok = 0;
        for (int i = m - 1; i >= 0; i--)
            if (r[i].e >= H - 48) { b->y = r[i].s; b->h = H - r[i].s; b->ok = 1; break; }
    }
    if (g_ab[AB_SEL].w) g_sel_mid = g_ab[AB_SEL].x + 60;
    if (g_ab[AB_TERR].w) g_terr_mid = g_ab[AB_TERR].x + g_ab[AB_TERR].w / 2;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_pcol[j], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_prow[j], 0);
}

static void ui_probe(ID3D11Texture2D *t) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    if (!g_pcol[0] || (int)d.Width != g_pW || (int)d.Height != g_pH) {
        for (int i = 0; i < 2; i++) {
            if (g_pcol[i]) ID3D11Texture2D_Release(g_pcol[i]);
            if (g_prow[i]) ID3D11Texture2D_Release(g_prow[i]);
            g_pcol[i] = g_prow[i] = NULL;
            g_pready[i] = 0;
        }
        D3D11_TEXTURE2D_DESC sd = {0};
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < 2; i++) {
            sd.Width = 3; sd.Height = d.Height;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &g_pcol[i]))) return;
            sd.Width = d.Width; sd.Height = 1;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &g_prow[i]))) return;
        }
        g_pW = (int)d.Width;
        g_pH = (int)d.Height;
    }
    static int k;
    if (++k % 15) return;
    int i = g_pidx, j = i ^ 1;
    if (g_pready[j]) probe_parse(j); // copied 15 frames ago: ready without waiting
    int xs[3] = {g_pW - 140, g_sel_mid >= 0 ? g_sel_mid : g_pW / 2 - 300,
                 g_terr_mid >= 0 ? g_terr_mid : g_pW - 450};
    for (int c = 0; c < 3; c++) {
        int x = xs[c] < 0 ? 0 : xs[c] >= g_pW ? g_pW - 1 : xs[c];
        D3D11_BOX box = {(UINT)x, 0, 0, (UINT)x + 1, (UINT)g_pH, 1};
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_pcol[i], 0, c, 0, 0, (ID3D11Resource *)t, 0, &box);
        g_pxs[i][c] = x;
    }
    D3D11_BOX rbx = {0, (UINT)(g_pH - 40), 0, (UINT)g_pW, (UINT)(g_pH - 39), 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_prow[i], 0, 0, 0, 0, (ID3D11Resource *)t, 0, &rbx);
    g_pready[i] = 1;
    g_pidx = j;
}
static ui_panel g_panels[15];
static char g_claims[512]; // "claim" lines: window classes no panel shows (and @rest skips)
static int g_npanels = -1; // -1 = no layout file
static FILETIME g_layout_time;

static void load_layout(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') n--;
    path[n] = 0;
    strcat_s(path, MAX_PATH, "NorthgardVR_ui_layout.txt");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) { g_npanels = -1; return; }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_layout_time) == 0) return;
    g_layout_time = fa.ftLastWriteTime;
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") || !f) return;
    char line[512];
    int k = 0;
    g_claims[0] = 0;
    while (k < 15 && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        if (!strncmp(line, "claim ", 6)) { // claim cls1,cls2  (windows to leave out)
            char c[400];
            if (sscanf_s(line + 6, "%399s", c, 400) == 1) {
                if (g_claims[0]) strcat_s(g_claims, sizeof(g_claims), ",");
                strcat_s(g_claims, sizeof(g_claims), c);
            }
            continue;
        }
        ui_panel q;
        char anchor[200];
        memset(&q, 0, sizeof(q));
        if (sscanf_s(line, "%31s %199s %f %f %f %f %f %f %f %f %f %f", q.name, 32, anchor, 200, &q.x, &q.y, &q.w, &q.h,
                     &q.p[0], &q.p[1], &q.p[2], &q.yaw, &q.pitch, &q.width) != 12)
            continue;
        q.autobox = -1;
        if (!strncmp(anchor, "@win:", 5)) {
            strcpy_s(q.win, sizeof(q.win), anchor + 5);
        } else if (!strcmp(anchor, "@rest")) {
            q.rest = 1;
        } else if (anchor[0] == '@') {
            const char *nm[AB_N] = {"@build", "@tally", "@sel", "@terr"};
            for (int a = 0; a < AB_N; a++) if (!strcmp(anchor, nm[a])) q.autobox = a;
            if (q.autobox < 0) continue;
        }
        q.ax = anchor[0] == 'r' ? 1.f : anchor[0] == 'c' ? 0.5f : 0.f;
        q.ay = anchor[1] == 'b' ? 1.f : anchor[1] == 'm' ? 0.5f : 0.f;
        g_panels[k++] = q;
    }
    fclose(f);
    g_npanels = k;
    logf_("UI layout: %d panels", k);
}

static XrQuaternionf quat_yaw_pitch(float yaw, float pitch) {
    float cy = cosf(yaw / 2), sy = sinf(yaw / 2), cp = cosf(pitch / 2), sp = sinf(pitch / 2);
    // q = qYaw(about Y) * qPitch(about X)
    XrQuaternionf q = {cy * sp, sy * cp, -sy * sp, cy * cp};
    return q;
}

// Last submitted panels, for pointer hit tests: g_hit_q / g_nhit (declared above).

// Ray (stage space) against the HUD panels. On a hit returns 1 with the UI
// pixel (engine pixels) and the distance along the ray.
int xrt_panel_hit(const float o[3], const float d[3], float *ux, float *uy, float *dist) {
    float best = 1e9f;
    int hit = 0;
    int besti = -1;
    for (int i = 0; i < g_nhit; i++) {
        if (i == g_tip_qi) continue;
        XrCompositionLayerQuad *L = &g_hit_q[i];
        XrQuaternionf q = L->pose.orientation;
        // basis vectors of the quad: q * (1,0,0), q * (0,1,0), q * (0,0,1)
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z,
              wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        float ax[3] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
        float ay[3] = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
        float az[3] = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
        float dn = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
        if (fabsf(dn) < 1e-5f) continue;
        float pd[3] = {L->pose.position.x - o[0], L->pose.position.y - o[1], L->pose.position.z - o[2]};
        float t = (pd[0] * az[0] + pd[1] * az[1] + pd[2] * az[2]) / dn;
        if (t <= 0 || t >= best) continue;
        float h[3] = {o[0] + t * d[0] - L->pose.position.x, o[1] + t * d[1] - L->pose.position.y,
                      o[2] + t * d[2] - L->pose.position.z};
        float u = (h[0] * ax[0] + h[1] * ax[1] + h[2] * ax[2]) / L->size.width + 0.5f;
        float v = 0.5f - (h[0] * ay[0] + h[1] * ay[1] + h[2] * ay[2]) / L->size.height;
        if (u < 0 || u > 1 || v < 0 || v > 1) continue;
        best = t;
        besti = i;
        *ux = L->subImage.imageRect.offset.x + u * L->subImage.imageRect.extent.width;
        *uy = L->subImage.imageRect.offset.y + v * L->subImage.imageRect.extent.height;
        hit = 1;
    }
    if (hit) {
        *dist = best;
        g_tip_anchor[0] = o[0] + best * d[0];
        g_tip_anchor[1] = o[1] + best * d[1];
        g_tip_anchor[2] = o[2] + best * d[2];
        g_tip_anchor_q = g_hit_q[besti].pose.orientation;
        g_tip_anchor_frame = g_stereo_frames;
        g_tip_anchor_ok = 1;
    }
    return hit;
}

// ---- panels from the game's own UI windows (xr_uiwin.c)
// "a,b*,c": exact class names, or prefixes ending in '*'.
static int pattern_match(const char *pat, const char *cls) {
    const char *p = pat;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '*') { if (!strncmp(cls, p, n - 1)) return 1; }
        else if (n == strlen(cls) && !strncmp(cls, p, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
typedef struct { int ok, x, y, w, h; } prect;
// Resolve every window panel's rect: exact patterns first, then wildcards,
// then @rest (the union of visible windows nobody claimed).
static void resolve_windows(prect *pr) {
    const xrw_win *w;
    int n = xrw_windows(&w);
    int claimed[XRW_MAX] = {0};
    for (int i = 0; i < n; i++)
        if (g_claims[0] && pattern_match(g_claims, w[i].cls)) claimed[i] = 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int k = 0; k < g_npanels; k++) {
            ui_panel *q = &g_panels[k];
            int wild = q->win[0] && strchr(q->win, '*') != NULL;
            if (pass == 0 && !(q->win[0] && !wild)) continue;
            if (pass == 1 && !wild) continue;
            if (pass == 2 && !q->rest) continue;
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
            for (int i = 0; i < n; i++) {
                if (claimed[i]) continue;
                int m = q->rest ? strcmp(w[i].cls, "tip") != 0 : pattern_match(q->win, w[i].cls);
                if (!m) continue;
                claimed[i] = 1;
                if (!w[i].vis) continue;
                if (w[i].x < x0) x0 = w[i].x;
                if (w[i].y < y0) y0 = w[i].y;
                if (w[i].x + w[i].w > x1) x1 = w[i].x + w[i].w;
                if (w[i].y + w[i].h > y1) y1 = w[i].y + w[i].h;
            }
            pr[k].ok = x1 > x0 && y1 > y0;
            pr[k].x = x0; pr[k].y = y0; pr[k].w = x1 - x0; pr[k].h = y1 - y0;
        }
    }
}


// Fills quad layers for the layout panels. Returns how many were added.
static int layout_quads(XrCompositionLayerQuad *quads, int max) {
    float tab[5];
    if (g_npanels <= 0 || !dio_table_info(tab)) return 0;
    float c = cosf(tab[3]), s = sinf(tab[3]);
    float k = tab[4] / 0.75f; // layout positions are for a 0.75 m table: scale them with its size
    int nq = 0;
    prect pr[15];
    memset(pr, 0, sizeof(pr));
    resolve_windows(pr);
    g_tip_qi = -1;
    for (int i = 0; i < g_npanels && nq < max; i++) {
        ui_panel *q = &g_panels[i];
        int rx = (int)(q->ax * g_cp_w + q->x), ry = (int)(q->ay * g_cp_h + q->y);
        int rw = (int)q->w, rh = q->h < 0 ? (int)(g_cp_h + q->h) - ry : (int)q->h; // h < 0: up to H - |h|
        int is_win = q->win[0] || q->rest;
        if (is_win) {
            if (!pr[i].ok) continue; // not on screen right now
            rx = pr[i].x - 4; ry = pr[i].y - 4; rw = pr[i].w + 8; rh = pr[i].h + 8;
        } else if (q->autobox >= 0) {
            uibox *b = &g_ab[q->autobox];
            if (!b->ok || b->w <= 0 || b->h <= 0) continue; // not on screen right now
            rx = b->x - 4; ry = b->y - 4; rw = b->w + 8; rh = b->h + 8;
        }
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > g_cp_w) rw = g_cp_w - rx;
        if (ry + rh > g_cp_h) rh = g_cp_h - ry;
        if (rw <= 0 || rh <= 0) continue;
        XrCompositionLayerQuad *L = &quads[nq++];
        memset(L, 0, sizeof(*L));
        L->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        L->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        L->space = g_space;
        L->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        L->subImage.swapchain = g_ui_swap;
        L->subImage.imageRect.offset.x = rx;
        L->subImage.imageRect.offset.y = ry;
        L->subImage.imageRect.extent.width = rw;
        L->subImage.imageRect.extent.height = rh;
        // keep the panels' distance from the near edge (10 cm in front of you)
        float lx = q->p[0] * k, ly = q->p[1], lz = q->p[2] * k;
        L->pose.position.x = tab[0] + c * lx + s * lz;
        L->pose.position.y = tab[1] + ly;
        L->pose.position.z = tab[2] - s * lx + c * lz;
        L->pose.orientation = quat_yaw_pitch(tab[3] + q->yaw * 0.0174533f, q->pitch * 0.0174533f);
        if (is_win && !strcmp(q->win, "tip")) g_tip_qi = nq - 1;
        if (is_win && !strcmp(q->win, "tip") && g_tip_anchor_ok && g_stereo_frames - g_tip_anchor_frame < 30) {
            // tooltip: just above where the laser meets a panel, facing the same way, 3 cm in front
            float mpp = q->width / (q->w > 0 ? q->w : 280);
            L->size.width = rw * mpp;
            L->size.height = rh * mpp;
            XrQuaternionf o = g_tip_anchor_q;
            float upx = 2 * (o.x * o.y - o.w * o.z), upy = 1 - 2 * (o.x * o.x + o.z * o.z), upz = 2 * (o.y * o.z + o.w * o.x);
            float nx = 2 * (o.x * o.z + o.w * o.y), ny = 2 * (o.y * o.z - o.w * o.x), nz = 1 - 2 * (o.x * o.x + o.y * o.y);
            float lift = L->size.height / 2 + 0.03f;
            L->pose.orientation = o;
            L->pose.position.x = g_tip_anchor[0] + upx * lift + nx * 0.03f;
            L->pose.position.y = g_tip_anchor[1] + upy * lift + ny * 0.03f;
            L->pose.position.z = g_tip_anchor[2] + upz * lift + nz * 0.03f;
            continue;
        }
        if (q->autobox >= 0 || is_win) {
            // same scale as the nominal width (metres per UI pixel); the line's
            // position is the panel's bottom centre, so it grows upwards
            float mpp = q->width / (q->w > 0 ? q->w : 280);
            L->size.width = rw * mpp;
            L->size.height = rh * mpp;
            XrQuaternionf o = L->pose.orientation;
            float upx = 2 * (o.x * o.y - o.w * o.z), upy = 1 - 2 * (o.x * o.x + o.z * o.z), upz = 2 * (o.y * o.z + o.w * o.x);
            L->pose.position.x += upx * L->size.height / 2;
            L->pose.position.y += upy * L->size.height / 2;
            L->pose.position.z += upz * L->size.height / 2;
        } else {
            L->size.width = q->width;
            L->size.height = q->width * rh / rw;
        }
    }
    return nq;
}


// ------------------------------------------------------------ laser + cursor
// A beam from the right controller and a dot where it hits a HUD panel or the
// table, as two small quad layers on top of everything. Texture (128x64):
// left half = dot, right half = beam cross-section.
static XrSwapchain g_ptr_swap;
static XrSwapchainImageD3D11KHR g_ptr_images[8];
static ID3D11Texture2D *g_ptr_tex;
static void pointer_visuals_init(void) {
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_sw_fmt;
    ci.sampleCount = 1;
    ci.width = 128;
    ci.height = 64;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(p_xrCreateSwapchain(g_sess, &ci, &g_ptr_swap))) { g_ptr_swap = XR_NULL_HANDLE; return; }
    uint32_t n = 0;
    for (int i = 0; i < 8; i++) g_ptr_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    p_xrEnumerateSwapchainImages(g_ptr_swap, 8, &n, (XrSwapchainImageBaseHeader *)g_ptr_images);
    static unsigned char px[64][128][4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++) {
            float a, r, g, b;
            if (x < 64) { // dot: white core, cyan ring, soft edge
                float d = sqrtf((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f));
                a = d < 26 ? 1.f : d < 31 ? (31 - d) / 5 : 0.f;
                float core = d < 12 ? 1.f : d < 16 ? (16 - d) / 4 : 0.f;
                r = core + (1 - core) * 0.15f; g = core + (1 - core) * 0.85f; b = 1.f;
            } else { // beam: bright centre line, soft falloff across
                float dx = (x - 95.5f) / 12.f;
                a = expf(-dx * dx) * 0.9f;
                r = 0.55f; g = 0.9f; b = 1.f;
            }
            px[y][x][0] = (unsigned char)(255 * r * a); // premultiplied
            px[y][x][1] = (unsigned char)(255 * g * a);
            px[y][x][2] = (unsigned char)(255 * b * a);
            px[y][x][3] = (unsigned char)(255 * a);
        }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = 128;
    td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)g_sw_fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {px, 128 * 4, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &g_ptr_tex))) g_ptr_tex = NULL;
    logf_("XR: pointer laser %s", g_ptr_tex ? "ready" : "unavailable");
}

static XrQuaternionf quat_from_basis(const float X[3], const float Y[3], const float Z[3]) {
    float m00 = X[0], m11 = Y[1], m22 = Z[2], tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q.w = 0.25f * s; q.x = (Y[2] - Z[1]) / s; q.y = (Z[0] - X[2]) / s; q.z = (X[1] - Y[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q.w = (Y[2] - Z[1]) / s; q.x = 0.25f * s; q.y = (Y[0] + X[1]) / s; q.z = (Z[0] + X[2]) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q.w = (Z[0] - X[2]) / s; q.x = (Y[0] + X[1]) / s; q.y = 0.25f * s; q.z = (Z[1] + Y[2]) / s;
    } else {
        float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q.w = (X[1] - Y[0]) / s; q.x = (Z[0] + X[2]) / s; q.y = (Z[1] + Y[2]) / s; q.z = 0.25f * s;
    }
    return q;
}
static void v_norm(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
static void v_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

// Adds the beam (and the dot if it hits something). Returns layers added.

// ------------------------------------------------------------ VR options menu
// Left thumbstick click opens/closes a small panel in front of you; point the
// laser at a row and pull the trigger to change it. Changes apply at once and
// are saved in NorthgardVR_settings.txt.
int dio_open_map(void);
void dio_set_open_map(int on);
#define OPT_W 640
#define OPT_H 420
#define OPT_ROWS 4
#define OPT_ROW_Y0 86
#define OPT_ROW_H 78
static const float OPT_WM = 0.48f; // metres wide

static void settings_set(const char *key, const char *val) {
    char sp_[MAX_PATH], line[256];
    static char out[8192];
    DWORD n_ = GetModuleFileNameA(NULL, sp_, MAX_PATH);
    while (n_ > 0 && sp_[n_ - 1] != '\\') n_--;
    sp_[n_] = 0;
    strcat_s(sp_, MAX_PATH, "NorthgardVR_settings.txt");
    out[0] = 0;
    size_t kl = strlen(key);
    int done = 0;
    FILE *f = NULL;
    if (!fopen_s(&f, sp_, "rb") && f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '\t')) {
                if (!done) { strcat_s(out, sizeof(out), key); strcat_s(out, sizeof(out), " "); strcat_s(out, sizeof(out), val); strcat_s(out, sizeof(out), "\r\n"); done = 1; }
                continue;
            }
            if (strlen(out) + strlen(line) + 4 < sizeof(out)) {
                strcat_s(out, sizeof(out), line);
                size_t l = strlen(out);
                if (l && out[l - 1] != '\n') strcat_s(out, sizeof(out), "\r\n");
            }
        }
        fclose(f);
    }
    if (!done) { strcat_s(out, sizeof(out), key); strcat_s(out, sizeof(out), " "); strcat_s(out, sizeof(out), val); strcat_s(out, sizeof(out), "\r\n"); }
    f = NULL;
    if (!fopen_s(&f, sp_, "wb") && f) { fputs(out, f); fclose(f); }
}

static const struct { const char *name; unsigned char rgb[3]; } KEYS[] = {
    {"Magenta", {255, 0, 255}}, {"Green", {0, 255, 0}}, {"Blue", {0, 0, 255}}};
static int key_index(void) {
    for (int i = 0; i < 3; i++)
        if (!memcmp(KEYS[i].rgb, g_key_rgb, 3)) return i;
    return -1;
}
static void set_key(int i) {
    for (int c = 0; c < 3; c++) {
        g_key_rgb[c] = KEYS[i].rgb[c];
        double v = g_key_rgb[c] / 255.0;
        g_key_lin[c] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
    char b[8];
    sprintf_s(b, sizeof(b), "%u", g_key_rgb[0]); settings_set("mixed_reality_key_r", b);
    sprintf_s(b, sizeof(b), "%u", g_key_rgb[1]); settings_set("mixed_reality_key_g", b);
    sprintf_s(b, sizeof(b), "%u", g_key_rgb[2]); settings_set("mixed_reality_key_b", b);
    key_backdrop_fill();
}
static int runtime_passthrough(void) { return g_alpha_blend || g_pt_layer != XR_NULL_HANDLE; }

static void opt_row_text(int r, char *label, char *value, size_t n) {
    label[0] = value[0] = 0;
    switch (r) {
    case 0:
        strcpy_s(label, n, "Mixed reality");
        strcpy_s(value, n, g_mr == 0 ? "Off" : g_mr == 3 ? "Chroma key (Virtual Desktop)" : "Passthrough");
        break;
    case 1: {
        int k = key_index();
        strcpy_s(label, n, "Key colour");
        if (k >= 0) strcpy_s(value, n, KEYS[k].name);
        else sprintf_s(value, n, "%u,%u,%u", g_key_rgb[0], g_key_rgb[1], g_key_rgb[2]);
        break;
    }
    case 2:
        strcpy_s(label, n, "Map");
        strcpy_s(value, n, g_mr ? "Round table (mixed reality)" : dio_open_map() ? "Open map" : "Round table");
        break;
    case 3: strcpy_s(label, n, "Close"); break;
    }
}

// Draw the menu with GDI and upload it to the panel's swapchain.
static void opt_render(void) {
    if (!g_opt_swap) return;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = OPT_W;
    bi.bmiHeader.biHeight = -OPT_H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bm);
    RECT all = {0, 0, OPT_W, OPT_H};
    HBRUSH bg = CreateSolidBrush(RGB(24, 30, 38));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    HBRUSH edge = CreateSolidBrush(RGB(110, 140, 170));
    FrameRect(dc, &all, edge);
    SetBkMode(dc, TRANSPARENT);
    HFONT ft = CreateFontA(40, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HFONT fr = CreateFontA(30, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HFONT fs = CreateFontA(22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    SelectObject(dc, ft);
    SetTextColor(dc, RGB(235, 225, 200));
    RECT tr = {24, 18, OPT_W - 24, 70};
    DrawTextA(dc, "NorthgardVR options", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int r = 0; r < OPT_ROWS; r++) {
        RECT rr = {16, OPT_ROW_Y0 + r * OPT_ROW_H, OPT_W - 16, OPT_ROW_Y0 + r * OPT_ROW_H + OPT_ROW_H - 10};
        HBRUSH rb = CreateSolidBrush(r == g_opt_hover ? RGB(60, 92, 128) : RGB(36, 46, 58));
        FillRect(dc, &rr, rb);
        DeleteObject(rb);
        FrameRect(dc, &rr, edge);
        char label[64], value[64];
        opt_row_text(r, label, value, sizeof(label));
        SelectObject(dc, fr);
        SetTextColor(dc, RGB(230, 230, 230));
        RECT lr = rr;
        lr.left += 16;
        DrawTextA(dc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, RGB(140, 210, 255));
        RECT vr = rr;
        vr.right -= 16;
        DrawTextA(dc, value, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(170, 170, 160));
    RECT hr = {24, OPT_ROW_Y0 + OPT_ROWS * OPT_ROW_H - 4, OPT_W - 24, OPT_H - 6};
    char hint[160];
    if (g_mr == 3) {
        int k = key_index();
        sprintf_s(hint, sizeof(hint), "Virtual Desktop > Streaming > VR Passthrough on, key colour %s",
                  k >= 0 ? KEYS[k].name : "as set");
    } else {
        strcpy_s(hint, sizeof(hint), "Trigger: change   Left stick click: close");
    }
    DrawTextA(dc, hint, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    // BGRA (GDI) -> RGBA, opaque
    unsigned char *px = (unsigned char *)bits;
    for (int i = 0; i < OPT_W * OPT_H; i++) {
        unsigned char b = px[i * 4];
        px[i * 4] = px[i * 4 + 2];
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    XrSwapchainImageD3D11KHR img[4];
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, img[i].next = NULL;
    p_xrEnumerateSwapchainImages(g_opt_swap, 4, &n, (XrSwapchainImageBaseHeader *)img);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (n && XR_SUCCEEDED(p_xrAcquireSwapchainImage(g_opt_swap, &ai, &idx)) &&
        XR_SUCCEEDED(p_xrWaitSwapchainImage(g_opt_swap, &wi))) {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)img[idx].texture, 0, NULL, px, OPT_W * 4, 0);
        p_xrReleaseSwapchainImage(g_opt_swap, &ri);
        g_opt_ready = 1;
    }
    SelectObject(dc, old);
    DeleteObject(ft);
    DeleteObject(fr);
    DeleteObject(fs);
    DeleteObject(edge);
    DeleteObject(bm);
    DeleteDC(dc);
    g_opt_dirty = 0;
}

static void opt_place(void) {
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (!g_view_space || XR_FAILED(p_xrLocateSpace(g_view_space, g_space, g_fs.predictedDisplayTime, &loc))) return;
    XrQuaternionf q = loc.pose.orientation;
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) { fx = 0; fz = -1; fl = 1; }
    fx /= fl; fz /= fl;
    float yaw = atan2f(-fx, -fz);
    g_opt_quad.pose.position.x = loc.pose.position.x + fx * 0.6f;
    g_opt_quad.pose.position.y = loc.pose.position.y - 0.12f;
    g_opt_quad.pose.position.z = loc.pose.position.z + fz * 0.6f;
    g_opt_quad.pose.orientation = quat_yaw_pitch(yaw, -0.15f);
}

static void opt_init(void) {
    if (g_opt_swap) return;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g_sw_fmt;
    ci.sampleCount = 1;
    ci.width = OPT_W;
    ci.height = OPT_H;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(p_xrCreateSwapchain(g_sess, &ci, &g_opt_swap))) { g_opt_swap = XR_NULL_HANDLE; return; }
    g_opt_quad.space = g_space;
    g_opt_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_opt_quad.subImage.swapchain = g_opt_swap;
    g_opt_quad.subImage.imageRect.extent.width = OPT_W;
    g_opt_quad.subImage.imageRect.extent.height = OPT_H;
    g_opt_quad.size.width = OPT_WM;
    g_opt_quad.size.height = OPT_WM * OPT_H / OPT_W;
}

// Ray against the menu: distance along the ray and the row (-1 between rows).
static int opt_hit(const float o[3], const float d[3], float *dist, int *row) {
    if (!g_opt_open || !g_opt_ready) return 0;
    XrQuaternionf q = g_opt_quad.pose.orientation;
    float ax[3] = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.w * q.z), 2 * (q.x * q.z - q.w * q.y)};
    float ay[3] = {2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x)};
    float az[3] = {2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
    float dn = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
    if (fabsf(dn) < 1e-5f) return 0;
    float pd[3] = {g_opt_quad.pose.position.x - o[0], g_opt_quad.pose.position.y - o[1], g_opt_quad.pose.position.z - o[2]};
    float t = (pd[0] * az[0] + pd[1] * az[1] + pd[2] * az[2]) / dn;
    if (t <= 0) return 0;
    float h[3] = {o[0] + t * d[0] - g_opt_quad.pose.position.x, o[1] + t * d[1] - g_opt_quad.pose.position.y,
                  o[2] + t * d[2] - g_opt_quad.pose.position.z};
    float u = (h[0] * ax[0] + h[1] * ax[1] + h[2] * ax[2]) / g_opt_quad.size.width + 0.5f;
    float v = 0.5f - (h[0] * ay[0] + h[1] * ay[1] + h[2] * ay[2]) / g_opt_quad.size.height;
    if (u < 0 || u > 1 || v < 0 || v > 1) return 0;
    *dist = t;
    int py = (int)(v * OPT_H) - OPT_ROW_Y0;
    *row = (py >= 0 && py % OPT_ROW_H < OPT_ROW_H - 10 && py / OPT_ROW_H < OPT_ROWS) ? py / OPT_ROW_H : -1;
    return 1;
}

static void opt_activate(int row) {
    switch (row) {
    case 0: { // Off -> Chroma key -> Passthrough (if the runtime has it) -> Off
        int next = g_mr == 0 ? 3 : (g_mr == 3 && runtime_passthrough()) ? (g_alpha_blend ? 1 : 2) : 0;
        if (next == 3) key_backdrop_init();
        g_mr = next;
        settings_set("mixed_reality", next == 0 ? "0" : next == 3 ? "2" : "1");
        logf_("options: mixed reality -> %s", next == 0 ? "off" : next == 3 ? "chroma key" : "passthrough");
        break;
    }
    case 1: {
        int k = key_index();
        set_key(k < 0 ? 0 : (k + 1) % 3);
        logf_("options: key colour -> %u,%u,%u", g_key_rgb[0], g_key_rgb[1], g_key_rgb[2]);
        break;
    }
    case 2:
        if (!g_mr) {
            dio_set_open_map(!dio_open_map());
            settings_set("open_map", dio_open_map() ? "1" : "0");
            logf_("options: open map -> %d", dio_open_map());
        }
        break;
    case 3: g_opt_open = 0; break;
    }
    g_opt_dirty = 1;
}

int xrt_options_input(const xr_input *in) {
    static int prev_click, prev_trig;
    int click = in->stick_click[0];
    if (click && !prev_click) {
        opt_init();
        g_opt_open = !g_opt_open;
        if (g_opt_open) { opt_place(); g_opt_dirty = 1; }
        logf_("options menu %s", g_opt_open ? "opened" : "closed");
    }
    prev_click = click;
    int trig = in->trigger[1] > 0.6f;
    int edge = trig && !prev_trig;
    prev_trig = trig;
    if (!g_opt_open || !in->aim_valid[1]) return 0;
    if (g_opt_dirty) opt_render();
    float o[3] = {in->aim_pos[1][0], in->aim_pos[1][1], in->aim_pos[1][2]};
    float qx = in->aim_quat[1][0], qy = in->aim_quat[1][1], qz = in->aim_quat[1][2], qw = in->aim_quat[1][3];
    float d[3] = {-2 * (qx * qz + qw * qy), -2 * (qy * qz - qw * qx), -(1 - 2 * (qx * qx + qy * qy))};
    float dist;
    int row;
    if (!opt_hit(o, d, &dist, &row)) {
        if (g_opt_hover != -1) { g_opt_hover = -1; g_opt_dirty = 1; }
        return 0;
    }
    if (row != g_opt_hover) { g_opt_hover = row; g_opt_dirty = 1; if (row >= 0) xrt_haptic(1, 0.15f, 15); }
    if (edge && row >= 0) { opt_activate(row); xrt_haptic(1, 0.35f, 25); }
    if (g_opt_dirty) opt_render();
    return 1;
}

static int pointer_quads(XrCompositionLayerQuad *L, const float head[3], int table) {
    if (!g_ptr_swap || !g_ptr_tex || !g_last_in_ok || !g_last_in.aim_valid[1]) return 0;
    const xr_input *in = &g_last_in;
    float o[3] = {in->aim_pos[1][0], in->aim_pos[1][1], in->aim_pos[1][2]};
    float qx = in->aim_quat[1][0], qy = in->aim_quat[1][1], qz = in->aim_quat[1][2], qw = in->aim_quat[1][3];
    float d[3] = {-2 * (qx * qz + qw * qy), -2 * (qy * qz - qw * qx), -(1 - 2 * (qx * qx + qy * qy))};
    v_norm(d);
    float len = 1.5f, ux, uy, dist;
    int hit = 0;
    if (xrt_panel_hit(o, d, &ux, &uy, &dist)) { len = dist; hit = 1; }
    { float od; int orow; if (opt_hit(o, d, &od, &orow) && (!hit || od < len)) { len = od; hit = 1; } }
    float tab[5];
    if (!hit && table && dio_table_info(tab) && d[1] < -1e-3f) {
        float t = (tab[1] - o[1]) / d[1];
        float hx = o[0] + t * d[0] - tab[0], hz = o[2] + t * d[2] - tab[2];
        if (t > 0 && hx * hx + hz * hz < (tab[4] + 0.04f) * (tab[4] + 0.04f)) { len = t; hit = 1; }
    }
    int active = in->trigger_touch[1] || in->trigger[1] > 0.05f || in->btn_lower[1];

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_ptr_swap, &ai, &idx))) return 0;
    if (XR_FAILED(p_xrWaitSwapchainImage(g_ptr_swap, &wi))) return 0;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ptr_images[idx].texture, (ID3D11Resource *)g_ptr_tex);
    p_xrReleaseSwapchainImage(g_ptr_swap, &ri);

    int n = 0;
    // beam: quad Y along the ray, Z (normal) towards the head
    float c[3] = {o[0] + d[0] * len / 2, o[1] + d[1] * len / 2, o[2] + d[2] * len / 2};
    float toh[3] = {head[0] - c[0], head[1] - c[1], head[2] - c[2]}, X[3], Z[3];
    v_cross(d, toh, X);
    v_norm(X);
    v_cross(X, d, Z);
    memset(&L[n], 0, sizeof(L[n]));
    L[n].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    L[n].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    L[n].space = g_space;
    L[n].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    L[n].subImage.swapchain = g_ptr_swap;
    L[n].subImage.imageRect.offset.x = 64;
    L[n].subImage.imageRect.extent.width = 64;
    L[n].subImage.imageRect.extent.height = 64;
    L[n].pose.position.x = c[0]; L[n].pose.position.y = c[1]; L[n].pose.position.z = c[2];
    L[n].pose.orientation = quat_from_basis(X, d, Z);
    L[n].size.width = active ? 0.009f : 0.005f;
    L[n].size.height = len;
    n++;
    if (hit) { // dot facing the head, a few mm in front of the surface
        float p[3] = {o[0] + d[0] * (len - 0.004f), o[1] + d[1] * (len - 0.004f), o[2] + d[2] * (len - 0.004f)};
        float Zd[3] = {head[0] - p[0], head[1] - p[1], head[2] - p[2]}, up[3] = {0, 1, 0}, Xd[3], Yd[3];
        v_norm(Zd);
        v_cross(up, Zd, Xd);
        v_norm(Xd);
        v_cross(Zd, Xd, Yd);
        memset(&L[n], 0, sizeof(L[n]));
        L[n].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        L[n].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        L[n].space = g_space;
        L[n].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        L[n].subImage.swapchain = g_ptr_swap;
        L[n].subImage.imageRect.extent.width = 64;
        L[n].subImage.imageRect.extent.height = 64;
        L[n].pose.position.x = p[0]; L[n].pose.position.y = p[1]; L[n].pose.position.z = p[2];
        L[n].pose.orientation = quat_from_basis(Xd, Yd, Zd);
        float sz = (active ? 0.018f : 0.012f) * (0.5f + 0.5f * len);
        L[n].size.width = L[n].size.height = sz;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------- stereo
static int copy_to(XrSwapchain sc, XrSwapchainImageD3D11KHR *imgs, ID3D11Texture2D *bb, int eye) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(p_xrAcquireSwapchainImage(sc, &ai, &idx))) return 0;
    if (XR_FAILED(p_xrWaitSwapchainImage(sc, &wi))) return 0;
    int ok = (eye >= 0 && mask_copy(eye, idx, imgs[idx].texture, bb)) || copy_bb(imgs[idx].texture, bb);
    p_xrReleaseSwapchainImage(sc, &ri);
    return ok;
}

int xr_stereo_toggle(void) { g_stereo = !g_stereo; return g_stereo; }
// Phase 6: the size the game should render at while VR runs (the headset's
// recommended eye size, capped). 0 before the session exists.
int xrt_rt_size(int *w, int *h) {
    if (g_state != 1 || g_rec_w <= 0 || g_rec_h <= 0) return 0;
    double s = 1.0;
    if (g_rec_w > 4096) s = 4096.0 / g_rec_w;
    if (g_rec_h * s > 4096) s = 4096.0 / g_rec_h;
    *w = (int)(g_rec_w * s);
    *h = (int)(g_rec_h * s);
    return 1;
}
// Phase 4: the UI pass's offscreen target (UI on transparent, premultiplied).
int xrt_ui_capture(void) { return g_frame_open && g_frame_render && g_eye == 2 && g_ui_panel; }
void xrt_ui_target(void *res) {
    ID3D11Texture2D *t = (ID3D11Texture2D *)res;
    if (!t || !xrt_ui_capture()) return;
    if (g_stereo_frames == 120) probe("UI target", t);
    // NorthgardVR_dump.flag: dump the UI image now (for measuring popups).
    static int k;
    if (++k % 30 == 0) {
        char f[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, f, MAX_PATH);
        while (n > 0 && f[n - 1] != '\\') n--;
        f[n] = 0;
        strcat_s(f, MAX_PATH, "NorthgardVR_dump.flag");
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(f);
            dump_texture(t, "NorthgardVR_ui.raw");
        }
    }
    g_ui_copied = copy_to(g_ui_swap, g_ui_images, t, -1);
    ui_probe(t);
}
int xrt_running(void) { return g_state == 1 && g_running; }
int xrt_stereo_on(void) { return g_state == 1 && g_stereo && g_running; }

// Called before the game renders a frame. In stereo mode: wait for the
// headset, begin the frame and locate both eyes. Returns 1 if the caller
// should render the left eye now.
int xr_game_camera_live(void); // openxr_hdll.c: map camera updated recently?

int xrt_stereo_begin(void) {
    if (g_state != 1 || !g_stereo || !xr_game_camera_live()) return 0;
    poll_events();
    if (!g_running || g_frame_open) return 0;
    XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
    g_fs.type = XR_TYPE_FRAME_STATE;
    g_fs.next = NULL;
    if (XR_FAILED(p_xrWaitFrame(g_sess, &fwi, &g_fs))) return 0;
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(p_xrBeginFrame(g_sess, &fbi))) return 0;
    g_frame_open = 1;
    g_frame_render = 0;
    g_eye_copied[0] = g_eye_copied[1] = 0;
    g_ui_copied = 0;
    if (!g_fs.shouldRender) return 0;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_fs.predictedDisplayTime;
    li.space = g_space;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    g_views[0].type = g_views[1].type = XR_TYPE_VIEW;
    g_views[0].next = g_views[1].next = NULL;
    if (XR_FAILED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, g_views)) || n != 2) return 0;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return 0;
    g_frame_render = 1;
    g_eye = 0;
    // Head = midpoint of the eyes, orientation of the left eye.
    xr_input in;
    read_input(&in);
    g_last_in = in;
    g_last_in_ok = 1;
    float head[3] = {(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                     (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                     (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};
    float hq[4] = {g_views[0].pose.orientation.x, g_views[0].pose.orientation.y, g_views[0].pose.orientation.z,
                   g_views[0].pose.orientation.w};
    dio_frame(&in, head, hq, (double)g_fs.predictedDisplayPeriod * 1e-9);
    return 1;
}

// After the left eye was rendered into the back buffer: copy it, switch to the
// right eye (rendered by the game's normal render call that follows).
// Desktop mirror: the window shows the left eye (the UI pass would leave it
// black apart from the HUD). A copy of the left eye is put into the back
// buffer right before the frame is presented.
static ID3D11Texture2D *g_mirror;
static void mirror_keep(ID3D11Texture2D *bb) {
    D3D11_TEXTURE2D_DESC d, m;
    ID3D11Texture2D_GetDesc(bb, &d);
    if (g_mirror) {
        ID3D11Texture2D_GetDesc(g_mirror, &m);
        if (m.Width != d.Width || m.Height != d.Height || m.Format != d.Format) {
            ID3D11Texture2D_Release(g_mirror);
            g_mirror = NULL;
        }
    }
    if (!g_mirror) {
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.CPUAccessFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_mirror))) { g_mirror = NULL; return; }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_mirror, (ID3D11Resource *)bb);
}
static void mirror_show(ID3D11Texture2D *bb) {
    if (!g_mirror) return;
    D3D11_TEXTURE2D_DESC d, m;
    ID3D11Texture2D_GetDesc(bb, &d);
    ID3D11Texture2D_GetDesc(g_mirror, &m);
    if (m.Width == d.Width && m.Height == d.Height && m.Format == d.Format)
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)bb, (ID3D11Resource *)g_mirror);
}

void xrt_eye_done(void *backbuffer) {
    ID3D11Texture2D *bb = (ID3D11Texture2D *)backbuffer;
    if (!bb) return;
    if (g_eye == 0 || g_eye == 1) {
        g_eye_copied[g_eye] = copy_to(g_eye_swap[g_eye], g_eye_images[g_eye], bb, g_eye);
        if (g_eye == 0) mirror_keep(bb);
        if (g_stereo_frames == 120) probe(g_eye == 0 ? "left eye" : "right eye", bb);
        g_eye = g_eye == 0 ? 1 : -2;
    }
    ID3D11Texture2D_Release(bb);
}

// Before the UI-only pass: clear the back buffer to transparent black so the
// UI can be lifted out with its alpha. Returns 1 if the pass should run.
int xrt_ui_begin(void *backbuffer) {
    ID3D11Texture2D *bb = (ID3D11Texture2D *)backbuffer;
    int run = 0;
    if (bb && g_frame_open && g_frame_render && g_eye == -2) {
        ID3D11RenderTargetView *rtv = NULL;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &rtv))) {
            const float clear[4] = {0, 0, 0, 0};
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, clear);
            ID3D11RenderTargetView_Release(rtv);
        }
        g_eye = 2;
        run = 1;
    }
    if (bb) ID3D11Texture2D_Release(bb);
    return run;
}

// Game.onRender guards: eyes draw the world only, the UI pass draws the UI only.
int xrt_draw_3d(void) { return g_eye != 2; }
int xrt_draw_2d(void) { return g_eye != 0 && g_eye != 1; }

// Eye currently being rendered (for the camera hook). Returns 0 if none.
int xr_current_eye(float pos[3], float quat[4], float tan_lrud[4]) {
    if (g_eye != 0 && g_eye != 1) return 0;
    XrView *v = &g_views[g_eye];
    pos[0] = v->pose.position.x; pos[1] = v->pose.position.y; pos[2] = v->pose.position.z;
    quat[0] = v->pose.orientation.x; quat[1] = v->pose.orientation.y;
    quat[2] = v->pose.orientation.z; quat[3] = v->pose.orientation.w;
    tan_lrud[0] = tanf(v->fov.angleLeft); tan_lrud[1] = tanf(v->fov.angleRight);
    tan_lrud[2] = tanf(v->fov.angleUp); tan_lrud[3] = tanf(v->fov.angleDown);
    return 1;
}

static void end_stereo_frame(ID3D11Texture2D *bb) {
    int size_ok = bb_fits(bb);
    if (g_frame_render && g_eye == 1 && size_ok) // right eye not copied yet (no UI pass)
        g_eye_copied[1] = copy_to(g_eye_swap[1], g_eye_images[1], bb, 1);
    if (g_frame_render && g_eye == 2 && size_ok) { // back buffer holds the UI pass
        // the UI itself was captured with alpha by xrt_ui_target during the pass
    }
    if (g_frame_render && g_eye == 2) mirror_show(bb); // the UI was already captured from its own target
    if (g_frame_render) g_stereo_frames++;

    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad pquads[15];
    const XrCompositionLayerBaseHeader *layers[24];
    XrCompositionLayerQuad lquads[6];
    int nlaser = 0;
    uint32_t nlayers = 0;
    if (g_stereo_frames % 60 == 0) load_layout();
    if (g_frame_render && g_mr == 2) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&g_pt_comp;
    if (g_frame_render && g_eye_copied[0] && g_eye_copied[1]) {
        for (int e = 0; e < 2; e++) {
            memset(&pv[e], 0, sizeof(pv[e]));
            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[e].pose = g_views[e].pose;
            pv[e].fov = g_views[e].fov;
            pv[e].subImage.swapchain = g_eye_swap[e];
            pv[e].subImage.imageRect.extent.width = g_cp_w;
            pv[e].subImage.imageRect.extent.height = g_cp_h;
        }
        proj.space = g_space;
        if (g_mr == 1 || g_mr == 2) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // room shows where alpha is 0
        proj.viewCount = 2;
        proj.views = pv;
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&proj;
    }
    int budget = g_max_layers - 1 - 2 - (g_mr == 2) - 1; // projection + laser/dot (+ passthrough) + options menu
    if (budget > 15) budget = 15;
    int npq = (g_frame_render && g_ui_copied && budget > 0) ? layout_quads(pquads, budget) : 0;
    for (int i = 0; i < npq; i++) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&pquads[i];
    memcpy(g_hit_q, pquads, sizeof(XrCompositionLayerQuad) * npq);
    g_nhit = npq;
    if (g_frame_render && nlayers) {
        float head[3] = {(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                         (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                         (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};
        nlaser = pointer_quads(lquads, head, 1);
    }
    if (g_frame_render && g_ui_copied && g_npanels < 0) {
        // UI panel: 1.9 m wide, 1.5 m ahead and a little below eye level of the
        // starting head position; alpha-blended over the world.
        float w = 1.9f, h = w * (float)g_cp_h / (float)g_cp_w;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_ui_swap;
        quad.subImage.imageRect.extent.width = g_cp_w;
        quad.subImage.imageRect.extent.height = g_cp_h;
        anchor_from_view(g_fs.predictedDisplayTime);
        quad.pose = panel_pose(1.5f, 0.25f);
        quad.size.width = w;
        quad.size.height = h;
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quad;
    }
    if (g_opt_open && g_opt_ready && g_frame_render) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&g_opt_quad;
    for (int i = 0; i < nlaser; i++) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&lquads[i]; // on top
    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = g_fs.predictedDisplayTime;
    fei.environmentBlendMode = mr_blend();
    fei.layerCount = nlayers;
    fei.layers = layers;
    XrResult r = p_xrEndFrame(g_sess, &fei);
    static int warned;
    if (XR_FAILED(r) && !warned) { warned = 1; logf_("XR: stereo xrEndFrame failed (%d)", (int)r); }
    static int logged;
    if (nlayers && !logged) {
        logged = 1;
        logf_("XR: stereo layers: projection=%d ui_panel=%d", nlayers > 0 && proj.viewCount == 2, g_ui_copied);
        logf_("XR: first stereo frame submitted; L fov(%.1f %.1f %.1f %.1f) deg",
              g_views[0].fov.angleLeft * 57.2958f, g_views[0].fov.angleRight * 57.2958f,
              g_views[0].fov.angleUp * 57.2958f, g_views[0].fov.angleDown * 57.2958f);
    }
    g_frame_open = 0;
    g_frame_render = 0;
    g_eye = -1;
}

// Called by the patched bytecode before every present, with the game's back
// buffer (a new reference from IDXGISwapChain::GetBuffer, released here).
void xr_theater_frame(void *backbuffer) {
    ID3D11Texture2D *bb = (ID3D11Texture2D *)backbuffer;
    if (!bb) return;
    if (g_state == 0) {
        g_state = start(bb) ? 1 : -1;
        if (g_state < 0) logf_("XR: VR disabled for this run; the game continues on the monitor");
    }
    {
        static UINT lw, lh;
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(bb, &d);
        if (d.Width != lw || d.Height != lh) {
            lw = d.Width;
            lh = d.Height;
            logf_("XR: game back buffer now %ux%u", lw, lh);
        }
    }
    if (g_state == 1 && g_frame_open) {
        end_stereo_frame(bb);
    } else if (g_state == 1) {
        poll_events();
        if (g_running) {
            if (bb_fits(bb)) submit(bb);
            else {
                static int warned;
                if (!warned) { warned = 1; logf_("XR: back buffer grew past the swapchain size; theater paused"); }
            }
        }
    }
    ID3D11Texture2D_Release(bb);
}
