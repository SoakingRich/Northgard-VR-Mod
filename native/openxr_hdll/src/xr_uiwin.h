#pragma once
// HUD windows read from the game's UI (see xr_uiwin.c).
#define XRW_MAX 64
typedef struct {
    char cls[64];   // HashLink class, e.g. "ui.win.MiniMap"
    int x, y, w, h; // on-screen rect in UI pixels
    int vis;        // shown on screen right now
} xrw_win;
int xrw_windows(const xrw_win **out);
void xr_ui_windows_impl(void *ui);
