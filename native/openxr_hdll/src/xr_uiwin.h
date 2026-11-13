#pragma once
// HUD windows read from the game's UI (see xr_uiwin.c).
#define XRW_MAX 64
typedef struct {
    char cls[64];   // HashLink class, e.g. "ui.win.MiniMap"
    int x, y, w, h; // on-screen rect in UI pixels
    int vis;        // shown on screen right now
    int fs;         // has a full-screen backdrop (left out of the rect)
    int nflows;     // flows counted in the rect
} xrw_win;
int xrw_windows(const xrw_win **out);
#ifdef _WIN32
#define XRW_EXPORT __declspec(dllexport)
#else
#define XRW_EXPORT
#endif
XRW_EXPORT void xr_ui_windows_impl(void *ui);
XRW_EXPORT void xr_game_event_impl(void *ev);
XRW_EXPORT void xr_weather_impl(void *w);
