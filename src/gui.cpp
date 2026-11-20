// NibbleZip Windows GUI.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "platform.h"

static const wchar_t* kClass = L"NibbleZipMainWindow";
static const wchar_t* kTitle = L"NibbleZip";
static const wchar_t* kVersion = L"0.9.2";
static const DWORD kDisclaimerVersion = 2;   // raise to show the disclaimer again after its text changes
static const wchar_t* kMenuText = L"Compress and reclaim space (NibbleZip)";
#define WM_NZ_PROGRESS (WM_APP + 1)
#define WM_NZ_DONE (WM_APP + 2)
#define NZ_COPYDATA_ADD 0x4E5A0001
#define NZ_COPYDATA_EXTRACT 0x4E5A0002
#define NZ_COPYDATA_DROP 0x4E5A0003      // files dropped on NibbleZip.exe itself: decide the mode from what they are

enum { ID_LIST = 100, ID_ADDFILES, ID_ADDFOLDER, ID_REMOVE, ID_CLEAR, ID_ARCHIVE, ID_BROWSE, ID_FORMAT, ID_LEVEL, ID_EAT,
       ID_START, ID_PAUSE, ID_STOP, ID_MENU, ID_VERIFY, ID_THEME, ID_EATLABEL, ID_OUTPUT, ID_LOG, ID_SPLIT, ID_EXTRACT, ID_JOBS, ID_MORE, ID_ABOUT, ID_THEME_AUTO, ID_THEME_LIGHT, ID_THEME_DARK, ID_MODE_C, ID_MODE_X };

static std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr, list = nullptr, archive = nullptr, format = nullptr, level = nullptr, eat = nullptr, info = nullptr,
         bar = nullptr, st1 = nullptr, st2 = nullptr, start = nullptr, pause = nullptr, stop = nullptr, menuBtn = nullptr,
         addFiles = nullptr, addFolder = nullptr, removeBtn = nullptr, clearBtn = nullptr, browse = nullptr,
         eatLabel = nullptr, verifyBtn = nullptr, themeBtn = nullptr, output = nullptr, saveLabel = nullptr, logBtn = nullptr, split = nullptr, extractBtn = nullptr, jobsBtn = nullptr, moreBtn = nullptr,
         modeC = nullptr, modeX = nullptr, subtitle = nullptr, lblCreate = nullptr, lblFormat = nullptr, lblLevel = nullptr, lblSplit = nullptr, lblSplitHint = nullptr;
    int mode = 0;                 // 0 = compress, 1 = extract. Always starts in compress.
    bool modeChosen = false;      // the person picked a mode for the current list, so never switch it for them
    bool autoNote = false;        // the mode was switched to Extract automatically
    bool eatC = true, eatX = true;
    std::string saveC, saveX;
    HFONT font = nullptr, fontBig = nullptr;
    int dpi = 96;
    std::vector<std::string> items;
    bool autoName = true, settingText = false, running = false, closing = false, verifying = false;
    nz::VerifyResult vres;
    bool batch = false, undoing = false, extracting = false;
    struct XSummary {
        bool ok = false, paused = false, resumable = false, removed = false, eat = false;
        std::string error, dest; std::vector<std::string> warnings;
        uint64_t done = 0, total = 0, files = 0, bytes = 0, skipped = 0;
    } xsum;
    nz::BatchResult bres;
    std::thread worker;
    std::mutex mu;
    nz::Progress prog;
    nz::Result result;
    std::atomic<int> stopReq{0};
    std::atomic<bool> pauseFlag{false};
    ULONGLONG startTick = 0;
    uint64_t startDone = 0;
    bool haveStart = false;
    std::string jobArchive;
};
static App g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }
static std::wstring windowTitle() { return std::wstring(L"NibbleZip ") + kVersion; }
static void showJobsWindow();
static void refreshJobsButton();
static std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}
static int themedBox(HWND owner, const std::wstring& text, const wchar_t* title, UINT flags);
static int msg(const std::wstring& text, UINT flags) { return themedBox(g.wnd, text, kTitle, flags); }
static void ensureFonts() {
    if (g.font) return;
    g.font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontBig = CreateFontW(-MulDiv(15, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
static const wchar_t* extFor(int fmt) { return fmt == nz::FZIP ? L".zip" : L".7z"; }
static int curFormat() { return (int)SendMessageW(g.format, CB_GETCURSEL, 0, 0) == 1 ? nz::FZIP : nz::F7Z; }
static int curMode() { int v = (int)SendMessageW(g.output, CB_GETCURSEL, 0, 0); return v < 0 ? 0 : v; }
// Part size chosen in the "Split into parts" box, in bytes. 0 = no splitting, (uint64_t)-1 = not understood.
static uint64_t curSplit() {
    std::wstring t = getText(g.split);
    size_t i = 0;
    while (i < t.size() && t[i] == L' ') i++;
    if (i >= t.size() || t[i] < L'0' || t[i] > L'9') return t.empty() || t[i] == L'N' || t[i] == L'n' ? 0 : (uint64_t)-1;
    double v = 0; bool dot = false; double scale = 0.1;
    for (; i < t.size(); i++) {
        wchar_t c = t[i];
        if (c >= L'0' && c <= L'9') { if (dot) { v += (c - L'0') * scale; scale /= 10; } else v = v * 10 + (c - L'0'); }
        else if ((c == L'.' ) && !dot) dot = true;
        else if (c == L',') continue;
        else break;
    }
    while (i < t.size() && t[i] == L' ') i++;
    double unit = 1024.0 * 1024.0;
    if (i < t.size() && (t[i] == L'G' || t[i] == L'g')) unit *= 1024.0;
    else if (i < t.size() && (t[i] == L'K' || t[i] == L'k')) unit = 1024.0;
    double bytes = v * unit;
    if (bytes < 1024.0 * 1024.0 || bytes > 1e15) return (uint64_t)-1;
    return (uint64_t)bytes;
}
static int curLevel() { int v = (int)SendMessageW(g.level, CB_GETCURSEL, 0, 0); return v < 0 ? 1 : v; }

struct GuiCb : nz::Callbacks {
    void progress(const nz::Progress& p) override {
        { std::lock_guard<std::mutex> l(g.mu); g.prog = p; }
        PostMessageW(g.wnd, WM_NZ_PROGRESS, 0, 0);
    }
    int poll() override {
        while (g.pauseFlag.load() && g.stopReq.load() == 0) Sleep(100);
        return g.stopReq.load();
    }
};

// ---------------------------------------------------------------- settings and theme
static DWORD regGet(const wchar_t* name, DWORD def) {
    HKEY k; DWORD v = def, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NibbleZip", 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD) v = def;
    RegCloseKey(k);
    return v;
}
static void regSet(const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NibbleZip", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}
struct ThemeT {
    int pref = 0;            // 0 follow Windows, 1 light, 2 dark
    bool dark = false;
    COLORREF bg = 0, text = 0, ctl = 0, line = 0, hdr = 0;
    HBRUSH brBg = nullptr, brCtl = nullptr, brHdr = nullptr;
    FARPROC allowWnd = nullptr;
};
static ThemeT T;
static bool systemDark() {
    HKEY k; DWORD v = 1, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&v, &sz) != ERROR_SUCCESS) v = 1;
    RegCloseKey(k);
    return v == 0;
}
static DWORD winBuild() {
    typedef void(WINAPI * Fn)(DWORD*, DWORD*, DWORD*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    Fn f = nt ? (Fn)(void*)GetProcAddress(nt, "RtlGetNtVersionNumbers") : nullptr;
    DWORD a = 0, b = 0, c = 0;
    if (f) f(&a, &b, &c);
    return a >= 10 ? (c & 0x0FFFFFFF) : 0;
}
static void initTheme() {
    T.pref = (int)regGet(L"Theme", 0);
    T.dark = T.pref == 2 || (T.pref == 0 && systemDark());
    if (T.dark) { T.bg = RGB(32, 32, 32); T.text = RGB(240, 240, 240); T.ctl = RGB(45, 45, 45); T.line = RGB(80, 80, 80); T.hdr = RGB(56, 56, 56); }
    else { T.bg = GetSysColor(COLOR_WINDOW); T.text = GetSysColor(COLOR_WINDOWTEXT); T.ctl = T.bg; T.line = RGB(200, 200, 200); T.hdr = T.bg; }
    T.brBg = CreateSolidBrush(T.bg); T.brCtl = CreateSolidBrush(T.ctl); T.brHdr = CreateSolidBrush(T.hdr);
    DWORD build = winBuild();
    if (T.dark && build >= 17763) {
        // Windows' own dark styles for buttons, scroll bars and drop-downs (Windows 10 1809 and later).
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux) {
            FARPROC p135 = GetProcAddress(ux, MAKEINTRESOURCEA(135));
            if (p135) { if (build >= 18362) ((int(WINAPI*)(int))(void*)p135)(2); else ((BOOL(WINAPI*)(BOOL))(void*)p135)(TRUE); }
            FARPROC p104 = GetProcAddress(ux, MAKEINTRESOURCEA(104));
            if (p104) ((void(WINAPI*)())(void*)p104)();
            T.allowWnd = GetProcAddress(ux, MAKEINTRESOURCEA(133));
        }
    }
}
static void darken(HWND h, const wchar_t* theme) {
    if (!T.dark || !h) return;
    if (T.allowWnd) ((BOOL(WINAPI*)(HWND, BOOL))(void*)T.allowWnd)(h, TRUE);
    SetWindowTheme(h, theme, nullptr);
}
static void darkTitleBar(HWND h) {
    if (!T.dark) return;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * Fn)(HWND, DWORD, LPCVOID, DWORD);
    Fn f = (Fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    BOOL on = TRUE;
    if (f && FAILED(f(h, 20, &on, sizeof(on)))) f(h, 19, &on, sizeof(on));
}

// Message box that follows the dark theme. In light mode the normal Windows message box is used.
static const wchar_t* kBoxClass = L"NibbleZipBox";
struct BoxState { int result = 0; bool done = false; int defId = 0, cancelId = 0; };
static LRESULT CALLBACK boxProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    BoxState* st = (BoxState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND: {
        if (!st || HIWORD(wp) != BN_CLICKED) break;
        int id = LOWORD(wp);
        if (id == IDOK && !GetDlgItem(h, IDOK)) id = st->defId;                    // Enter key
        if (id == IDCANCEL && !GetDlgItem(h, IDCANCEL)) { id = st->cancelId; if (!id) return 0; }   // Esc key
        st->result = id; st->done = true;
        return 0;
    }
    case WM_CLOSE:
        if (st && st->cancelId) { st->result = st->cancelId; st->done = true; }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, T.text); SetBkColor((HDC)wp, T.ctl);
        return (LRESULT)T.brCtl;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static int themedBox(HWND owner, const std::wstring& text, const wchar_t* title, UINT flags) {
    if (!T.dark) return MessageBoxW(owner, text.c_str(), title, flags);
    ensureFonts();
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = boxProc; wc.hInstance = g.inst; wc.lpszClassName = kBoxClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = T.brCtl;
        RegisterClassExW(&wc); reg = true;
    }
    UINT type = flags & MB_TYPEMASK;
    struct Btn { int id; const wchar_t* label; };
    std::vector<Btn> btns;
    BoxState st;
    if (type == MB_OKCANCEL) { btns = {{IDOK, L"OK"}, {IDCANCEL, L"Cancel"}}; st.cancelId = IDCANCEL; }
    else if (type == MB_YESNO) { btns = {{IDYES, L"Yes"}, {IDNO, L"No"}}; st.cancelId = 0; }
    else if (type == MB_YESNOCANCEL) { btns = {{IDYES, L"Yes"}, {IDNO, L"No"}, {IDCANCEL, L"Cancel"}}; st.cancelId = IDCANCEL; }
    else { btns = {{IDOK, L"OK"}}; st.cancelId = IDOK; }
    size_t def = (flags & MB_DEFMASK) == MB_DEFBUTTON2 ? 1 : (flags & MB_DEFMASK) == MB_DEFBUTTON3 ? 2 : 0;
    if (def >= btns.size()) def = 0;
    st.defId = btns[def].id;
    LPCWSTR iconId = nullptr;
    switch (flags & MB_ICONMASK) {
        case MB_ICONWARNING: iconId = IDI_WARNING; break;
        case MB_ICONERROR: iconId = IDI_ERROR; break;
        case MB_ICONQUESTION: iconId = IDI_QUESTION; break;
        case MB_ICONINFORMATION: iconId = IDI_INFORMATION; break;
    }
    // Measure the text.
    const int pad = S(20), iconW = iconId ? S(32) + S(14) : 0, maxW = S(440), minW = S(240);
    HDC dc = GetDC(nullptr); HFONT old = (HFONT)SelectObject(dc, g.font);
    RECT one = {0, 0, 0, 0}; DrawTextW(dc, text.c_str(), -1, &one, DT_CALCRECT | DT_NOPREFIX);
    int tw = std::min(maxW, std::max(minW, (int)one.right + S(4)));
    RECT tr = {0, 0, tw, 0}; DrawTextW(dc, text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old); ReleaseDC(nullptr, dc);
    int th = std::max((int)tr.bottom, iconId ? S(32) : 0);
    const int bw = S(88), bh = S(28), gap = S(8);
    int cw = pad + iconW + tw + pad;
    int needBtn = (int)btns.size() * bw + ((int)btns.size() - 1) * gap + 2 * pad;
    if (cw < needBtn) cw = needBtn;
    int ch = pad + th + S(22) + bh + S(16);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT wr = {0, 0, cw, ch}; AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT orc;
    if (owner && IsWindowVisible(owner)) GetWindowRect(owner, &orc);
    else { orc.left = 0; orc.top = 0; orc.right = GetSystemMetrics(SM_CXSCREEN); orc.bottom = GetSystemMetrics(SM_CYSCREEN); }
    int x = orc.left + ((orc.right - orc.left) - ww) / 2, y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, kBoxClass, title, style, x, y, ww, wh, owner, nullptr, g.inst, nullptr);
    if (!h) return MessageBoxW(owner, text.c_str(), title, flags);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);
    darkTitleBar(h);
    if (iconId) {
        HWND ic = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ICON, pad, pad, S(32), S(32), h, nullptr, g.inst, nullptr);
        SendMessageW(ic, STM_SETICON, (WPARAM)LoadIconW(nullptr, iconId), 0);
    }
    HWND tx = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX, pad + iconW, pad, tw, th, h, nullptr, g.inst, nullptr);
    SendMessageW(tx, WM_SETFONT, (WPARAM)g.font, TRUE);
    int bx = cw - pad - (int)btns.size() * bw - ((int)btns.size() - 1) * gap, by = pad + th + S(22);
    HWND defBtn = nullptr;
    for (size_t i = 0; i < btns.size(); i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", btns[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : 0),
                                 bx + (int)i * (bw + gap), by, bw, bh, h, (HMENU)(INT_PTR)btns[i].id, g.inst, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
        darken(b, L"DarkMode_Explorer");
        if (i == def) defBtn = b;
    }
    bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    if (defBtn) SetFocus(defBtn);
    MSG m;
    while (!st.done) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)m.wParam); st.result = st.cancelId ? st.cancelId : st.defId; break; }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (owner && ownerWasEnabled) EnableWindow(owner, TRUE);
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    DestroyWindow(h);
    if (owner && ownerWasEnabled) SetForegroundWindow(owner);
    return st.result;
}

