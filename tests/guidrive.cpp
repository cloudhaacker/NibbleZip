// Test helper: drives the NibbleZip window (clicks buttons, answers message boxes) for automated GUI checks.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HWND g_dlg;
static BOOL CALLBACK findDlg(HWND h, LPARAM) {
    wchar_t cls[64]; GetClassNameW(h, cls, 63);
    if ((!wcscmp(cls, L"#32770") || !wcscmp(cls, L"NibbleZipBox")) && IsWindowVisible(h)) { g_dlg = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK dumpChild(HWND h, LPARAM) {
    wchar_t t[2048]; GetWindowTextW(h, t, 2047);
    char u[8192]; WideCharToMultiByte(CP_UTF8, 0, t, -1, u, 8191, 0, 0);
    if (u[0]) printf("  | %s\n", u);
    return TRUE;
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "sleep")) { Sleep(atoi(argv[++i])); continue; }
        if (!strcmp(argv[i], "click")) {
            int id = atoi(argv[++i]); HWND w = FindWindowW(L"NibbleZipMainWindow", 0);
            if (!w) { printf("no main window\n"); return 1; }
            PostMessageW(w, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(w, id));
            continue;
        }
        if (!strcmp(argv[i], "close")) { HWND w = FindWindowW(L"NibbleZipMainWindow", 0); if (w) PostMessageW(w, WM_CLOSE, 0, 0); continue; }
        if (!strcmp(argv[i], "combo")) {   // combo <id> <index>
            int id = atoi(argv[++i]), idx = atoi(argv[++i]); HWND w = FindWindowW(L"NibbleZipMainWindow", 0);
            SendMessageW(GetDlgItem(w, id), CB_SETCURSEL, idx, 0);
            SendMessageW(w, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), (LPARAM)GetDlgItem(w, id));
            continue;
        }
        if (!strcmp(argv[i], "settext")) {   // settext <id> <text>
            int id = atoi(argv[++i]); HWND w = FindWindowW(L"NibbleZipMainWindow", 0);
            wchar_t t[512]; MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, t, 511);
            SetWindowTextW(GetDlgItem(w, id), t);
            continue;
        }
        if (!strcmp(argv[i], "dialog")) {   // dialog <buttonId> : wait for a message box, print its text, press the button
            int id = atoi(argv[++i]); g_dlg = 0;
            for (int t = 0; t < 600 && !g_dlg; t++) { EnumWindows(findDlg, 0); if (!g_dlg) Sleep(100); }
            if (!g_dlg) { printf("no dialog appeared\n"); return 1; }
            printf("dialog:\n"); EnumChildWindows(g_dlg, dumpChild, 0); fflush(stdout);
            if (id >= 0) { Sleep(300); PostMessageW(g_dlg, WM_COMMAND, id, 0); Sleep(500); }
            continue;
        }
        if (!strcmp(argv[i], "status")) {
            HWND w = FindWindowW(L"NibbleZipMainWindow", 0); if (!w) { printf("no main window\n"); continue; }
            printf("main window:\n"); EnumChildWindows(w, dumpChild, 0); fflush(stdout);
        }
    }
    return 0;
}
