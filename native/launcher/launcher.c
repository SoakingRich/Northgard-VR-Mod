// NorthgardVR.exe - starts Northgard in VR.
// Lives in the Northgard game folder next to Northgard.exe, hlpatch.exe and the mod DLL.
//  1. If vrboot.dat is missing, or hlboot.dat changed since the last patch (game update),
//     run hlpatch.exe to make a fresh vrboot.dat from the player's own game files.
//  2. Start "Northgard.exe vrboot.dat".
// Playing from Steam's Play button stays the normal game. Messages go to NorthgardVR_launcher_log.txt.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include <stdarg.h>

static char g_dir[MAX_PATH];
static FILE *g_log;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}
static void in_dir(const char *name, char *out) { sprintf_s(out, MAX_PATH, "%s\\%s", g_dir, name); }
static int exists(const char *name) { char p[MAX_PATH]; in_dir(name, p); return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

// "size mtime" of hlboot.dat, identifies the game version that vrboot.dat was made from
static int game_stamp(char *out, size_t n) {
    char p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    in_dir("hlboot.dat", p);
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return 0;
    sprintf_s(out, n, "%lu %lu %lu", fa.nFileSizeLow, fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
    return 1;
}
static void read_file_line(const char *name, char *out, size_t n) {
    char p[MAX_PATH];
    FILE *f = NULL;
    out[0] = 0;
    in_dir(name, p);
    if (fopen_s(&f, p, "rb") || !f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\r' || out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
}
static void write_file_line(const char *name, const char *text) {
    char p[MAX_PATH];
    FILE *f = NULL;
    in_dir(name, p);
    if (!fopen_s(&f, p, "wb") && f) { fputs(text, f); fclose(f); }
}

// Run hlpatch.exe; its output goes to NorthgardVR_patch_log.txt. Returns 1 on success.
static int run_patcher(void) {
    char lib[64], cmd[2 * MAX_PATH + 64], outp[MAX_PATH];
    read_file_line("NorthgardVR_lib.txt", lib, sizeof(lib)); // DLL name without .hdll (default "openxr")
    if (!lib[0]) strcpy_s(lib, sizeof(lib), "openxr");
    SetEnvironmentVariableA("XR_LIB", lib);
    sprintf_s(cmd, sizeof(cmd), "\"%s\\hlpatch.exe\" patch --phase1 hlboot.dat vrboot.new", g_dir);
    in_dir("NorthgardVR_patch_log.txt", outp);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE out = CreateFileA(outp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi;
    logf_("patching: %s (XR_LIB=%s)", cmd, lib);
    char tmp[MAX_PATH]; in_dir("vrboot.new", tmp); DeleteFileA(tmp);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi)) {
        logf_("cannot start hlpatch.exe (%lu)", GetLastError());
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    logf_("hlpatch exit code %lu", code);
    if (code != 0) return 0;
    char dst[MAX_PATH]; in_dir("vrboot.dat", dst);
    if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) { logf_("cannot replace vrboot.dat (%lu)", GetLastError()); return 0; }
    return 1;
}

static int start_game(int vr) {
    char cmd[MAX_PATH + 32];
    sprintf_s(cmd, sizeof(cmd), "\"%s\\Northgard.exe\"%s", g_dir, vr ? " vrboot.dat" : "");
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    logf_("starting: %s", cmd);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        logf_("cannot start Northgard.exe (%lu)", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cl, int show) {
    (void)h; (void)p; (void)cl; (void)show;
    DWORD n = GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    while (n > 0 && g_dir[n - 1] != '\\') n--;
    if (n) g_dir[n - 1] = 0;
    char lp[MAX_PATH];
    in_dir("NorthgardVR_launcher_log.txt", lp);
    g_log = _fsopen(lp, "w", _SH_DENYNO);
    logf_("NorthgardVR launcher in %s", g_dir);
    if (!exists("Northgard.exe") || !exists("hlboot.dat")) {
        MessageBoxA(NULL, "NorthgardVR.exe must be in the Northgard game folder (next to Northgard.exe).", "NorthgardVR", MB_ICONERROR);
        return 1;
    }
    char stamp[96], last[96];
    game_stamp(stamp, sizeof(stamp));
    read_file_line("NorthgardVR_patched.txt", last, sizeof(last));
    int need = !exists("vrboot.dat") || strcmp(stamp, last) != 0;
    logf_("game stamp '%s', last patched '%s': %s", stamp, last, need ? "patch needed" : "up to date");
    if (need) {
        int ok = exists("hlpatch.exe") && run_patcher();
        if (ok) {
            write_file_line("NorthgardVR_patched.txt", stamp);
            logf_("vrboot.dat ready");
        } else {
            int r = MessageBoxA(NULL,
                "NorthgardVR could not prepare this version of Northgard for VR\n"
                "(see NorthgardVR_patch_log.txt in the game folder).\n\n"
                "This usually means Northgard was updated and the mod needs an update too.\n\n"
                "Start the normal (non-VR) game instead?", "NorthgardVR", MB_ICONWARNING | MB_YESNO);
            logf_("patch failed; user chose %s", r == IDYES ? "normal game" : "quit");
            return r == IDYES ? !start_game(0) : 1;
        }
    }
    return !start_game(1);
}