static bool acceptDisclaimer() {
    if (regGet(L"DisclaimerAccepted", 0) >= kDisclaimerVersion) return true;
    int r = themedBox(nullptr,
        L"NibbleZip permanently deletes your original files as it compresses them, and can delete archives as it extracts them. "
        L"That is its purpose, and it cannot be undone.\n\n"
        L"NibbleZip was written by an AI. Its code, tests and documentation were written by Claude, an AI model made by Anthropic, "
        L"for its author, who designed it and tests it. It is heavily tested, but no human programmer has reviewed the code line by line.\n\n"
        L"This software is provided \"as is\", without warranty of any kind. The authors and contributors are not responsible or liable "
        L"for any loss of data or any other damage arising from its use. You use it entirely at your own risk.\n\n"
        L"Keep a backup of anything you cannot afford to lose.\n\n"
        L"Press OK to accept these terms and continue, or Cancel to exit.",
        L"NibbleZip \u2013 please read", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (r != IDOK) return false;
    regSet(L"DisclaimerAccepted", kDisclaimerVersion);
    return true;
}
static void showAbout() {
    std::wstring t = std::wstring(L"NibbleZip ") + kVersion + L"\nCompress in place. Verify, then delete.\n\n"
        L"Idea, design and testing: cloudhaacker.\n"
        L"Code, tests and documentation: written by Claude, an AI model made by Anthropic.\n\n"
        L"Free software under the MIT licence, with no warranty (see LICENSE.txt).\n"
        L"Compression: LZMA SDK by Igor Pavlov (public domain) and zlib by Jean-loup Gailly and Mark Adler.\n\n"
        L"Source code, bug reports and how it works:\ngithub.com/cloudhaacker/NibbleZip\n\nOpen the project page now?";
    if (themedBox(g.wnd, t, L"About NibbleZip", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES)
        ShellExecuteW(g.wnd, L"open", L"https://github.com/cloudhaacker/NibbleZip", nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- right-click menu
static std::wstring exePath() {
    std::wstring b(32768, 0);
    DWORD n = GetModuleFileNameW(nullptr, &b[0], 32767);
    b.resize(n);
    return b;
}
static const wchar_t* kMenuKeys[2] = {L"Software\\Classes\\*\\shell\\NibbleZip", L"Software\\Classes\\Directory\\shell\\NibbleZip"};
static bool menuInstalled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kMenuKeys[0], 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}
static void setStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static const wchar_t* kExtractKeys[3] = {L"Software\\Classes\\SystemFileAssociations\\.7z\\shell\\NibbleZipExtract",
                                         L"Software\\Classes\\SystemFileAssociations\\.zip\\shell\\NibbleZipExtract",
                                         L"Software\\Classes\\SystemFileAssociations\\.001\\shell\\NibbleZipExtract"};
static bool setMenu(bool on) {
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, kExtractKeys[i]); continue; }
        HKEY k, c;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kExtractKeys[i], 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) { ok = false; continue; }
        std::wstring exe = exePath();
        setStr(k, nullptr, L"Extract and reclaim space (NibbleZip)");
        setStr(k, L"Icon", L"\"" + exe + L"\"");
        setStr(k, L"MultiSelectModel", L"Player");
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            setStr(c, nullptr, L"\"" + exe + L"\" --extract \"%1\"");
            RegCloseKey(c);
        } else ok = false;
        RegCloseKey(k);
    }
    for (int i = 0; i < 2; i++) {
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, kMenuKeys[i]); continue; }
        HKEY k, c;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kMenuKeys[i], 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) { ok = false; continue; }
        std::wstring exe = exePath();
        setStr(k, nullptr, kMenuText);
        setStr(k, L"Icon", L"\"" + exe + L"\"");
        setStr(k, L"MultiSelectModel", L"Player");
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            setStr(c, nullptr, L"\"" + exe + L"\" --compress \"%1\"");
            RegCloseKey(c);
        } else ok = false;
        RegCloseKey(k);
    }
    return ok;
}
static void refreshMenuButton() {
    SetWindowTextW(g.menuBtn, menuInstalled() ? L"Remove from right-click menu" : L"Add to right-click menu");
}

