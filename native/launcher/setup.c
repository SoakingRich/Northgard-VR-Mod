// NorthgardVR_Setup.exe - installs / uninstalls NorthgardVR.
// Run from the folder the release zip was extracted to (the files to install sit next to it).
//   NorthgardVR_Setup.exe              install (asks before starting the game)
//   NorthgardVR_Setup.exe /uninstall   remove the installed files and shortcuts (settings are kept)
//   /quiet                             no dialogs (results go to NorthgardVR_setup_log.txt)
// Finds Northgard (Steam app 466560) through Steam's registry entry and libraryfolders.vdf.
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include <stdarg.h>

static char g_src[MAX_PATH], g_game[MAX_PATH];
static int g_quiet;
static FILE *g_log;

// Files the installer copies and the uninstaller removes. Settings (NorthgardVR_settings.txt) are never touched.
static const char *FILES[] = {"NorthgardVR.exe", "hlpatch.exe", "openxr.hdll", "NorthgardVR_ui_layout.txt",
                              "NorthgardVR_notify_rules.txt", "NorthgardVR_sky_stormy.raw", "NorthgardVR_sky_dusk.raw"};
#define NFILES (int)(sizeof(FILES) / sizeof(FILES[0]))
// Files the mod creates while running (removed on uninstall)
static const char *MADE[] = {"vrboot.dat", "NorthgardVR_lib.txt", "NorthgardVR_patched.txt", "NorthgardVR_launcher_log.txt",
                             "NorthgardVR_patch_log.txt", "NorthgardVR_log.txt"};
#define NMADE (int)(sizeof(MADE) / sizeof(MADE[0]))

static void logf_(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    if (g_log) { vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log); }
    va_end(a);
}
static void say(const char *msg, UINT icon) {
    logf_("%s", msg);
    if (!g_quiet) MessageBoxA(NULL, msg, "NorthgardVR Setup", icon);
}
static int file_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static const char *del_msg(const char *p) { // delete a file; say what happened
    static char m[64];
    if (DeleteFileA(p)) return "ok";
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return "not there";
    sprintf_s(m, sizeof(m), "FAILED (error %lu)", e);
    return m;
}

// Steam library folders: Steam's own folder, then every "path" in steamapps\libraryfolders.vdf.
static int find_game(void) {
    char steam[MAX_PATH] = "";
    DWORD sz = sizeof(steam);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS) {
        sz = sizeof(steam);
        RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath", RRF_RT_REG_SZ, NULL, steam, &sz);
    }
    for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
    logf_("Steam: %s", steam[0] ? steam : "(not found)");
    char libs[16][MAX_PATH];
    int nl = 0;
    if (steam[0]) strcpy_s(libs[nl++], MAX_PATH, steam);
    char vdf[MAX_PATH];
    sprintf_s(vdf, sizeof(vdf), "%s\\steamapps\\libraryfolders.vdf", steam);
    FILE *f = NULL;
    if (steam[0] && !fopen_s(&f, vdf, "rb") && f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && nl < 16) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *q1 = strchr(k + 6, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            char path[MAX_PATH];
            int o = 0;
            for (char *r = q1 + 1; r < q2 && o < MAX_PATH - 1; r++) {
                if (r[0] == '\\' && r[1] == '\\') r++;
                path[o++] = *r;
            }
            path[o] = 0;
            strcpy_s(libs[nl++], MAX_PATH, path);
        }
        fclose(f);
    }
    for (int i = 0; i < nl; i++) {
        char exe[MAX_PATH];
        sprintf_s(g_game, sizeof(g_game), "%s\\steamapps\\common\\Northgard", libs[i]);
        sprintf_s(exe, sizeof(exe), "%s\\Northgard.exe", g_game);
        logf_("checking %s", exe);
        if (file_exists(exe)) return 1;
    }
    g_game[0] = 0;
    return 0;
}

static int make_shortcut(int csidl, const char *target, const char *dir) {
    char folder[MAX_PATH], lnk[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, csidl, NULL, 0, folder))) return 0;
    sprintf_s(lnk, sizeof(lnk), "%s\\Northgard VR.lnk", folder);
    IShellLinkA *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) {
        IShellLinkA_SetPath(sl, target);
        IShellLinkA_SetWorkingDirectory(sl, dir);
        IShellLinkA_SetDescription(sl, "Northgard in VR (NorthgardVR mod)");
        char icon[MAX_PATH];
        sprintf_s(icon, sizeof(icon), "%s\\Northgard.exe", dir);
        IShellLinkA_SetIconLocation(sl, icon, 0);
        if (SUCCEEDED(IShellLinkA_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            WCHAR w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, lnk, -1, w, MAX_PATH);
            ok = SUCCEEDED(IPersistFile_Save(pf, w, TRUE));
            IPersistFile_Release(pf);
        }
        IShellLinkA_Release(sl);
    }
    logf_("shortcut %s: %s", lnk, ok ? "ok" : "FAILED");
    return ok;
}
static void remove_shortcut(int csidl) {
    char folder[MAX_PATH], lnk[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, csidl, NULL, 0, folder))) return;
    sprintf_s(lnk, sizeof(lnk), "%s\\Northgard VR.lnk", folder);
    logf_("remove %s: %s", lnk, del_msg(lnk));
}

