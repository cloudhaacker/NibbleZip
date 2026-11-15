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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "platform.h"

static const wchar_t* kClass = L"NibbleZipMainWindow";
static const wchar_t* kTitle = L"NibbleZip";
static const wchar_t* kMenuText = L"Compress and reclaim space (NibbleZip)";
#define WM_NZ_PROGRESS (WM_APP + 1)
#define WM_NZ_DONE (WM_APP + 2)
#define NZ_COPYDATA_ADD 0x4E5A0001

enum { ID_LIST = 100, ID_ADDFILES, ID_ADDFOLDER, ID_REMOVE, ID_CLEAR, ID_ARCHIVE, ID_BROWSE, ID_FORMAT, ID_LEVEL, ID_EAT,
       ID_START, ID_PAUSE, ID_STOP, ID_MENU, ID_VERIFY, ID_THEME, ID_EATLABEL, ID_OUTPUT, ID_LOG, ID_SPLIT };

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
         eatLabel = nullptr, verifyBtn = nullptr, themeBtn = nullptr, output = nullptr, saveLabel = nullptr, logBtn = nullptr, split = nullptr;
    HFONT font = nullptr, fontBig = nullptr;
    int dpi = 96;
    std::vector<std::string> items;
    bool autoName = true, settingText = false, running = false, closing = false, verifying = false;
    nz::VerifyResult vres;
    bool batch = false, undoing = false;
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
static const wchar_t* themeLabel() { return T.pref == 1 ? L"Theme: Light" : T.pref == 2 ? L"Theme: Dark" : L"Theme: Auto"; }

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
    if (regGet(L"DisclaimerAccepted", 0) == 1) return true;
    int r = themedBox(nullptr,
        L"NibbleZip permanently deletes your original files as it compresses them. That is its purpose, and it cannot be undone.\n\n"
        L"This software is provided \"as is\", without warranty of any kind. The authors and contributors are not responsible or liable "
        L"for any loss of data or any other damage arising from its use. You use it entirely at your own risk.\n\n"
        L"Keep a backup of anything you cannot afford to lose.\n\n"
        L"Press OK to accept these terms and continue, or Cancel to exit.",
        L"NibbleZip – please read", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (r != IDOK) return false;
    regSet(L"DisclaimerAccepted", 1);
    return true;
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
static bool setMenu(bool on) {
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, kMenuKeys[i]); continue; }
        HKEY k, c;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kMenuKeys[i], 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) { ok = false; continue; }
        std::wstring exe = exePath();
        setStr(k, nullptr, kMenuText);
        setStr(k, L"Icon", L"\"" + exe + L"\"");
        setStr(k, L"MultiSelectModel", L"Player");
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            setStr(c, nullptr, L"\"" + exe + L"\" \"%1\"");
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
static void updateAutoName() {
    if (!g.autoName || curMode() != nz::OutSingle) return;
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
    if (g.items.empty()) t = L"Drop files or folders here, or use the buttons above.";
    else {
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
    std::wstring name = W(plat::baseName(p)), size = st.isDir ? L"Folder" : W(nz::humanSize(st.size)), loc = W(plat::parentDir(p));
    it.mask = LVIF_TEXT; it.iItem = (int)g.items.size() - 1; it.pszText = (LPWSTR)name.c_str();
    int idx = (int)SendMessageW(g.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iItem = idx; it.iSubItem = 1; it.pszText = (LPWSTR)size.c_str(); SendMessageW(g.list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 2; it.pszText = (LPWSTR)loc.c_str(); SendMessageW(g.list, LVM_SETITEMW, 0, (LPARAM)&it);
    updateAutoName(); updateInfo();
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
    g.autoName = true;
    updateAutoName(); updateInfo();
}
static void pickItems(bool folders) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return;
    DWORD o = 0; d->GetOptions(&o);
    d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0));
    d->SetTitle(folders ? L"Choose folders to compress" : L"Choose files to compress");
    if (SUCCEEDED(d->Show(g.wnd))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr)) && arr) {
            DWORD n = 0; arr->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it)) && it) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { addItem(U(p)); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    d->Release();
}
static void browseArchive() {
    if (curMode() != nz::OutSingle) {   // several archives: pick the folder they go in
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return;
        DWORD o = 0; d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Choose the folder for the archives");
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
    if (curMode() != nz::OutSingle) return;
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
    HWND inputs[] = {g.output, g.logBtn, g.split, g.archive, g.format, g.level, g.eat, g.eatLabel, g.addFiles, g.addFolder, g.removeBtn, g.clearBtn, g.browse, g.menuBtn, g.verifyBtn, g.themeBtn};
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
    else { g.verifying = false; g.batch = false; g.undoing = false; SetWindowTextW(g.wnd, kTitle); updateInfo(); }
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
    std::wstring s1 = g.pauseFlag.load() ? L"Paused" : (p.current.empty() ? (g.verifying ? L"Checking…" : L"Preparing…") : (g.verifying ? L"Checking: " : g.undoing ? L"Putting back: " : L"Compressing: ") + W(p.current));
    wchar_t pct[32]; _snwprintf(pct, 31, L"%.1f%%", frac * 100); pct[31] = 0;
    std::wstring s2 = std::wstring(pct);
    if (g.undoing) s2 += L"";
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
    wchar_t title[64]; _snwprintf(title, 63, L"%d%% - NibbleZip", (int)(frac * 100)); title[63] = 0;
    SetWindowTextW(g.wnd, title);
}
static void onDone() {
    if (g.worker.joinable()) g.worker.join();
    SetThreadExecutionState(ES_CONTINUOUS);
    nz::Result r; nz::VerifyResult v; nz::BatchResult b; bool wasVerify = g.verifying, wasBatch = g.batch, wasUndo = g.undoing;
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
    std::wstring stats = std::to_wstring(r.final.filesDone) + L" file(s) stored in the archive.\nArchive size: " + W(nz::humanSize(r.final.packedBytes));
    if (wasBatch) {
        stats = std::to_wstring(b.done) + L" of " + std::to_wstring(b.total) + L" archives were made";
        if (b.skipped) stats += L" (" + std::to_wstring(b.skipped) + L" skipped, see notes)";
        stats += L".\nTotal size of the archives: " + W(nz::humanSize(r.final.packedBytes));
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
    if (g.items.empty()) { msg(L"Add at least one file or folder first.", MB_OK | MB_ICONINFORMATION); return; }
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
    if (g.verifying || g.undoing) { g.stopReq = 1; g.pauseFlag = false; EnableWindow(g.stop, FALSE); return; }
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
    std::vector<nz::JobInfo> jobs = nz::pendingJobs();
    for (auto& j : jobs) { if (g.running) break; offerResume(j); }
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

static void createControls() {
    ensureFonts();
    const int M = 20, CW = 640 - 2 * M;
    HWND title = mk(L"STATIC", L"NibbleZip", 0, 0, M, 14, 200, 30, -1);
    g.logBtn = mk(L"BUTTON", L"Deletion log", WS_TABSTOP, 0, M + CW - 100 - 8 - 150 - 8 - 100, 14, 100, 26, ID_LOG);
    SendMessageW(title, WM_SETFONT, (WPARAM)g.fontBig, TRUE);
    g.verifyBtn = mk(L"BUTTON", L"Check an archive…", WS_TABSTOP, 0, M + CW - 100 - 8 - 150, 14, 150, 26, ID_VERIFY);
    g.themeBtn = mk(L"BUTTON", themeLabel(), WS_TABSTOP, 0, M + CW - 100, 14, 100, 26, ID_THEME);
    mk(L"STATIC", L"Compresses files and frees up their space as it goes, so you don't need room for both.", 0, 0, M, 46, CW, 18, -1);

    g.list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, M, 74, CW, 170, ID_LIST);
    SendMessageW(g.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[3] = {L"Name", L"Size", L"Location"}; int widths[3] = {230, 90, 255};
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

    mk(L"STATIC", L"Create:", 0, 0, M, 300, 60, 18, -1);
    g.output = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 62, 296, CW - 62, 200, ID_OUTPUT);
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"One archive containing everything");
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"A separate archive for each item in the list");
    SendMessageW(g.output, CB_ADDSTRING, 0, (LPARAM)L"A separate archive for each file (including files inside folders)");
    SendMessageW(g.output, CB_SETCURSEL, 0, 0);

    g.saveLabel = mk(L"STATIC", L"Save archive as:", SS_NOPREFIX, 0, M, 332, CW, 18, -1);
    g.archive = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, M, 352, CW - 98, 24, ID_ARCHIVE);
    g.browse = mk(L"BUTTON", L"Browse…", WS_TABSTOP, 0, M + CW - 90, 350, 90, 28, ID_BROWSE);

    mk(L"STATIC", L"Format:", 0, 0, M, 394, 60, 18, -1);
    g.format = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 62, 390, 220, 200, ID_FORMAT);
    SendMessageW(g.format, CB_ADDSTRING, 0, (LPARAM)L"7z  –  smallest files");
    SendMessageW(g.format, CB_ADDSTRING, 0, (LPARAM)L"ZIP  –  opens everywhere");
    SendMessageW(g.format, CB_SETCURSEL, regGet(L"Format", 0) == 1 ? 1 : 0, 0);
    mk(L"STATIC", L"Compression:", 0, 0, M + 310, 394, 84, 18, -1);
    g.level = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, M + 396, 390, CW - 396, 200, ID_LEVEL);
    const wchar_t* lv[4] = {L"Fast", L"Normal", L"Maximum", L"Ultra (slow, most RAM)"};
    for (auto s : lv) SendMessageW(g.level, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(g.level, CB_SETCURSEL, std::min<DWORD>(3, regGet(L"Level", 1)), 0);

    mk(L"STATIC", L"Split into parts:", 0, 0, M, 432, 100, 18, -1);
    g.split = mk(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0, M + 104, 428, 178, 220, ID_SPLIT);
    const wchar_t* sp[7] = {L"No", L"100 MB", L"700 MB  (CD)", L"1 GB", L"4092 MB  (FAT32 drives)", L"4480 MB  (DVD)", L"23040 MB  (Blu-ray)"};
    for (auto x : sp) SendMessageW(g.split, CB_ADDSTRING, 0, (LPARAM)x);
    SendMessageW(g.split, CB_SETCURSEL, 0, 0);
    mk(L"STATIC", L"or type a size, for example 250 MB", 0, 0, M + 296, 432, CW - 296, 18, -1);

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
        darken(g.output, L"DarkMode_CFD"); darken(g.logBtn, L"DarkMode_Explorer"); darken(g.split, L"DarkMode_CFD");
        HWND btns[] = {g.addFiles, g.addFolder, g.removeBtn, g.clearBtn, g.browse, g.menuBtn, g.start, g.pause, g.stop, g.verifyBtn, g.themeBtn, g.eat};
        for (HWND b : btns) darken(b, L"DarkMode_Explorer");
        darken(g.format, L"DarkMode_CFD"); darken(g.level, L"DarkMode_CFD"); darken(g.archive, L"DarkMode_CFD");
        SetWindowTheme(g.bar, L"", L"");
        SendMessageW(g.bar, PBM_SETBKCOLOR, 0, (LPARAM)RGB(60, 60, 60));
        SendMessageW(g.bar, PBM_SETBARCOLOR, 0, (LPARAM)RGB(70, 160, 210));
    }
    refreshMenuButton();
    setRunningUI(false);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_ARCHIVE && code == EN_CHANGE) { if (!g.settingText) { if (curMode() == nz::OutSingle) g.autoName = false; updateInfo(); } return 0; }
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
        case ID_LOG: {
            std::string lp = nz::deletionLogPath(); plat::Stat st;
            if (!plat::stat(lp, st)) msg(L"Nothing has been deleted yet, so there is no log.\n\nOnce NibbleZip removes an original, it is listed here with the date, its size and the archive it went into.", MB_OK | MB_ICONINFORMATION);
            else ShellExecuteW(g.wnd, L"open", L"notepad.exe", (L"\"" + W(lp) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case ID_THEME:
            T.pref = (T.pref + 1) % 3; regSet(L"Theme", (DWORD)T.pref);
            relaunch();   // the theme is applied when the window is built
            return 0;
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
                             L"\n\nOn Windows 11 it is under \"Show more options\".\n\nIf you move NibbleZip.exe, press this button again to refresh it.", MB_OK | MB_ICONINFORMATION);
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
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(d, i, nullptr, 0);
                std::wstring p(len + 1, 0);
                DragQueryFileW(d, i, &p[0], len + 1); p.resize(len);
                addItem(U(p));
            }
        }
        DragFinish(d);
        return 0;
    }
    case WM_COPYDATA: {
        COPYDATASTRUCT* cd = (COPYDATASTRUCT*)lp;
        if (!cd || cd->dwData != NZ_COPYDATA_ADD || g.running || !IsWindowEnabled(h)) return FALSE;
        std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        while (!p.empty() && p.back() == 0) p.pop_back();
        addItem(U(p));
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
    case WM_TIMER: if (wp == 1) { KillTimer(h, 1); if (!g.running && IsWindowEnabled(h)) onStartup(); } return 0;
    case WM_CLOSE:
        if (g.running) {
            if (g.closing) return 0;
            if (g.verifying || g.undoing) { g.closing = true; g.stopReq = 1; g.pauseFlag = false; return 0; }
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

struct SendCtx { const std::wstring* path; bool sent; };
static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    SendCtx* c = (SendCtx*)lp;
    wchar_t cls[128];
    if (!GetClassNameW(h, cls, 127) || wcscmp(cls, kClass) != 0) return TRUE;
    COPYDATASTRUCT cd; cd.dwData = NZ_COPYDATA_ADD; cd.cbData = (DWORD)((c->path->size() + 1) * sizeof(wchar_t)); cd.lpData = (PVOID)c->path->c_str();
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) && res) { c->sent = true; SetForegroundWindow(h); return FALSE; }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g.inst = inst;
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> paths;
    bool forceNew = false; std::wstring checkPath;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--new")) forceNew = true;
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
                SendCtx c{&*it, false};
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
    g.wnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g.wnd) return 1;
    darkTitleBar(g.wnd);
    createControls();
    for (auto& p : paths) addItem(U(p));
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