// ---------------------------------------------------------------- item list
static std::string uniqueArchive(const std::string& base, const std::string& ext) {
    std::string cand = base + ext;
    for (int i = 2; i < 1000 && nz::archiveExists(cand); i++) cand = base + " (" + std::to_string(i) + ")" + ext;
    return cand;
}
static void setArchiveText(const std::string& p) {
    g.settingText = true;
    SetWindowTextW(g.archive, W(p).c_str());
    g.settingText = false;
}
// 1 = an archive NibbleZip can open (.7z, .zip, or the .001 part of a split one), 2 = a later part of a split archive, 0 = anything else.
static int archiveKind(const std::string& path) {
    plat::Stat st;
    if (!plat::stat(path, st) || st.isDir) return 0;
    std::string n = plat::baseName(path);
    for (auto& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    auto ends = [&](const char* e) { size_t k = strlen(e); return n.size() > k && n.compare(n.size() - k, k, e) == 0; };
    if (ends(".7z") || ends(".zip")) return 1;
    size_t dot = n.find_last_of('.');
    if (dot == std::string::npos || n.size() - dot != 4) return 0;
    for (size_t i = dot + 1; i < n.size(); i++) if (n[i] < '0' || n[i] > '9') return 0;
    std::string stem = n.substr(0, dot);
    bool arcStem = (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".7z") == 0) || (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".zip") == 0);
    if (!arcStem) return 0;
    return n.compare(dot, 4, ".001") == 0 ? 1 : 2;
}
static std::string firstPartOf(const std::string& path) { return path.substr(0, path.size() - 4) + ".001"; }
// Total size and number of parts of a split archive, given its .001 part.
static uint64_t splitInfo(const std::string& first, int& parts) {
    uint64_t t = 0; std::string base = first.substr(0, first.size() - 4); plat::Stat st; parts = 0;
    for (int i = 1; i < 100000; i++) {
        char suf[16]; snprintf(suf, sizeof(suf), ".%03d", i);
        if (!plat::stat(base + suf, st)) break;
        t += st.size; parts++;
    }
    return t;
}
static bool isFirstPart(const std::string& p) { return p.size() > 4 && p.compare(p.size() - 4, 4, ".001") == 0; }
// What the Size column shows: in Extract mode a split archive is shown as a whole.
static std::wstring sizeText(const std::string& p) {
    plat::Stat st;
    if (!plat::stat(p, st)) return L"";
    if (st.isDir) return L"Folder";
    if (g.mode == 1 && isFirstPart(p) && archiveKind(p) == 1) {
        int parts = 0; uint64_t t = splitInfo(p, parts);
        return W(nz::humanSize(t)) + L" (" + std::to_wstring(parts) + L" parts)";
    }
    return W(nz::humanSize(st.size));
}
static void refreshSizes() {
    for (size_t i = 0; i < g.items.size(); i++) {
        std::wstring t = sizeText(g.items[i]);
        LVITEMW it; memset(&it, 0, sizeof(it)); it.iSubItem = 1; it.pszText = (LPWSTR)t.c_str();
        SendMessageW(g.list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
}
static void collectArchives(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
    std::vector<plat::DirEnt> ents;
    if (depth > 32 || !plat::listDir(dir, ents)) return;
    for (auto& e : ents) {
        if (e.st.isLink) continue;
        std::string p = plat::join(dir, e.name);
        if (e.st.isDir) collectArchives(p, out, depth + 1);
        else if (archiveKind(p) == 1) out.push_back(p);
    }
}
static void place(HWND h, int x, int y, int w, int hh) { SetWindowPos(h, nullptr, S(x), S(y), S(w), S(hh), SWP_NOZORDER | SWP_NOACTIVATE); }
static void updateInfo();
static void updateAutoName();
static void setMode(int m, bool chosen) {
    if (chosen) g.modeChosen = true;
    if (m != g.mode) {
        bool eat = SendMessageW(g.eat, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::string save = U(getText(g.archive));
        if (g.mode == 0) { g.eatC = eat; g.saveC = save; } else { g.eatX = eat; g.saveX = save; }
        g.mode = m;
        SendMessageW(g.eat, BM_SETCHECK, (m == 0 ? g.eatC : g.eatX) ? BST_CHECKED : BST_UNCHECKED, 0);
        setArchiveText(m == 0 ? g.saveC : g.saveX);
    }
    if (chosen || m == 0) g.autoNote = false;
    SendMessageW(g.modeC, BM_SETCHECK, m == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.modeX, BM_SETCHECK, m == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g.subtitle, m == 0 ? L"Compresses files and frees up their space as it goes."
                                      : L"Extracts archives and frees up their space as it goes.");
    HWND compressOnly[] = {g.lblCreate, g.output, g.lblFormat, g.format, g.lblLevel, g.level, g.lblSplit, g.split, g.lblSplitHint};
    for (HWND h : compressOnly) ShowWindow(h, m == 0 ? SW_SHOW : SW_HIDE);
    const int M = 20, CW = 600;
    if (m == 0) {
        SetWindowTextW(g.saveLabel, curMode() == nz::OutSingle ? L"Save archive as:" : L"Save the archives in this folder (leave empty to put each one next to its original):");
        SetWindowTextW(g.eatLabel, L"Delete each original once it is safely stored and verified (frees space while compressing)");
        place(g.saveLabel, M, 332, CW, 18); place(g.archive, M, 352, CW - 98, 24); place(g.browse, M + CW - 90, 350, 90, 28);
        place(g.eat, M, 465, 18, 18); place(g.eatLabel, M + 22, 466, CW - 22, 20);
        place(g.info, M, 500, CW, 70); place(g.st1, M, 496, CW, 18); place(g.bar, M, 518, CW, 18); place(g.st2, M, 542, CW, 34);
        SetWindowTextW(g.addFiles, L"Add files\u2026");
    } else {
        SetWindowTextW(g.saveLabel, L"Extract into this folder (leave empty to put each archive's files in a new folder next to it):");
        SetWindowTextW(g.eatLabel, L"Delete each archive as its files are extracted and verified (frees space while extracting)");
        place(g.saveLabel, M, 300, CW, 18); place(g.archive, M, 320, CW - 98, 24); place(g.browse, M + CW - 90, 318, 90, 28);
        place(g.eat, M, 364, 18, 18); place(g.eatLabel, M + 22, 365, CW - 22, 20);
        place(g.info, M, 400, CW, 120); place(g.st1, M, 400, CW, 18); place(g.bar, M, 422, CW, 18); place(g.st2, M, 446, CW, 34);
        SetWindowTextW(g.addFiles, L"Add archives\u2026");
    }
    InvalidateRect(g.wnd, nullptr, TRUE);
    refreshSizes();
    updateAutoName(); updateInfo();
}
static void updateAutoName() {
    if (g.mode != 0 || !g.autoName || curMode() != nz::OutSingle) return;
    if (g.items.empty()) { setArchiveText(""); return; }
    std::string ext = U(extFor(curFormat())), first = g.items[0], dir = plat::parentDir(first), base;
    if (g.items.size() == 1) {
        std::string name = plat::baseName(first); plat::Stat st; plat::stat(first, st);
        if (!st.isDir) { size_t dot = name.find_last_of('.'); if (dot != std::string::npos && dot > 0) name = name.substr(0, dot); }
        base = plat::join(dir, name);
    } else {
        std::string name = plat::baseName(dir);
        if (name.empty() || name.find(':') != std::string::npos) name = "Archive";
        base = plat::join(dir, name);
    }
    setArchiveText(uniqueArchive(base, ext));
}
static void updateInfo() {
    if (g.running) return;
    std::wstring t;
    if (g.items.empty()) t = g.mode == 0 ? L"Drop files or folders here, or use the buttons above." : L"Drop archives (.7z, .zip, or the .001 part of a split one), or folders that contain them, here.";
    else if (g.mode == 1) {
        size_t arcs = 0, other = 0;
        for (auto& it : g.items) { if (archiveKind(it) == 1) arcs++; else other++; }
        t = std::to_wstring(arcs) + (arcs == 1 ? L" archive selected." : L" archives selected.");
        if (other) t += L"  " + std::to_wstring(other) + L" other item(s) are not 7z or ZIP archives and will be left alone.";
        uint64_t fr = 0; std::string dst = U(getText(g.archive));
        std::string where = dst.empty() ? plat::parentDir(g.items[0]) : plat::absPath(dst);
        if (plat::freeSpace(where, fr)) t += L"  Free space: " + W(nz::humanSize(fr)) + L".";
        if (g.autoNote) t += L"\n\nNibbleZip switched to Extract because everything you added is an archive. To pack them into a new archive instead, choose Compress.";
    } else {
        std::string arc = U(getText(g.archive)); uint64_t fr = 0;
        t = std::to_wstring(g.items.size()) + (g.items.size() == 1 ? L" item selected." : L" items selected.");
        std::string where = curMode() == nz::OutSingle ? (arc.empty() ? "" : plat::parentDir(plat::absPath(arc))) : (arc.empty() ? plat::parentDir(g.items[0]) : plat::absPath(arc));
        if (!where.empty() && plat::freeSpace(where, fr)) t += L"  Free space on the archive drive: " + W(nz::humanSize(fr)) + L".";
    }
    SetWindowTextW(g.info, t.c_str());
}
static void addItem(const std::string& raw) {
    std::string p = plat::absPath(raw); plat::Stat st;
    if (!plat::stat(p, st)) return;
    for (auto& x : g.items) if (plat::samePath(x, p)) return;
    g.items.push_back(p);
    LVITEMW it; memset(&it, 0, sizeof(it));
    std::wstring name = W(plat::baseName(p)), size = sizeText(p), loc = W(plat::parentDir(p));
    it.mask = LVIF_TEXT; it.iItem = (int)g.items.size() - 1; it.pszText = (LPWSTR)name.c_str();
    int idx = (int)SendMessageW(g.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iItem = idx; it.iSubItem = 1; it.pszText = (LPWSTR)size.c_str(); SendMessageW(g.list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 2; it.pszText = (LPWSTR)loc.c_str(); SendMessageW(g.list, LVM_SETITEMW, 0, (LPARAM)&it);
    updateAutoName(); updateInfo();
}
// source: 0 = dropped or picked in the window, 1 = the "Compress" right-click entry, 2 = the "Extract" right-click entry
static void addItems(const std::vector<std::string>& raw, int source) {
    bool wasEmpty = g.items.empty();
    if (wasEmpty) { g.modeChosen = false; g.autoNote = false; }
    if (source == 1 && (wasEmpty || g.mode == 0)) setMode(0, true);
    if (source == 2 && (wasEmpty || g.mode == 1)) setMode(1, true);
    std::vector<std::string> paths;
    for (auto& r : raw) { std::string p = plat::absPath(r); plat::Stat st; if (plat::stat(p, st)) paths.push_back(p); }
    if (paths.empty()) return;
    if (wasEmpty && !g.modeChosen && g.mode == 0) {
        bool all = true;
        for (auto& p : paths) if (archiveKind(p) == 0) all = false;
        if (all) { setMode(1, false); g.autoNote = true; }
    }
    for (auto& p : paths) {
        if (g.mode == 1) {
            plat::Stat st; plat::stat(p, st);
            if (st.isDir) { std::vector<std::string> found; collectArchives(p, found); for (auto& f : found) addItem(f); continue; }
            if (archiveKind(p) == 2) { if (archiveKind(firstPartOf(p)) == 1) addItem(firstPartOf(p)); continue; }
        }
        addItem(p);
    }
    updateInfo();
}
static void removeSelected() {
    for (int i = (int)g.items.size() - 1; i >= 0; i--) {
        if (SendMessageW(g.list, LVM_GETITEMSTATE, i, LVIS_SELECTED) & LVIS_SELECTED) {
            SendMessageW(g.list, LVM_DELETEITEM, i, 0);
            g.items.erase(g.items.begin() + i);
        }
    }
    updateAutoName(); updateInfo();
}
static void clearItems() {
    SendMessageW(g.list, LVM_DELETEALLITEMS, 0, 0);
    g.items.clear();
    g.modeChosen = false; g.autoNote = false;
    g.autoName = true;
    updateAutoName(); updateInfo();
}
static void pickItems(bool folders) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return;
    DWORD o = 0; d->GetOptions(&o);
    d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0));
    if (g.mode == 1 && !folders) { COMDLG_FILTERSPEC spec[2] = {{L"Archives (*.7z, *.zip, *.001)", L"*.7z;*.zip;*.001"}, {L"All files", L"*.*"}}; d->SetFileTypes(2, spec); }
    d->SetTitle(g.mode == 1 ? (folders ? L"Choose folders that contain archives" : L"Choose archives to extract") : (folders ? L"Choose folders to compress" : L"Choose files to compress"));
    std::vector<std::string> picked;
    if (SUCCEEDED(d->Show(g.wnd))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr)) && arr) {
            DWORD n = 0; arr->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it)) && it) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { picked.push_back(U(p)); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    d->Release();
    if (!picked.empty()) addItems(picked, 0);
}
static void browseArchive() {
    if (g.mode == 1 || curMode() != nz::OutSingle) {   // a folder: for several archives, or for extracted files
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return;
        DWORD o = 0; d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(g.mode == 1 ? L"Choose the folder to extract into" : L"Choose the folder for the archives");
        if (SUCCEEDED(d->Show(g.wnd))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it)) && it) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { setArchiveText(U(p)); CoTaskMemFree(p); updateInfo(); }
                it->Release();
            }
        }
        d->Release();
        return;
    }
    wchar_t buf[32768]; buf[0] = 0;
    std::wstring cur = getText(g.archive);
    if (cur.size() < 32000) wcscpy(buf, cur.c_str());
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = curFormat() == nz::FZIP ? L"ZIP archive (*.zip)\0*.zip\0" : L"7z archive (*.7z)\0*.7z\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 32767; ofn.lpstrDefExt = curFormat() == nz::FZIP ? L"zip" : L"7z";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Save archive as";
    if (GetSaveFileNameW(&ofn)) { g.autoName = false; setArchiveText(U(buf)); updateInfo(); }
}
static void outputChanged() {
    if (curMode() == nz::OutSingle) {
        SetWindowTextW(g.saveLabel, L"Save archive as:");
        g.autoName = true; updateAutoName();
    } else {
        SetWindowTextW(g.saveLabel, L"Save the archives in this folder (leave empty to put each one next to its original):");
        g.autoName = false; setArchiveText("");
    }
    updateInfo();
}
static void formatChanged() {
    if (g.mode != 0 || curMode() != nz::OutSingle) return;
    if (g.autoName) { updateAutoName(); return; }
    std::wstring t = getText(g.archive), lower = t;
    CharLowerBuffW(&lower[0], (DWORD)lower.size());
    const wchar_t* exts[2] = {L".7z", L".zip"};
    for (auto e : exts) {
        size_t n = wcslen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) { setArchiveText(U(t.substr(0, t.size() - n) + extFor(curFormat()))); return; }
    }
}