static int install(void) {
    int missing = 0;
    for (int i = 0; i < NFILES; i++) {
        char s[MAX_PATH], d[MAX_PATH];
        sprintf_s(s, sizeof(s), "%s\\%s", g_src, FILES[i]);
        sprintf_s(d, sizeof(d), "%s\\%s", g_game, FILES[i]);
        if (!file_exists(s)) { logf_("missing in the release folder: %s", FILES[i]); missing++; continue; }
        BOOL ok = CopyFileA(s, d, FALSE);
        logf_("copy %s: %s", FILES[i], ok ? "ok" : "FAILED");
        if (!ok) {
            char m[512];
            sprintf_s(m, sizeof(m), "Could not copy %s into\n%s\n\nIs Northgard running? Close it and run Setup again.", FILES[i], g_game);
            say(m, MB_ICONERROR);
            return 0;
        }
    }
    if (missing) { say("Some mod files are missing next to NorthgardVR_Setup.exe. Extract the whole zip and run Setup again.", MB_ICONERROR); return 0; }
    char lib[MAX_PATH];
    sprintf_s(lib, sizeof(lib), "%s\\NorthgardVR_lib.txt", g_game);
    FILE *f = NULL;
    if (!fopen_s(&f, lib, "wb") && f) { fputs("openxr", f); fclose(f); }
    char patched[MAX_PATH]; // force a fresh vrboot.dat for this mod version on the next start
    sprintf_s(patched, sizeof(patched), "%s\\NorthgardVR_patched.txt", g_game);
    DeleteFileA(patched);
    char exe[MAX_PATH];
    sprintf_s(exe, sizeof(exe), "%s\\NorthgardVR.exe", g_game);
    CoInitialize(NULL);
    make_shortcut(CSIDL_DESKTOPDIRECTORY, exe, g_game);
    make_shortcut(CSIDL_PROGRAMS, exe, g_game);
    CoUninitialize();
    return 1;
}

static void uninstall(void) {
    for (int i = 0; i < NFILES; i++) {
        char d[MAX_PATH];
        sprintf_s(d, sizeof(d), "%s\\%s", g_game, FILES[i]);
        logf_("remove %s: %s", FILES[i], del_msg(d));
    }
    for (int i = 0; i < NMADE; i++) {
        char d[MAX_PATH];
        sprintf_s(d, sizeof(d), "%s\\%s", g_game, MADE[i]);
        logf_("remove %s: %s", MADE[i], del_msg(d));
    }
    remove_shortcut(CSIDL_DESKTOPDIRECTORY);
    remove_shortcut(CSIDL_PROGRAMS);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cl, int show) {
    (void)h; (void)p; (void)show;
    g_quiet = strstr(cl, "/quiet") != NULL;
    int un = strstr(cl, "/uninstall") != NULL;
    DWORD n = GetModuleFileNameA(NULL, g_src, MAX_PATH);
    while (n > 0 && g_src[n - 1] != '\\') n--;
    if (n) g_src[n - 1] = 0;
    char lp[MAX_PATH];
    sprintf_s(lp, sizeof(lp), "%s\\NorthgardVR_setup_log.txt", g_src);
    g_log = _fsopen(lp, "w", _SH_DENYNO);
    logf_("NorthgardVR Setup (%s) from %s", un ? "uninstall" : "install", g_src);
    if (!find_game()) {
        say("Northgard (Steam) was not found.\n\nInstall Northgard through Steam first, then run Setup again.", MB_ICONERROR);
        return 1;
    }
    logf_("Northgard: %s", g_game);
    if (un) {
        uninstall();
        say("NorthgardVR was removed. Your NorthgardVR settings file was kept.\nNorthgard itself is unchanged.", MB_ICONINFORMATION);
        return 0;
    }
    if (!install()) return 1;
    logf_("installed");
    if (!g_quiet) {
        char m[MAX_PATH + 256];
        sprintf_s(m, sizeof(m),
                  "NorthgardVR is installed in\n%s\n\nStart \"Northgard VR\" from the desktop or Start menu shortcut.\n"
                  "Steam's Play button still starts the normal game.\n\nStart Northgard VR now?", g_game);
        if (MessageBoxA(NULL, m, "NorthgardVR Setup", MB_ICONINFORMATION | MB_YESNO) == IDYES) {
            char exe[MAX_PATH];
            sprintf_s(exe, sizeof(exe), "%s\\NorthgardVR.exe", g_game);
            ShellExecuteA(NULL, "open", exe, NULL, g_game, SW_SHOWNORMAL);
        }
    }
    return 0;
}