// ---------------------------------------------------------------- running a job
static void setRunningUI(bool on) {
    g.running = on;
    HWND inputs[] = {g.output, g.jobsBtn, g.moreBtn, g.modeC, g.modeX, g.split, g.archive, g.format, g.level, g.eat, g.eatLabel, g.addFiles, g.addFolder, g.removeBtn, g.clearBtn, g.browse, g.menuBtn, g.verifyBtn};
    for (HWND h : inputs) EnableWindow(h, !on);
    ShowWindow(g.start, on ? SW_HIDE : SW_SHOW);
    ShowWindow(g.pause, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.stop, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.bar, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.st1, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.st2, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.info, on ? SW_HIDE : SW_SHOW);
    EnableWindow(g.pause, TRUE); EnableWindow(g.stop, TRUE);
    SetWindowTextW(g.pause, L"Pause");
    if (on) { SendMessageW(g.bar, PBM_SETPOS, 0, 0); SetWindowTextW(g.st1, L"Preparing…"); SetWindowTextW(g.st2, L""); }
    else { g.verifying = false; g.batch = false; g.undoing = false; g.extracting = false; SetWindowTextW(g.wnd, windowTitle().c_str()); updateInfo(); refreshJobsButton(); }
}
static void startVerify(const std::string& path) {
    if (g.worker.joinable()) g.worker.join();
    g.stopReq = 0; g.pauseFlag = false; g.haveStart = false;
    { std::lock_guard<std::mutex> l(g.mu); g.prog = nz::Progress(); g.vres = nz::VerifyResult(); }
    setRunningUI(true);
    g.verifying = true;
    SetWindowTextW(g.st1, L"Checking…");
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    g.worker = std::thread([path]() {
        GuiCb cb;
        nz::VerifyResult v = nz::verifyArchive(path, cb);
        { std::lock_guard<std::mutex> l(g.mu); g.vres = v; }
        PostMessageW(g.wnd, WM_NZ_DONE, 0, 0);
    });
}
static void pickVerify() {
    wchar_t buf[32768]; buf[0] = 0;
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"Archives (*.7z;*.zip;*.001)\0*.7z;*.zip;*.001\0All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 32767;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Choose an archive to check";
    if (GetOpenFileNameW(&ofn)) startVerify(U(buf));
}
static void relaunch() {
    std::wstring cmd = L"\"" + exePath() + L"\" --new";
    for (auto& it : g.items) cmd += L" \"" + W(it) + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DestroyWindow(g.wnd);
    }
}
static void startUndo(const std::string& archive) {
    if (g.worker.joinable()) g.worker.join();
    g.stopReq = 0; g.pauseFlag = false; g.haveStart = false;
    { std::lock_guard<std::mutex> l(g.mu); g.prog = nz::Progress(); g.result = nz::Result(); }
    setRunningUI(true);
    g.undoing = true;
    SetWindowTextW(g.st1, L"Checking the stored part…");
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    g.worker = std::thread([archive]() {
        GuiCb cb;
        nz::Result r = nz::undoPartial(archive, cb);
        { std::lock_guard<std::mutex> l(g.mu); g.result = r; }
        PostMessageW(g.wnd, WM_NZ_DONE, 0, 0);
    });
}
struct XCb : GuiCb {
    uint64_t idx = 0, count = 0;
    void progress(const nz::Progress& p) override { nz::Progress q = p; q.jobIndex = idx; q.jobCount = count; GuiCb::progress(q); }
};
struct XJob { std::string archive, dest; };
static uint64_t splitTotal(const std::string& first) { int n = 0; return splitInfo(first, n); }
static void startExtract(const std::vector<XJob>& jobs, bool eat) {
    if (g.worker.joinable()) g.worker.join();
    g.stopReq = 0; g.pauseFlag = false; g.haveStart = false;
    { std::lock_guard<std::mutex> l(g.mu); g.prog = nz::Progress(); g.xsum = App::XSummary(); }
    setRunningUI(true);
    g.extracting = true;
    SetWindowTextW(g.st1, L"Opening the archive\u2026");
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    g.worker = std::thread([jobs, eat]() {
        App::XSummary s; s.total = jobs.size(); s.eat = eat; s.ok = true;
        for (size_t i = 0; i < jobs.size(); i++) {
            if (g.stopReq.load() == 1) { s.ok = false; s.paused = true; s.resumable = true; break; }
            XCb cb; cb.idx = i + 1; cb.count = jobs.size();
            nz::ExtractOptions o; o.archive = jobs[i].archive; o.dest = jobs[i].dest; o.eat = eat;
            nz::ExtractResult r = nz::extractArchive(o, cb);
            for (auto& w : r.warnings) if (s.warnings.size() < 50) s.warnings.push_back(w);
            s.files += r.files; s.bytes += r.bytes; s.dest = r.dest; if (r.archiveRemoved) s.removed = true;
            if (r.ok) { s.done++; continue; }
            std::string label = jobs.size() > 1 ? plat::baseName(jobs[i].archive) + ": " : "";
            if (!r.paused && !r.resumable && jobs.size() > 1) {   // refused before anything was changed: skip it, carry on with the rest
                s.skipped++;
                if (s.warnings.size() < 50) s.warnings.push_back("Skipped " + label + r.error);
                continue;
            }
            s.ok = false; s.paused = r.paused; s.resumable = r.resumable;
            s.error = label + r.error;
            break;
        }
        { std::lock_guard<std::mutex> l(g.mu); g.xsum = s; }
        PostMessageW(g.wnd, WM_NZ_DONE, 0, 0);
    });
}
static void onStartExtract() {
    std::vector<std::string> arcs;
    for (auto& it : g.items) if (archiveKind(it) == 1) arcs.push_back(it);
    if (arcs.empty()) { msg(L"There are no 7z or ZIP archives in the list.", MB_OK | MB_ICONINFORMATION); return; }
    std::string outDir = U(getText(g.archive));
    while (!outDir.empty() && (outDir.back() == ' ' || outDir.back() == '\t')) outDir.pop_back();
    if (!outDir.empty()) {
        outDir = plat::absPath(outDir); plat::Stat st;
        if (!plat::stat(outDir, st) || !st.isDir) { msg(L"The folder to extract into does not exist:\n\n" + W(outDir), MB_OK | MB_ICONINFORMATION); return; }
    }
    bool eat = SendMessageW(g.eat, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::vector<XJob> jobs; std::set<std::string> used; uint64_t total = 0;
    for (auto& a : arcs) {
        XJob j; j.archive = a;
        if (!outDir.empty()) {
            std::string d = nz::defaultExtractDest(a, outDir), base = d; int n = 2;
            while (used.count(plat::pathKey(d))) d = base + " (" + std::to_string(n++) + ")";
            used.insert(plat::pathKey(d)); j.dest = d;
        }
        plat::Stat st;
        if (plat::stat(a, st)) total += (a.size() > 4 && a.compare(a.size() - 4, 4, ".001") == 0) ? splitTotal(a) : st.size;
        jobs.push_back(j);
    }
    uint64_t fr = 0; plat::freeSpace(outDir.empty() ? plat::parentDir(arcs[0]) : outDir, fr);
    std::wstring t = jobs.size() == 1 ? L"Extract " + W(plat::baseName(arcs[0])) + L" (" + W(nz::humanSize(total)) + L") into a new folder:\n" +
                                            W(outDir.empty() ? nz::defaultExtractDest(arcs[0]) : jobs[0].dest)
                                      : L"Extract " + std::to_wstring(jobs.size()) + L" archives (" + W(nz::humanSize(total)) + L" in total), each into its own new folder " +
                                            (outDir.empty() ? std::wstring(L"next to it, named after the archive.") : L"inside:\n" + W(outDir));
    t += L"\n\n";
    if (eat) t += L"Each archive is checked in full first. Then, as its files are written and verified, the part of the archive they came from is released, "
                  L"and the archive is PERMANENTLY DELETED at the end. Deleted archives do not go to the Recycle Bin.\n";
    else t += L"The archives will be kept. Nothing is deleted, but you need room for all the extracted files.\n";
    t += L"\nFree space now: " + W(nz::humanSize(fr)) + L".";
    size_t other = g.items.size() - arcs.size();
    if (other) t += L"\n\n\u2022 " + std::to_wstring(other) + L" item(s) in the list are not 7z or ZIP archives and will be left alone.";
    t += L"\n\nContinue?";
    if (msg(t, MB_OKCANCEL | (eat ? MB_ICONWARNING | MB_DEFBUTTON2 : MB_ICONQUESTION)) != IDOK) return;
    startExtract(jobs, eat);
}
static void startBatch(const std::vector<nz::BatchJob>& jobs) {
    if (g.worker.joinable()) g.worker.join();
    g.stopReq = 0; g.pauseFlag = false; g.haveStart = false;
    { std::lock_guard<std::mutex> l(g.mu); g.prog = nz::Progress(); g.result = nz::Result(); g.bres = nz::BatchResult(); }
    setRunningUI(true);
    g.batch = true;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    g.worker = std::thread([jobs]() {
        GuiCb cb;
        nz::BatchResult b = nz::runBatch(jobs, cb);
        { std::lock_guard<std::mutex> l(g.mu); g.bres = b; g.result = b.r; }
        PostMessageW(g.wnd, WM_NZ_DONE, 0, 0);
    });
}
static void startJob(const nz::Options& opt) {
    if (g.worker.joinable()) g.worker.join();
    g.stopReq = 0; g.pauseFlag = false; g.haveStart = false; g.jobArchive = opt.archive;
    { std::lock_guard<std::mutex> l(g.mu); g.prog = nz::Progress(); g.result = nz::Result(); }
    setRunningUI(true);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    g.worker = std::thread([opt]() {
        GuiCb cb;
        nz::Result r = nz::run(opt, cb);
        { std::lock_guard<std::mutex> l(g.mu); g.result = r; }
        PostMessageW(g.wnd, WM_NZ_DONE, 0, 0);
    });
}
static std::wstring fmtTime(double sec) {
    if (sec < 60) return L"Under a minute left";
    uint64_t m = (uint64_t)(sec / 60 + 0.5);
    if (m < 60) return L"About " + std::to_wstring(m) + L" min left";
    return L"About " + std::to_wstring(m / 60) + L" h " + std::to_wstring(m % 60) + L" min left";
}
static void onProgress() {
    nz::Progress p;
    { std::lock_guard<std::mutex> l(g.mu); p = g.prog; }
    if (!g.running) return;
    double frac = p.totalBytes ? (double)p.doneBytes / (double)p.totalBytes : 0;
    if (frac > 1) frac = 1;
    SendMessageW(g.bar, PBM_SETPOS, (WPARAM)(frac * 1000), 0);
    ULONGLONG now = GetTickCount64();
    if (!g.haveStart && p.totalBytes) { g.haveStart = true; g.startTick = now; g.startDone = p.doneBytes; }
    const wchar_t* verb = g.extracting ? (p.phase == 1 ? L"Checking: " : L"Extracting: ") : g.verifying ? L"Checking: " : g.undoing ? L"Putting back: " : L"Compressing: ";
    std::wstring s1 = g.pauseFlag.load() ? L"Paused" : (p.current.empty() ? (g.verifying ? L"Checking…" : g.extracting ? L"Opening the archive…" : L"Preparing…") : verb + W(p.current));
    wchar_t pct[32]; _snwprintf(pct, 31, L"%.1f%%", frac * 100); pct[31] = 0;
    std::wstring s2 = std::wstring(pct);
    if (g.extracting) {
        if (p.jobCount > 1) s2 += L"   •   Archive " + std::to_wstring(p.jobIndex) + L" of " + std::to_wstring(p.jobCount);
        if (p.phase == 1) s2 += L"   •   Checking the archive before anything is changed";
        else {
            s2 += L"   •   Files: " + std::to_wstring(p.filesDone) + L" of " + std::to_wstring(p.filesTotal);
            if (p.reclaimedBytes) s2 += L"   •   Archive space released: " + W(nz::humanSize(p.reclaimedBytes));
        }
    }
    else if (g.undoing) s2 += L"";
    else if (g.verifying) s2 += L"   •   Read " + W(nz::humanSize(p.doneBytes)) + L" of " + W(nz::humanSize(p.totalBytes));
    else s2 += L"   •   Archive: " + W(nz::humanSize(p.packedBytes)) + L"   •   Originals removed: " + W(nz::humanSize(p.reclaimedBytes)) +
               (p.jobCount > 1 ? L"   •   Archive " + std::to_wstring(p.jobIndex) + L" of " + std::to_wstring(p.jobCount)
                               : L"   •   Files: " + std::to_wstring(p.filesDone) + L" of " + std::to_wstring(p.filesTotal));
    double el = (double)(now - g.startTick) / 1000.0;
    if (g.haveStart && el > 8 && p.doneBytes > g.startDone && p.totalBytes > p.doneBytes && !g.pauseFlag.load()) {
        double rate = (double)(p.doneBytes - g.startDone) / el;
        s2 += L"   •   " + fmtTime((double)(p.totalBytes - p.doneBytes) / rate);
    }
    SetWindowTextW(g.st1, s1.c_str()); SetWindowTextW(g.st2, s2.c_str());
    wchar_t title[64]; _snwprintf(title, 63, L"%d%% - NibbleZip %ls", (int)(frac * 100), kVersion); title[63] = 0;
    SetWindowTextW(g.wnd, title);
}
static void onDone() {
    if (g.worker.joinable()) g.worker.join();
    SetThreadExecutionState(ES_CONTINUOUS);
    nz::Result r; nz::VerifyResult v; nz::BatchResult b; bool wasVerify = g.verifying, wasBatch = g.batch, wasUndo = g.undoing, wasExtract = g.extracting;
    App::XSummary xs; { std::lock_guard<std::mutex> l(g.mu); xs = g.xsum; }
    { std::lock_guard<std::mutex> l(g.mu); r = g.result; v = g.vres; b = g.bres; }
    setRunningUI(false);
    if (g.closing) { DestroyWindow(g.wnd); return; }
    if (wasVerify) {
        if (v.ok) msg(L"The archive is intact.\n\n" + std::to_wstring(v.files) + L" file(s), " + W(nz::humanSize(v.bytes)) +
                      L", were read back from disk, decompressed and matched their checksums.", MB_OK | MB_ICONINFORMATION);
        else if (v.cancelled) msg(L"Check cancelled.", MB_OK | MB_ICONINFORMATION);
        else msg(L"A problem was found:\n\n" + W(v.error), MB_OK | MB_ICONERROR);
        return;
    }
    if (wasExtract) {
        std::wstring t; UINT icon = MB_ICONINFORMATION;
        if (xs.ok) {
            t = L"All done.\n\nExtracted " + std::to_wstring(xs.files) + L" file(s), " + W(nz::humanSize(xs.bytes));
            if (xs.total > 1) t += L", from " + std::to_wstring(xs.done) + L" of " + std::to_wstring(xs.total) + L" archives, each into its own folder." +
                                   (xs.skipped ? L" " + std::to_wstring(xs.skipped) + L" could not be extracted and were left untouched (see the notes)." : std::wstring());
            else t += L", into:\n" + W(xs.dest);
            if (xs.eat) t += xs.total > 1 ? L"\n\nEach archive was checked, emptied as its files came out, and removed."
                                          : (xs.removed ? L"\n\nThe archive was checked, emptied as its files came out, and removed." : L"\n\nThe archive could not be removed; see the notes.");
            else t += xs.total > 1 ? L"\n\nThe archives were kept." : L"\n\nThe archive was kept.";
        } else if (xs.paused) {
            t = L"Stopped. Nothing is lost: the files extracted so far are safe, and the rest is still inside the archive.\n\nStart NibbleZip again to carry on.";
        } else {
            icon = MB_ICONERROR;
            t = L"The extraction stopped because of a problem:\n\n" + W(xs.error);
            if (xs.resumable) t += L"\n\nNothing is lost. Once the problem is fixed, start NibbleZip again to carry on.";
        }
        if (!xs.warnings.empty()) {
            t += L"\n\nNotes:";
            for (size_t i = 0; i < xs.warnings.size() && i < 6; i++) t += L"\n\u2022 " + W(xs.warnings[i]);
            if (icon == MB_ICONINFORMATION) icon = MB_ICONWARNING;
        }
        msg(t, MB_OK | icon);
        return;
    }
    if (wasUndo) {
        if (r.ok) msg(std::wstring(L"The file has been put back exactly as it was.\n\n") +
                      (r.archiveRemoved ? L"The archive held nothing else, so it was removed."
                                        : L"The archive was closed and holds the " + std::to_wstring(r.final.filesDone) + L" file(s) that were stored before it. Those originals had already been removed."),
                      MB_OK | MB_ICONINFORMATION);
        else if (r.paused) msg(L"Stopped. Nothing is lost. Next time you can carry on putting the file back, or resume the job instead.", MB_OK | MB_ICONINFORMATION);
        else msg(L"The file could not be put back:\n\n" + W(r.error) + (r.resumable ? L"\n\nNothing is lost. The job is still saved, so you can try again or resume it." : L""), MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring t; UINT icon = MB_ICONINFORMATION;
    // "Original size -> archive size (N% smaller)", shown whether or not originals were deleted.
    auto sizeLines = [&](const wchar_t* archiveLabel) {
        std::wstring o; uint64_t orig = r.final.doneBytes, packed = r.final.packedBytes;
        if (orig > 0) o += L"\nOriginal size: " + W(nz::humanSize(orig));
        o += std::wstring(L"\n") + archiveLabel + W(nz::humanSize(packed));
        if (orig > 0 && r.ok) {
            if (packed < orig) { wchar_t pc[32]; _snwprintf(pc, 31, L"%.1f", 100.0 * (double)(orig - packed) / (double)orig); pc[31] = 0; o += std::wstring(L"  (") + pc + L"% smaller)"; }
            else o += L"  (no smaller: these files were already compressed)";
        }
        return o;
    };
    std::wstring stats = std::to_wstring(r.final.filesDone) + L" file(s) stored in the archive." + sizeLines(L"Archive size: ");
    if (wasBatch) {
        stats = std::to_wstring(b.done) + L" of " + std::to_wstring(b.total) + L" archives were made";
        if (b.skipped) stats += L" (" + std::to_wstring(b.skipped) + L" skipped, see notes)";
        stats += L"." + sizeLines(L"Total size of the archives: ");
    }
    if (r.final.reclaimedBytes > 0) {
        stats += L"\nOriginals removed: " + W(nz::humanSize(r.final.reclaimedBytes));
        if (r.final.reclaimedBytes >= r.final.packedBytes) stats += L"\nNet space saved: " + W(nz::humanSize(r.final.reclaimedBytes - r.final.packedBytes));
        else stats += L"\nNet space saved: none (the archive is " + W(nz::humanSize(r.final.packedBytes - r.final.reclaimedBytes)) + L" larger than what was removed)";
    } else if (r.ok) stats += L"\nOriginals were kept.";
    if (r.ok && !r.partial) { t = L"All done.\n\n" + stats; }
    else if (r.ok && wasBatch) { t = L"Stopped early. Every archive that was made is complete.\n\n" + stats + L"\n\nItems that were not reached are still on disk, untouched. Press Start again to carry on with them."; }
    else if (r.ok) { t = L"The archive was closed early and is complete as it stands.\n\n" + stats + L"\n\nFiles that were not reached are still on disk, untouched."; }
    else if (r.paused && wasBatch) { t = L"Stopped. Nothing is lost: everything stored so far is safe.\n\nPress Start again with the same items and settings to continue where it left off.\n\n" + stats; }
    else if (r.paused) { t = L"Stopped. Nothing is lost: everything stored so far is safe and the job was saved.\n\nStart NibbleZip again to resume it.\n\n" + stats; }
    else {
        icon = MB_ICONERROR;
        t = L"The job stopped because of a problem:\n\n" + W(r.error);
        if (r.resumable && wasBatch) t += L"\n\nNothing is lost. Once the problem is fixed, press Start again with the same items and settings to continue.\n\n" + stats;
        else if (r.resumable) t += L"\n\nNothing is lost. The job was saved and can be resumed once the problem is fixed. Start NibbleZip again to resume it.";
    }
    if (!r.warnings.empty()) {
        t += L"\n\nNotes:";
        for (size_t i = 0; i < r.warnings.size() && i < 8; i++) t += L"\n• " + W(r.warnings[i]);
        if (r.warnings.size() > 8) t += L"\n…and " + std::to_wstring(r.warnings.size() - 8) + L" more.";
        if (icon == MB_ICONINFORMATION) icon = MB_ICONWARNING;
    }
    if (r.ok && !(wasBatch && r.partial)) clearItems();
    msg(t, MB_OK | icon);
}
static bool offerResume(const nz::JobInfo& ji) {
    std::wstring t = L"An unfinished job was found:\n\n" + W(ji.archive) + L"\n\n" + std::to_wstring(ji.filesDone) + L" file(s) are already stored in it";
    if (ji.midFile) t += L", and it stopped part-way through:\n" + W(ji.midFileName) + L"\n\nPart of that file now lives only in the archive.\n\n"
                         L"Yes = resume and finish the job\nNo = undo: put that file back as it was, then close the archive\nCancel = decide later";
    else t += L".\n\nYes = resume now\nNo = close the archive as it is (files not yet stored stay where they are)\nCancel = decide later";
    int r = msg(t, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDYES) { startJob(ji.opt); return true; }
    if (r == IDNO && ji.midFile) { startUndo(ji.archive); return true; }
    if (r == IDNO) { nz::Options o = ji.opt; o.finalizeOnly = true; startJob(o); return true; }
    return false;
}
static void onStartBatch() {
    nz::Options base; base.inputs = g.items; base.format = curFormat(); base.level = curLevel();
    base.eat = SendMessageW(g.eat, BM_GETCHECK, 0, 0) == BST_CHECKED;
    base.split = curSplit();
    if (base.split == (uint64_t)-1) { msg(L"The part size was not understood. Type a size such as 700 MB or 4 GB (at least 1 MB), or choose No.", MB_OK | MB_ICONINFORMATION); return; }
    std::string outDir = U(getText(g.archive));
    while (!outDir.empty() && (outDir.back() == ' ' || outDir.back() == '\t')) outDir.pop_back();
    regSet(L"Format", base.format == nz::FZIP ? 1 : 0); regSet(L"Level", (DWORD)base.level);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    nz::BatchPlan plan = nz::planBatch(base, curMode(), outDir);
    SetCursor(old);
    if (!plan.error.empty()) {
        std::wstring t = W(plan.error);
        for (auto& n : plan.notes) t += L"\n\u2022 " + W(n);
        msg(t, MB_OK | MB_ICONWARNING); return;
    }
    uint64_t fr = 0; std::string where = outDir.empty() ? plat::parentDir(plan.jobs[0].opt.archive) : plat::absPath(outDir);
    plat::freeSpace(where, fr);
    std::wstring t = std::to_wstring(plan.jobs.size()) + L" archive(s) will be made from " + W(nz::humanSize(plan.totalBytes)) + L" of files. Each is named after its original and saved " +
                     (outDir.empty() ? std::wstring(L"next to it.") : L"in:\n" + W(plat::absPath(outDir))) + L"\n\n";
    if (base.eat) t += L"Each original is PERMANENTLY DELETED as soon as its own archive has been written to disk, read back and checked. "
                       L"Deleted originals do not go to the Recycle Bin.\n\nFree space now: " + W(nz::humanSize(fr)) + L".  Largest single item: " + W(nz::humanSize(plan.largestJob)) + L".\n";
    else t += L"Originals will be kept. Nothing is deleted.\n";
    {
        HCURSOR oc = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        uint64_t tot = 0, est = nz::estimateArchiveSize(base.inputs, base.format, base.level, tot);
        SetCursor(oc);
        if (est && tot) {
            if (est >= tot - tot / 33) t += L"\nThese files look already compressed, so expect little or no space saving.\n";
            else t += L"\nEstimated total size of the archives: about " + W(nz::humanSize(est)) + L", saving about " + W(nz::humanSize(tot - est)) + L". This is a rough estimate from samples.\n";
        }
    }
    if (base.split) t += L"\nAny archive larger than " + W(nz::humanSize(base.split)) + L" will be written in parts (.001, .002 and so on).\n";
    for (size_t i = 0; i < plan.notes.size() && i < 6; i++) t += L"\n\u2022 " + W(plan.notes[i]);
    t += L"\n\nContinue?";
    if (msg(t, MB_OKCANCEL | (base.eat ? MB_ICONWARNING | MB_DEFBUTTON2 : MB_ICONQUESTION)) != IDOK) return;
    startBatch(plan.jobs);
}
static void onStart() {
    if (g.items.empty()) { msg(g.mode == 1 ? L"Add at least one archive first." : L"Add at least one file or folder first.", MB_OK | MB_ICONINFORMATION); return; }
    if (g.mode == 1) { onStartExtract(); return; }
    if (curMode() != nz::OutSingle) { onStartBatch(); return; }
    std::string arc = U(getText(g.archive));
    if (arc.empty()) { msg(L"Choose where to save the archive.", MB_OK | MB_ICONINFORMATION); return; }
    arc = plat::absPath(arc);
    nz::JobInfo ji;
    if (nz::archiveExists(arc) && nz::loadJob(arc, ji)) { offerResume(ji); return; }
    nz::Options opt; opt.inputs = g.items; opt.archive = arc; opt.format = curFormat(); opt.level = curLevel();
    opt.eat = SendMessageW(g.eat, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.split = curSplit();
    if (opt.split == (uint64_t)-1) { msg(L"The part size was not understood. Type a size such as 700 MB or 4 GB (at least 1 MB), or choose No.", MB_OK | MB_ICONINFORMATION); return; }
    regSet(L"Format", opt.format == nz::FZIP ? 1 : 0); regSet(L"Level", (DWORD)opt.level);   // remembered for next time
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    nz::Preflight pf = nz::preflight(opt);
    SetCursor(old);
    if (!pf.ok) { msg(W(pf.error), MB_OK | MB_ICONWARNING); return; }
    std::wstring t = std::to_wstring(pf.files) + L" file(s), " + W(nz::humanSize(pf.totalBytes)) + L" in total, will be compressed into:\n" + W(arc) + L"\n\n";
    if (opt.eat) {
        t += L"Each original is PERMANENTLY DELETED as soon as its copy in the archive has been written to disk, read back and checked. "
             L"Deleted originals do not go to the Recycle Bin.\n\n"
             L"Free space now: " + W(nz::humanSize(pf.freeBytes)) + L".  Needed to start: about " + W(nz::humanSize(pf.needBytes)) + L".\n";
    } else {
        t += L"Originals will be kept. Nothing is deleted.\n";
    }
    {
        HCURSOR oc = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        uint64_t tot = 0, est = nz::estimateArchiveSize(opt.inputs, opt.format, opt.level, tot);
        SetCursor(oc);
        if (est && tot) {
            if (est >= tot - tot / 33) t += L"\nThese files look already compressed, so expect little or no space saving.\n";
            else t += L"\nEstimated archive size: about " + W(nz::humanSize(est)) + L", saving about " + W(nz::humanSize(tot - est)) + L". This is a rough estimate from samples.\n";
        }
    }
    if (opt.split) t += L"\nThe archive will be written in parts of " + W(nz::humanSize(opt.split)) + L" each, named " + W(plat::baseName(arc)) + L".001, .002 and so on.\n";
    for (auto& n : pf.notes) t += L"\n• " + W(n);
    t += L"\n\nContinue?";
    if (msg(t, MB_OKCANCEL | (opt.eat ? MB_ICONWARNING | MB_DEFBUTTON2 : MB_ICONQUESTION)) != IDOK) return;
    startJob(opt);
}
static void onStop() {
    if (g.verifying || g.undoing || g.extracting) {
        if (g.extracting) SetWindowTextW(g.st1, L"Stopping safely\u2026");
        g.stopReq = 1; g.pauseFlag = false; EnableWindow(g.stop, FALSE); EnableWindow(g.pause, FALSE); return;
    }
    int r = msg(g.batch ? L"Stop compressing?\n\n"
                          L"Yes = stop now. Everything stored so far is safe and you can continue later.\n\n"
                          L"No = finish the archive in progress, then stop. Items not reached stay where they are.\n\n"
                          L"Cancel = keep going."
                        : L"Stop compressing?\n\n"
                          L"Yes = stop now. Everything stored so far is safe and the job can be resumed later.\n\n"
                          L"No = finish the file in progress, then close the archive. Files not reached stay where they are.\n\n"
                          L"Cancel = keep going.", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
    if (!g.running) return;
    if (r == IDYES) { g.stopReq = 1; g.pauseFlag = false; SetWindowTextW(g.st1, L"Stopping safely…"); EnableWindow(g.pause, FALSE); EnableWindow(g.stop, FALSE); }
    else if (r == IDNO) { g.stopReq = 2; g.pauseFlag = false; EnableWindow(g.stop, FALSE); }
}
static void onStartup() {
    refreshJobsButton();
    if (IsWindowEnabled(g.jobsBtn)) showJobsWindow();
}

// ---------------------------------------------------------------- window
static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.wnd, (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    return c;
}
// Dark mode: the list header is drawn by hand so it never ends up light.
static LRESULT CALLBACK listSub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_NOTIFY && T.dark) {
        NMHDR* n = (NMHDR*)lp;
        HWND hdr = (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0);
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == hdr) {
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                RECT r = cd->rc;
                FillRect(cd->hdc, &r, T.brHdr);
                RECT sep = r; sep.left = sep.right - 1;
                HBRUSH lb = CreateSolidBrush(T.line); FillRect(cd->hdc, &sep, lb); DeleteObject(lb);
                wchar_t txt[64]; txt[0] = 0;
                HDITEMW hi; memset(&hi, 0, sizeof(hi)); hi.mask = HDI_TEXT; hi.pszText = txt; hi.cchTextMax = 63;
                SendMessageW(hdr, HDM_GETITEMW, (WPARAM)cd->dwItemSpec, (LPARAM)&hi);
                SetTextColor(cd->hdc, T.text); SetBkMode(cd->hdc, TRANSPARENT);
                r.left += S(6); r.right -= S(4);
                DrawTextW(cd->hdc, txt, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, wp, lp);
}

// ---------------------------------------------------------------- unfinished jobs
static void refreshJobsButton() {
    if (!g.jobsBtn) return;
    size_t n = nz::pendingJobs().size() + nz::pendingExtractions().size();
    SetWindowTextW(g.jobsBtn, n ? (L"Unfinished jobs (" + std::to_wstring(n) + L")").c_str() : L"No unfinished jobs");
    EnableWindow(g.jobsBtn, n > 0 && !g.running);
}
enum { JB_LIST = 300, JB_HINT, JB_RESUME, JB_UNDO, JB_CLOSEASIS, JB_FORGET, JB_DONE };
struct JobRow { bool extract = false; nz::JobInfo ji; nz::ExtractInfo xi; };
struct JobsState { std::vector<JobRow> rows; int action = 0, index = -1; bool done = false; HWND list = nullptr, hint = nullptr; };
static const wchar_t* kJobsClass = L"NibbleZipJobs";
static int selectedRow(JobsState* st) { return st->list ? (int)SendMessageW(st->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) : -1; }
static void jobsUpdate(HWND h, JobsState* st) {
    int i = selectedRow(st);
    const JobRow* r = (i >= 0 && i < (int)st->rows.size()) ? &st->rows[i] : nullptr;
    EnableWindow(GetDlgItem(h, JB_RESUME), r != nullptr);
    EnableWindow(GetDlgItem(h, JB_UNDO), r && !r->extract && r->ji.midFile);
    EnableWindow(GetDlgItem(h, JB_CLOSEASIS), r && !r->extract && !r->ji.midFile);
    EnableWindow(GetDlgItem(h, JB_FORGET), r && r->extract && !r->xi.eat);
    std::wstring t;
    if (!r) t = L"Select a job to see what you can do with it.";
    else if (r->extract && r->xi.eat)
        t = L"Resume to finish extracting. The archive has been partly emptied as its files came out, so this extraction needs to be finished.";
    else if (r->extract)
        t = L"Resume to finish extracting, or Forget it. The archive was never changed; files extracted so far stay where they are.";
    else if (r->ji.midFile)
        t = L"Resume to finish the job, or Put the file back: “" + W(r->ji.midFileName) + L"” is restored exactly as it was and the archive is closed "
            L"with the " + std::to_wstring(r->ji.filesDone) + L" file(s) stored before it.";
    else
        t = L"Resume to finish the job, or Close the archive as it is: it keeps the " + std::to_wstring(r->ji.filesDone) +
            L" file(s) already stored, and the files not yet reached stay where they are.";
    if (r) t = L"Archive: " + W(r->extract ? r->xi.archive : r->ji.archive) + L"\n" + t;
    SetWindowTextW(st->hint, t.c_str());
}
static LRESULT CALLBACK jobsProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    JobsState* st = (JobsState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (!st || HIWORD(wp) != BN_CLICKED) break;
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == JB_DONE) { st->done = true; return 0; }
        if (LOWORD(wp) == IDOK) { if (IsWindowEnabled(GetDlgItem(h, JB_RESUME))) { st->action = JB_RESUME; st->index = selectedRow(st); st->done = true; } return 0; }
        if (LOWORD(wp) >= JB_RESUME && LOWORD(wp) <= JB_FORGET) { st->action = LOWORD(wp); st->index = selectedRow(st); st->done = true; return 0; }
        break;
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (st && n->idFrom == JB_LIST && n->code == LVN_ITEMCHANGED) jobsUpdate(h, st);
        if (st && n->idFrom == JB_LIST && n->code == NM_DBLCLK && selectedRow(st) >= 0) { st->action = JB_RESUME; st->index = selectedRow(st); st->done = true; }
        break;
    }
    case WM_CLOSE: if (st) st->done = true; return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, T.text); SetBkColor((HDC)wp, T.bg);
        return (LRESULT)T.brBg;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void showJobsWindow() {
    if (g.running) return;
    JobsState st;
    for (auto& j : nz::pendingJobs()) { JobRow r; r.ji = j; st.rows.push_back(r); }
    for (auto& x : nz::pendingExtractions()) { JobRow r; r.extract = true; r.xi = x; st.rows.push_back(r); }
    if (st.rows.empty()) { refreshJobsButton(); msg(L"There are no unfinished jobs.", MB_OK | MB_ICONINFORMATION); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = jobsProc; wc.hInstance = g.inst; wc.lpszClassName = kJobsClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = T.brBg;
        RegisterClassExW(&wc); reg = true;
    }
    const int cw = S(620), ch = S(368), pad = S(16);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT wr = {0, 0, cw, ch}; AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT orc; GetWindowRect(g.wnd, &orc);
    int x = std::max(0L, orc.left + ((orc.right - orc.left) - ww) / 2), y = std::max(0L, orc.top + ((orc.bottom - orc.top) - wh) / 2);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, kJobsClass, L"Unfinished jobs", style, x, y, ww, wh, g.wnd, nullptr, g.inst, nullptr);
    if (!h) return;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);
    darkTitleBar(h);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD sty, DWORD ex, int x0, int y0, int w0, int h0, int id) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | sty, x0, y0, w0, h0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
        return c;
    };
    add(L"STATIC", L"These jobs were stopped before they finished. Nothing in them is lost: choose one to carry on with it.", SS_NOPREFIX, 0, pad, pad, cw - 2 * pad, S(18), -1);
    st.list = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, WS_EX_CLIENTEDGE, pad, pad + S(26), cw - 2 * pad, S(170), JB_LIST);
    SendMessageW(st.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[3] = {L"Job", L"Archive", L"Where it stopped"}; int widths[3] = {72, 230, 282};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c; memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; c.pszText = (LPWSTR)cols[i]; c.cx = S(widths[i]); c.iSubItem = i;
        SendMessageW(st.list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    for (size_t i = 0; i < st.rows.size(); i++) {
        const JobRow& r = st.rows[i];
        std::wstring kind = r.extract ? L"Extract" : L"Compress", arc = W(plat::baseName(r.extract ? r.xi.archive : r.ji.archive)), where;
        if (r.extract) where = L"into " + W(plat::baseName(r.xi.dest)) + (r.xi.eat ? L", archive partly emptied" : L", archive untouched");
        else if (r.ji.midFile) where = std::to_wstring(r.ji.filesDone) + L" stored, part-way through " + W(r.ji.midFileName);
        else where = std::to_wstring(r.ji.filesDone) + L" file(s) stored";
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)i; it.pszText = (LPWSTR)kind.c_str();
        int idx = (int)SendMessageW(st.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iItem = idx; it.iSubItem = 1; it.pszText = (LPWSTR)arc.c_str(); SendMessageW(st.list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = (LPWSTR)where.c_str(); SendMessageW(st.list, LVM_SETITEMW, 0, (LPARAM)&it);
    }
    if (T.dark) {
        SendMessageW(st.list, LVM_SETBKCOLOR, 0, (LPARAM)T.ctl);
        SendMessageW(st.list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)T.ctl);
        SendMessageW(st.list, LVM_SETTEXTCOLOR, 0, (LPARAM)T.text);
        SetWindowSubclass(st.list, listSub, 1, 0);
        darken(st.list, L"DarkMode_Explorer");
        darken((HWND)SendMessageW(st.list, LVM_GETHEADER, 0, 0), L"ItemsView");
    }
    st.hint = add(L"STATIC", L"", SS_NOPREFIX, 0, pad, pad + S(204), cw - 2 * pad, S(70), JB_HINT);
    struct B { int id; const wchar_t* label; int w; };
    B bs[5] = {{JB_RESUME, L"Resume", 92}, {JB_UNDO, L"Put the file back", 128}, {JB_CLOSEASIS, L"Close the archive as it is", 168}, {JB_FORGET, L"Forget", 72}, {JB_DONE, L"Close", 84}};
    int bx = pad, by = ch - pad - S(28);
    for (int i = 0; i < 5; i++) {
        if (i == 4) bx = cw - pad - S(bs[i].w);
        HWND b = add(L"BUTTON", bs[i].label, WS_TABSTOP | (i == 0 ? BS_DEFPUSHBUTTON : 0), 0, bx, by, S(bs[i].w), S(28), bs[i].id);
        darken(b, L"DarkMode_Explorer");
        bx += S(bs[i].w) + S(8);
    }
    LVITEMW sel; memset(&sel, 0, sizeof(sel)); sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED; sel.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(st.list, LVM_SETITEMSTATE, 0, (LPARAM)&sel);
    jobsUpdate(h, &st);
    EnableWindow(g.wnd, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(st.list);
    MSG mm;
    while (!st.done) {
        BOOL r = GetMessageW(&mm, nullptr, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)mm.wParam); break; }
        if (IsDialogMessageW(h, &mm)) continue;
        TranslateMessage(&mm);
        DispatchMessageW(&mm);
    }
    EnableWindow(g.wnd, TRUE);
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    DestroyWindow(h);
    SetForegroundWindow(g.wnd);
    if (st.index < 0 || st.index >= (int)st.rows.size() || !st.action) { refreshJobsButton(); return; }
    const JobRow& r = st.rows[st.index];
    switch (st.action) {
    case JB_RESUME:
        if (r.extract) { XJob xj; xj.archive = r.xi.archive; startExtract(std::vector<XJob>(1, xj), r.xi.eat); }
        else startJob(r.ji.opt);
        break;
    case JB_UNDO: startUndo(r.ji.archive); break;
    case JB_CLOSEASIS: { nz::Options o = r.ji.opt; o.finalizeOnly = true; startJob(o); break; }
    case JB_FORGET:
        if (!nz::forgetExtraction(r.xi.archive)) msg(L"That job could not be forgotten.", MB_OK | MB_ICONWARNING);
        refreshJobsButton();
        break;
    }
}
static void showMoreMenu() {
    HMENU m = CreatePopupMenu(), th = CreatePopupMenu();
    AppendMenuW(th, MF_STRING | (T.pref == 0 ? MF_CHECKED : 0), ID_THEME_AUTO, L"Follow Windows");
    AppendMenuW(th, MF_STRING | (T.pref == 1 ? MF_CHECKED : 0), ID_THEME_LIGHT, L"Light");
    AppendMenuW(th, MF_STRING | (T.pref == 2 ? MF_CHECKED : 0), ID_THEME_DARK, L"Dark");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Deletion log");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"Theme");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"About NibbleZip");
    RECT r; GetWindowRect(g.moreBtn, &r);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0, g.wnd, nullptr);
    DestroyMenu(m);
    if (cmd) PostMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(cmd, BN_CLICKED), 0);
}

static void createControls() {
    ensureFonts();
    const int M = 20, CW = 640 - 2 * M;
    HWND title = mk(L"STATIC", L"NibbleZip", 0, 0, M, 14, 150, 30, -1);
    SendMessageW(title, WM_SETFONT, (WPARAM)g.fontBig, TRUE);
    g.moreBtn = mk(L"BUTTON", L"More", WS_TABSTOP, 0, M + CW - 76, 14, 76, 26, ID_MORE);
    g.verifyBtn = mk(L"BUTTON", L"Check an archive\u2026", WS_TABSTOP, 0, M + CW - 76 - 8 - 134, 14, 134, 26, ID_VERIFY);
    g.jobsBtn = mk(L"BUTTON", L"No unfinished jobs", WS_TABSTOP, 0, M + CW - 76 - 8 - 134 - 8 - 136, 14, 136, 26, ID_JOBS);
    g.modeC = mk(L"BUTTON", L"Compress", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP | WS_GROUP, 0, M, 44, 96, 26, ID_MODE_C);
    g.modeX = mk(L"BUTTON", L"Extract", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 0, M + 98, 44, 96, 26, ID_MODE_X);
    g.subtitle = mk(L"STATIC", L"", SS_NOPREFIX, 0, M + 208, 49, CW - 208, 18, -1);

    g.list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, M, 78, CW, 166, ID_LIST);
    SendMessageW(g.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[3] = {L"Name", L"Size", L"Location"}; int widths[3] = {220, 124, 231};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c; memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; c.pszText = (LPWSTR)cols[i]; c.cx = S(widths[i]); c.iSubItem = i;
        SendMessageW(g.list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    SendMessageW(g.list, LVM_SETCOLUMNWIDTH, 2, LVSCW_AUTOSIZE_USEHEADER);
    if (T.dark) {
        SendMessageW(g.list, LVM_SETBKCOLOR, 0, (LPARAM)T.ctl);
        SendMessageW(g.list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)T.ctl);
        SendMessageW(g.list, LVM_SETTEXTCOLOR, 0, (LPARAM)T.text);
        SetWindowSubclass(g.list, listSub, 1, 0);
        darken(g.list, L"DarkMode_Explorer");
        darken((HWND)SendMessageW(g.list, LVM_GETHEADER, 0, 0), L"ItemsView");
    }
    g.addFiles = mk(L"BUTTON", L"Add files…", WS_TABSTOP, 0, M, 252, 100, 28, ID_ADDFILES);
    g.addFolder = mk(L"BUTTON", L"Add folder…", WS_TABSTOP, 0, M + 108, 252, 100, 28, ID_ADDFOLDER);
    g.removeBtn = mk(L"BUTTON", L"Remove", WS_TABSTOP, 0, M + CW - 188, 252, 90, 28, ID_REMOVE);
    g.clearBtn = mk(L"BUTTON", L"Clear", WS_TABSTOP, 0, M + CW - 90, 252, 90, 28, ID_CLEAR);

    g.lblCreate = mk(L"STATIC", L"Create:", 0, 0, M, 300, 60, 18, -1);
    g.output = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 62, 296, CW - 62, 200, ID_OUTPUT);
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"One archive containing everything");
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"A separate archive for each item in the list");
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"A separate archive for each file (including files inside folders)");
    SendMessageW(g.output, CB_SETCURSEL, 0, 0);

    g.saveLabel = mk(L"STATIC", L"Save archive as:", SS_NOPREFIX, 0, M, 332, CW, 18, -1);
    g.archive = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, M, 352, CW - 98, 24, ID_ARCHIVE);
    g.browse = mk(L"BUTTON", L"Browse…", WS_TABSTOP, 0, M + CW - 90, 350, 90, 28, ID_BROWSE);

    g.lblFormat = mk(L"STATIC", L"Format:", 0, 0, M, 394, 60, 18, -1);
    g.format = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 62, 390, 220, 200, ID_FORMAT);
    SendMessageW(g.format, CB_ADDSTRING, 0, (LPARAM)L"7z  –  smallest files");
    SendMessageW(g.format, CB_ADDSTRING, 0, (LPARAM)L"ZIP  –  opens everywhere");
    SendMessageW(g.format, CB_SETCURSEL, regGet(L"Format", 0) == 1 ? 1 : 0, 0);
    g.lblLevel = mk(L"STATIC", L"Compression:", 0, 0, M + 310, 394, 84, 18, -1);
    g.level = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 396, 390, CW - 396, 200, ID_LEVEL);
    const wchar_t* lv[4] = {L"Fast", L"Normal", L"Maximum", L"Ultra (slow, most RAM)"};
    for (auto s : lv) SendMessageW(g.level, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(g.level, CB_SETCURSEL, std::min<DWORD>(3, regGet(L"Level", 1)), 0);

    g.lblSplit = mk(L"STATIC", L"Split into parts:", 0, 0, M, 432, 100, 18, -1);
    g.split = mk(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0, M + 104, 428, 178, 220, ID_SPLIT);
    const wchar_t* sp[7] = {L"No", L"100 MB", L"700 MB  (CD)", L"1 GB", L"4092 MB  (FAT32 drives)", L"4480 MB  (DVD)", L"23040 MB  (Blu-ray)"};
    for (auto x : sp) SendMessageW(g.split, CB_ADDSTRING, 0, (LPARAM)x);
    SendMessageW(g.split, CB_SETCURSEL, 0, 0);
    g.lblSplitHint = mk(L"STATIC", L"or type a size, for example 250 MB", 0, 0, M + 296, 432, CW - 296, 18, -1);

    // The tick box and its text are separate controls so the text colour follows the theme.
    g.eat = mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 0, M, 465, 18, 18, ID_EAT);
    g.eatLabel = mk(L"STATIC", L"Delete each original once it is safely stored and verified (frees space while compressing)", SS_NOTIFY | SS_NOPREFIX, 0, M + 22, 466, CW - 22, 20, ID_EATLABEL);
    SendMessageW(g.eat, BM_SETCHECK, BST_CHECKED, 0);

    g.info = mk(L"STATIC", L"", SS_NOPREFIX, 0, M, 500, CW, 54, -1);
    g.st1 = mk(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 0, M, 496, CW, 18, -1);
    g.bar = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, M, 518, CW, 18, -1);
    SendMessageW(g.bar, PBM_SETRANGE32, 0, 1000);
    g.st2 = mk(L"STATIC", L"", SS_NOPREFIX, 0, M, 542, CW, 34, -1);

    g.menuBtn = mk(L"BUTTON", L"", WS_TABSTOP, 0, M, 588, 200, 30, ID_MENU);
    g.start = mk(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, M + CW - 130, 588, 130, 30, ID_START);
    g.pause = mk(L"BUTTON", L"Pause", WS_TABSTOP, 0, M + CW - 268, 588, 130, 30, ID_PAUSE);
    g.stop = mk(L"BUTTON", L"Stop…", WS_TABSTOP, 0, M + CW - 130, 588, 130, 30, ID_STOP);
    if (T.dark) {
        darken(g.output, L"DarkMode_CFD"); darken(g.jobsBtn, L"DarkMode_Explorer"); darken(g.moreBtn, L"DarkMode_Explorer"); darken(g.modeC, L"DarkMode_Explorer"); darken(g.modeX, L"DarkMode_Explorer"); darken(g.split, L"DarkMode_CFD");
        HWND btns[] = {g.addFiles, g.addFolder, g.removeBtn, g.clearBtn, g.browse, g.menuBtn, g.start, g.pause, g.stop, g.verifyBtn, g.eat};
        for (HWND b : btns) darken(b, L"DarkMode_Explorer");
        darken(g.format, L"DarkMode_CFD"); darken(g.level, L"DarkMode_CFD"); darken(g.archive, L"DarkMode_CFD");
        SetWindowTheme(g.bar, L"", L"");
        SendMessageW(g.bar, PBM_SETBKCOLOR, 0, (LPARAM)RGB(60, 60, 60));
        SendMessageW(g.bar, PBM_SETBARCOLOR, 0, (LPARAM)RGB(70, 160, 210));
    }
    refreshMenuButton();
    setRunningUI(false);
    setMode(0, false);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_ARCHIVE && code == EN_CHANGE) { if (!g.settingText) { if (g.mode == 0 && curMode() == nz::OutSingle) g.autoName = false; updateInfo(); } return 0; }
        if (id == ID_FORMAT && code == CBN_SELCHANGE) { formatChanged(); return 0; }
        if (id == ID_OUTPUT && code == CBN_SELCHANGE) { outputChanged(); return 0; }
        if (id == ID_EATLABEL && code == STN_CLICKED) {
            if (!g.running) SendMessageW(g.eat, BM_SETCHECK, SendMessageW(g.eat, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            return 0;
        }
        if (code != BN_CLICKED) break;
        switch (id) {
        case ID_ADDFILES: pickItems(false); return 0;
        case ID_ADDFOLDER: pickItems(true); return 0;
        case ID_REMOVE: removeSelected(); return 0;
        case ID_CLEAR: clearItems(); return 0;
        case ID_BROWSE: browseArchive(); return 0;
        case ID_START: onStart(); return 0;
        case ID_VERIFY: pickVerify(); return 0;
        case ID_MODE_C: if (!g.running) setMode(0, true); return 0;
        case ID_MODE_X: if (!g.running) setMode(1, true); return 0;
        case ID_LOG: {
            std::string lp = nz::deletionLogPath(); plat::Stat st;
            if (!plat::stat(lp, st)) msg(L"Nothing has been deleted yet, so there is no log.\n\nOnce NibbleZip removes an original, it is listed here with the date, its size and the archive it went into.", MB_OK | MB_ICONINFORMATION);
            else ShellExecuteW(g.wnd, L"open", L"notepad.exe", (L"\"" + W(lp) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case ID_THEME_AUTO: case ID_THEME_LIGHT: case ID_THEME_DARK: {
            int want = id - ID_THEME_AUTO;
            if (want != T.pref) { T.pref = want; regSet(L"Theme", (DWORD)T.pref); relaunch(); }   // the theme is applied when the window is built
            return 0;
        }
        case ID_MORE: showMoreMenu(); return 0;
        case ID_JOBS: showJobsWindow(); return 0;
        case ID_ABOUT: showAbout(); return 0;
        case ID_STOP: onStop(); return 0;
        case ID_PAUSE: {
            bool p = !g.pauseFlag.load(); g.pauseFlag = p;
            SetWindowTextW(g.pause, p ? L"Continue" : L"Pause");
            if (p) SetWindowTextW(g.st1, L"Paused. Press Continue to carry on.");
            else g.haveStart = false;
            return 0;
        }
        case ID_MENU: {
            bool on = !menuInstalled();
            if (!setMenu(on)) msg(L"Could not update the right-click menu.", MB_OK | MB_ICONWARNING);
            else if (on) msg(L"Added. Right-click any file or folder and choose:\n\n" + std::wstring(kMenuText) +
                             L"\n\nArchives (.7z, .zip) also get:\n\nExtract and reclaim space (NibbleZip)\n\nwhich opens them here, ready to extract\n\nOn Windows 11 these are under \"Show more options\".\n\nIf you move NibbleZip.exe, press this button again to refresh it.", MB_OK | MB_ICONINFORMATION);
            refreshMenuButton();
            return 0;
        }
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom == ID_LIST && n->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_DELETE && !g.running) removeSelected();
        break;
    }
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        if (!g.running) {
            UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> dropped;
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(d, i, nullptr, 0);
                std::wstring p(len + 1, 0);
                DragQueryFileW(d, i, &p[0], len + 1); p.resize(len);
                dropped.push_back(U(p));
            }
            addItems(dropped, 0);
        }
        DragFinish(d);
        return 0;
    }
    case WM_COPYDATA: {
        COPYDATASTRUCT* cd = (COPYDATASTRUCT*)lp;
        if (!cd || (cd->dwData != NZ_COPYDATA_ADD && cd->dwData != NZ_COPYDATA_EXTRACT && cd->dwData != NZ_COPYDATA_DROP) || g.running || !IsWindowEnabled(h)) return FALSE;
        std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        while (!p.empty() && p.back() == 0) p.pop_back();
        addItems(std::vector<std::string>(1, U(p)), cd->dwData == NZ_COPYDATA_EXTRACT ? 2 : cd->dwData == NZ_COPYDATA_ADD ? 1 : 0);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, T.text); SetBkColor((HDC)wp, T.bg);
        return (LRESULT)T.brBg;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, T.text); SetBkColor((HDC)wp, T.ctl);
        return (LRESULT)T.brCtl;
    case WM_NZ_PROGRESS: onProgress(); return 0;
    case WM_NZ_DONE: onDone(); return 0;
    case WM_TIMER:
        if (wp == 1) { KillTimer(h, 1); if (!g.running && IsWindowEnabled(h)) onStartup(); }
        return 0;
    case WM_CLOSE:
        if (g.running) {
            if (g.closing) return 0;
            if (g.verifying || g.undoing || g.extracting) { g.closing = true; g.stopReq = 1; g.pauseFlag = false; return 0; }
            if (msg(L"A job is running.\n\nStop it safely and exit? Everything stored so far is kept and the job can be resumed next time.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
            if (!g.running) { DestroyWindow(h); return 0; }
            g.closing = true; g.stopReq = 1; g.pauseFlag = false;
            SetWindowTextW(g.st1, L"Stopping safely…");
            EnableWindow(g.pause, FALSE); EnableWindow(g.stop, FALSE);
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

struct SendCtx { const std::wstring* path; bool sent; ULONG_PTR kind; };
static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    SendCtx* c = (SendCtx*)lp;
    wchar_t cls[128];
    if (!GetClassNameW(h, cls, 127) || wcscmp(cls, kClass) != 0) return TRUE;
    COPYDATASTRUCT cd; cd.dwData = c->kind; cd.cbData = (DWORD)((c->path->size() + 1) * sizeof(wchar_t)); cd.lpData = (PVOID)c->path->c_str();
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) && res) { c->sent = true; SetForegroundWindow(h); return FALSE; }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g.inst = inst;
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> paths;
    bool forceNew = false, extractMode = false, compressMode = false; std::wstring checkPath;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--new")) forceNew = true;
        else if (!wcscmp(argv[i], L"--extract")) extractMode = true;
        else if (!wcscmp(argv[i], L"--compress")) compressMode = true;
        else if (!wcscmp(argv[i], L"--check") && i + 1 < argc) { checkPath = argv[++i]; forceNew = true; }
        else paths.push_back(argv[i]);
    }

    // When Explorer launches one copy per selected item, later copies hand their item to the first window.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\NibbleZipInstance");
    bool first = GetLastError() != ERROR_ALREADY_EXISTS;
    if (!first && !paths.empty() && !forceNew) {
        for (int attempt = 0; attempt < 40; attempt++) {
            bool all = true;
            for (auto it = paths.begin(); it != paths.end();) {
                SendCtx c{&*it, false, extractMode ? (ULONG_PTR)NZ_COPYDATA_EXTRACT : compressMode ? (ULONG_PTR)NZ_COPYDATA_ADD : (ULONG_PTR)NZ_COPYDATA_DROP};
                EnumWindows(enumProc, (LPARAM)&c);
                if (c.sent) it = paths.erase(it); else { all = false; ++it; }
            }
            if (all) return 0;
            if (attempt > 20 && FindWindowW(kClass, nullptr)) break;   // a window exists but is busy with a job
            Sleep(100);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc; icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr); g.dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc);
    if (g.dpi < 96) g.dpi = 96;
    initTheme();
    if (!acceptDisclaimer()) return 0;

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = T.brBg;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    if (!wc.hIcon) { wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hIconSm = wc.hIcon; }
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(640), S(638)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_ACCEPTFILES);
    g.wnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, windowTitle().c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g.wnd) return 1;
    darkTitleBar(g.wnd);
    createControls();
    if (!paths.empty()) {
        std::vector<std::string> u;
        for (auto& p : paths) u.push_back(U(p));
        addItems(u, extractMode ? 2 : compressMode ? 1 : 0);
    }
    ShowWindow(g.wnd, show);
    UpdateWindow(g.wnd);
    if (menuInstalled()) setMenu(true);   // keep the menu pointing at this exe if it was moved
    if (!checkPath.empty()) startVerify(U(checkPath));
    SetTimer(g.wnd, 1, 1500, nullptr);   // look for unfinished jobs once sibling launches have handed over their items

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g.wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g.worker.joinable()) g.worker.join();
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
