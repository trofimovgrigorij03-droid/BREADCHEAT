// Hlebushek Mod Menu — DLL, которая загружается в процесс игры (Dizzy.exe, 32 бит).
// После инжекта в игре появляется окно меню: сканер памяти, таблица значений с заморозкой,
// сервисные функции окна игры. Показать/скрыть меню: клавиша Insert. Заморозка всего: F1.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN64
#error "Игра 32-битная: собирай DLL как 32-bit (x86)"
#endif

enum { T_I32, T_I16, T_U8, T_F32 };
static const wchar_t* TN[] = {L"int32", L"int16", L"byte", L"float"};
static const int TS[] = {4, 2, 1, 4};
static const size_t MAX_HITS = 500000;

struct Hit   { uintptr_t addr; uint8_t old[4]; };
struct Entry { std::wstring name; uintptr_t addr; int type; bool freeze; uint8_t fv[4]; };

enum {
    ID_TAB = 100, ID_TYPE, ID_VAL, ID_FIRST, ID_MODE, ID_NEXT, ID_RESET, ID_STATUS, ID_HITS, ID_NAME, ID_ADD,
    ID_TLIST, ID_NEWVAL, ID_WRITE, ID_FREEZE, ID_DEL, ID_FREEZEALL, ID_SAVE, ID_LOAD, ID_TSTATUS,
    ID_TOPMOST, ID_ALPHA, ID_OPENDIR, ID_EJECT
};

static HINSTANCE g_self;
static HWND g_wnd, g_game, g_tab;
static HFONT g_font;
static std::vector<HWND> g_pages[4];
static int g_page = 0;
static std::vector<Hit> g_hits;
static int g_scanType = T_I32;
static std::vector<Entry> g_table;
static CRITICAL_SECTION g_cs;
static volatile bool g_quit = false, g_eject = false;
static HANDLE g_freezeThread;
static uintptr_t g_modBase = 0, g_modSize = 0;
static bool g_keyF1 = false, g_keyIns = false;

// ------------------------------------------------------------------ память
static bool Rd(uintptr_t a, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, buf, n, &got) && got == n;
}
static bool Wr(uintptr_t a, const void* buf, size_t n) {
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)a, buf, n, &got) && got == n;
}
static double Load(int t, const uint8_t* b) {
    switch (t) {
        case T_I32: { int32_t x; memcpy(&x, b, 4); return x; }
        case T_I16: { int16_t x; memcpy(&x, b, 2); return x; }
        case T_U8:  return b[0];
        default:    { float f; memcpy(&f, b, 4); return f; }
    }
}
static bool ParseVal(const wchar_t* s, int t, uint8_t out[4]) {
    wchar_t* e = NULL;
    memset(out, 0, 4);
    if (t == T_F32) {
        float f = (float)wcstod(s, &e);
        if (e == s) return false;
        memcpy(out, &f, 4);
        return true;
    }
    long long v = wcstoll(s, &e, 0);
    if (e == s) return false;
    if (t == T_I32) { int32_t x = (int32_t)v; memcpy(out, &x, 4); }
    else if (t == T_I16) { int16_t x = (int16_t)v; memcpy(out, &x, 2); }
    else out[0] = (uint8_t)v;
    return true;
}
static std::wstring ValText(int t, const uint8_t* b) {
    wchar_t buf[48];
    double v = Load(t, b);
    if (t == T_F32) _snwprintf(buf, 48, L"%g", v);
    else _snwprintf(buf, 48, L"%ld", (long)v);
    return buf;
}
static std::wstring AddrText(uintptr_t a) {
    wchar_t b[64];
    if (a >= g_modBase && a < g_modBase + g_modSize) _snwprintf(b, 64, L"exe+0x%X", (unsigned)(a - g_modBase));
    else _snwprintf(b, 64, L"0x%08X", (unsigned)a);
    return b;
}

static void FirstScan(std::vector<Hit>& hits, int type, const uint8_t* pat) {
    hits.clear();
    const int sz = TS[type];
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t maxa = (uintptr_t)si.lpMaximumApplicationAddress;
    std::vector<uint8_t> buf(1 << 20);
    const uintptr_t bufLo = (uintptr_t)buf.data(), bufHi = bufLo + buf.size();
    MEMORY_BASIC_INFORMATION mbi;
    while (a < maxa && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                  !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok && bufLo >= base && bufLo < end) ok = false;  // свой буфер сканера не сканируем
        (void)bufHi;
        if (ok) {
            for (uintptr_t p = base; p < end; p += buf.size()) {
                size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - p);
                if (!Rd(p, buf.data(), n)) continue;
                for (size_t i = 0; i + sz <= n; i += sz) {
                    if (memcmp(&buf[i], pat, sz) == 0) {
                        Hit h;
                        h.addr = p + i;
                        memset(h.old, 0, 4);
                        memcpy(h.old, pat, sz);
                        hits.push_back(h);
                        if (hits.size() >= MAX_HITS) return;
                    }
                }
            }
        }
        a = end;
        if (end <= base) break;
    }
}

static bool Match(int mode, int t, const uint8_t* now, const uint8_t* old, const uint8_t* target) {
    double n = Load(t, now), o = Load(t, old);
    switch (mode) {
        case 0: return n == Load(t, target);
        case 1: return n != o;
        case 2: return n == o;
        case 3: return n > o;
        default: return n < o;
    }
}
static void NextScan(std::vector<Hit>& hits, int type, int mode, const uint8_t* target) {
    std::vector<Hit> out;
    const int sz = TS[type];
    for (size_t i = 0; i < hits.size(); i++) {
        uint8_t now[4] = {0, 0, 0, 0};
        if (!Rd(hits[i].addr, now, sz)) continue;
        if (Match(mode, type, now, hits[i].old, target)) {
            Hit h = hits[i];
            memcpy(h.old, now, 4);
            out.push_back(h);
        }
    }
    hits.swap(out);
}

// ------------------------------------------------------------------ заморозка
static DWORD WINAPI FreezeThread(LPVOID) {
    while (!g_quit) {
        EnterCriticalSection(&g_cs);
        for (size_t i = 0; i < g_table.size(); i++)
            if (g_table[i].freeze) Wr(g_table[i].addr, g_table[i].fv, TS[g_table[i].type]);
        LeaveCriticalSection(&g_cs);
        Sleep(20);
    }
    return 0;
}

// ------------------------------------------------------------------ файлы таблицы
static std::wstring CheatFile() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L"\\/");
    return (k == std::wstring::npos ? L"" : s.substr(0, k + 1)) + L"Hlebushek_cheats.txt";
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static bool SaveTable() {
    std::wstring out;
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_table.size(); i++) {
        const Entry& e = g_table[i];
        wchar_t line[256];
        bool inMod = e.addr >= g_modBase && e.addr < g_modBase + g_modSize;
        _snwprintf(line, 256, L"\t%d\t%s%X\t%d\t%s\r\n", e.type, inMod ? L"+" : L"", (unsigned)(inMod ? e.addr - g_modBase : e.addr),
                   e.freeze ? 1 : 0, ValText(e.type, e.fv).c_str());
        std::wstring nm = e.name;
        std::replace(nm.begin(), nm.end(), L'\t', L' ');
        out += nm + line;
    }
    LeaveCriticalSection(&g_cs);
    std::string u = ToUtf8(out);
    HANDLE h = CreateFileW(CheatFile().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, NULL);
    CloseHandle(h);
    return true;
}
static bool LoadTable() {
    HANDLE h = CreateFileW(CheatFile().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, NULL), r = 0;
    std::string s(sz, 0);
    if (sz) ReadFile(h, &s[0], sz, &r, NULL);
    CloseHandle(h);
    std::wistringstream in(FromUtf8(s));
    std::wstring line;
    std::vector<Entry> t;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);
        std::vector<std::wstring> f;
        std::wstring cur;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == L'\t') { f.push_back(cur); cur.clear(); } else cur += line[i];
        }
        f.push_back(cur);
        if (f.size() < 5) continue;
        Entry e;
        e.name = f[0];
        e.type = _wtoi(f[1].c_str());
        if (e.type < 0 || e.type > 3) continue;
        bool rel = !f[2].empty() && f[2][0] == L'+';
        uintptr_t a = (uintptr_t)wcstoul(f[2].c_str() + (rel ? 1 : 0), NULL, 16);
        e.addr = rel ? g_modBase + a : a;
        e.freeze = false;
        memset(e.fv, 0, 4);
        ParseVal(f[4].c_str(), e.type, e.fv);
        t.push_back(e);
    }
    EnterCriticalSection(&g_cs);
    g_table.swap(t);
    LeaveCriticalSection(&g_cs);
    return true;
}

// ------------------------------------------------------------------ окно игры
static BOOL CALLBACK EnumWnd(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || w == g_wnd || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(w, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    HWND* best = (HWND*)lp;
    RECT br;
    if (!*best) { *best = w; return TRUE; }
    GetWindowRect(*best, &br);
    if (area > (br.right - br.left) * (br.bottom - br.top)) *best = w;
    return TRUE;
}
static HWND FindGameWindow() {
    HWND best = NULL;
    EnumWindows(EnumWnd, (LPARAM)&best);
    return best;
}

// ------------------------------------------------------------------ GUI
static HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }

static HWND Mk(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | (page == 0 ? WS_VISIBLE : 0) | style, x, y, w, h, g_wnd,
                             (HMENU)(INT_PTR)id, g_self, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_pages[page].push_back(c);
    return c;
}
static void ShowPage(int p) {
    g_page = p;
    for (int i = 0; i < 4; i++)
        for (size_t k = 0; k < g_pages[i].size(); k++) ShowWindow(g_pages[i][k], i == p ? SW_SHOW : SW_HIDE);
}
static std::wstring GetText(int id) {
    wchar_t b[256];
    GetWindowTextW(Ctl(id), b, 256);
    return b;
}
static void SetStatus(int id, const std::wstring& s) { SetWindowTextW(Ctl(id), s.c_str()); }

static void RefreshHits() {
    HWND lb = Ctl(ID_HITS);
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    size_t n = std::min<size_t>(g_hits.size(), 400);
    for (size_t i = 0; i < n; i++) {
        uint8_t cur[4] = {0, 0, 0, 0};
        if (!Rd(g_hits[i].addr, cur, TS[g_scanType])) memcpy(cur, g_hits[i].old, 4);
        std::wstring row = AddrText(g_hits[i].addr) + L"    =  " + ValText(g_scanType, cur);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
    wchar_t s[96];
    _snwprintf(s, 96, L"Найдено: %u%s", (unsigned)g_hits.size(), g_hits.size() > 400 ? L" (показаны первые 400)" : L"");
    SetStatus(ID_STATUS, s);
}
static void RefreshTable() {
    HWND lb = Ctl(ID_TLIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_table.size(); i++) {
        const Entry& e = g_table[i];
        uint8_t cur[4] = {0, 0, 0, 0};
        std::wstring v = Rd(e.addr, cur, TS[e.type]) ? ValText(e.type, cur) : L"—";
        std::wstring row = std::wstring(e.freeze ? L"[ЗАМОРОЖ] " : L"          ") + e.name + L"   " + AddrText(e.addr) +
                           L"   " + TN[e.type] + L" = " + v;
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    LeaveCriticalSection(&g_cs);
    if (sel >= 0) SendMessageW(lb, LB_SETCURSEL, sel, 0);
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
}


// ------------------------------------------------------------------ вкладка «Читы»: готовые слоты
struct Slot { const wchar_t* name; const wchar_t* lockDef; std::vector<Hit> hits; int type; };
static Slot g_slots[3] = {{L"Жизни", L"9", {}, T_I32}, {L"Энергия", L"100", {}, T_I32}, {L"Очки", L"99999", {}, T_I32}};
enum { SLOT_BASE = 300, SLOT_STRIDE = 10, S_TYPE = 0, S_VAL, S_FIND, S_RESET, S_LOCK, S_CHECK, S_STATUS };
static int SlotId(int s, int k) { return SLOT_BASE + s * SLOT_STRIDE + k; }

static int FindEntry(const std::wstring& name) {
    for (size_t i = 0; i < g_table.size(); i++) if (g_table[i].name == name) return (int)i;
    return -1;
}
static void SlotBind(int s, const Hit& h) {
    Entry e;
    e.name = g_slots[s].name;
    e.addr = h.addr;
    e.type = g_slots[s].type;
    e.freeze = false;
    Rd(e.addr, e.fv, TS[e.type]);
    EnterCriticalSection(&g_cs);
    int i = FindEntry(e.name);
    if (i >= 0) g_table[i] = e; else g_table.push_back(e);
    LeaveCriticalSection(&g_cs);
    SendMessageW(Ctl(SlotId(s, S_CHECK)), BM_SETCHECK, BST_UNCHECKED, 0);
}
static void SlotFind(int s) {
    Slot& sl = g_slots[s];
    int t = (int)SendMessageW(Ctl(SlotId(s, S_TYPE)), CB_GETCURSEL, 0, 0);
    if (t < 0) t = 0;
    uint8_t v[4];
    if (!ParseVal(GetText(SlotId(s, S_VAL)).c_str(), t, v)) { SetStatus(SlotId(s, S_STATUS), L"Введи текущее значение из игры."); return; }
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (sl.hits.empty() || sl.type != t) { sl.type = t; FirstScan(sl.hits, t, v); }
    else NextScan(sl.hits, t, 0, v);
    SetCursor(old);
    wchar_t st[160];
    if (sl.hits.size() == 1) {
        SlotBind(s, sl.hits[0]);
        _snwprintf(st, 160, L"Найдено и привязано: %s. Поставь галочку.", AddrText(sl.hits[0].addr).c_str());
        sl.hits.clear();
    } else if (sl.hits.empty()) {
        _snwprintf(st, 160, L"Ничего не найдено. Проверь значение/тип и начни заново.");
    } else {
        _snwprintf(st, 160, L"Кандидатов: %u. Измени значение в игре, введи новое и нажми «Найти».", (unsigned)sl.hits.size());
    }
    SetStatus(SlotId(s, S_STATUS), st);
}
static void SlotCheck(int s) {
    bool on = SendMessageW(Ctl(SlotId(s, S_CHECK)), BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnterCriticalSection(&g_cs);
    int i = FindEntry(g_slots[s].name);
    bool ok = i >= 0;
    if (ok) {
        Entry& e = g_table[i];
        uint8_t v[4];
        if (on) {
            if (ParseVal(GetText(SlotId(s, S_LOCK)).c_str(), e.type, v)) { memcpy(e.fv, v, 4); e.freeze = true; }
            else ok = false;
        } else e.freeze = false;
    }
    LeaveCriticalSection(&g_cs);
    if (!ok && on) {
        SendMessageW(Ctl(SlotId(s, S_CHECK)), BM_SETCHECK, BST_UNCHECKED, 0);
        SetStatus(SlotId(s, S_STATUS), L"Сначала найди адрес (кнопка «Найти») и проверь число для удержания.");
    }
}

static void CmdFirst() {
    int t = (int)SendMessageW(Ctl(ID_TYPE), CB_GETCURSEL, 0, 0);
    if (t < 0) t = 0;
    uint8_t pat[4];
    if (!ParseVal(GetText(ID_VAL).c_str(), t, pat)) { SetStatus(ID_STATUS, L"Введи число в поле «Значение»."); return; }
    SetStatus(ID_STATUS, L"Сканирую память…");
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    g_scanType = t;
    FirstScan(g_hits, t, pat);
    SetCursor(old);
    RefreshHits();
}
static void CmdNext() {
    if (g_hits.empty()) { SetStatus(ID_STATUS, L"Сначала сделай первый скан."); return; }
    int mode = (int)SendMessageW(Ctl(ID_MODE), CB_GETCURSEL, 0, 0);
    if (mode < 0) mode = 0;
    uint8_t target[4] = {0, 0, 0, 0};
    if (mode == 0 && !ParseVal(GetText(ID_VAL).c_str(), g_scanType, target)) { SetStatus(ID_STATUS, L"Введи число в поле «Значение»."); return; }
    NextScan(g_hits, g_scanType, mode, target);
    RefreshHits();
}
static void CmdAdd() {
    int i = (int)SendMessageW(Ctl(ID_HITS), LB_GETCURSEL, 0, 0);
    if (i < 0 || (size_t)i >= g_hits.size()) { SetStatus(ID_STATUS, L"Выбери адрес в списке."); return; }
    Entry e;
    e.name = GetText(ID_NAME);
    if (e.name.empty()) e.name = L"Значение";
    e.addr = g_hits[i].addr;
    e.type = g_scanType;
    e.freeze = false;
    Rd(e.addr, e.fv, TS[e.type]);
    EnterCriticalSection(&g_cs);
    g_table.push_back(e);
    LeaveCriticalSection(&g_cs);
    SetStatus(ID_STATUS, L"Добавлено в таблицу (вкладка «Таблица»).");
}
static int TableSel() {
    int i = (int)SendMessageW(Ctl(ID_TLIST), LB_GETCURSEL, 0, 0);
    return (i >= 0 && (size_t)i < g_table.size()) ? i : -1;
}
static void CmdWrite() {
    int i = TableSel();
    if (i < 0) return;
    uint8_t v[4];
    EnterCriticalSection(&g_cs);
    Entry& e = g_table[i];
    if (ParseVal(GetText(ID_NEWVAL).c_str(), e.type, v)) {
        Wr(e.addr, v, TS[e.type]);
        memcpy(e.fv, v, 4);
    } else SetStatus(ID_TSTATUS, L"Некорректное значение.");
    LeaveCriticalSection(&g_cs);
    RefreshTable();
}
static void CmdFreeze() {
    int i = TableSel();
    if (i < 0) return;
    EnterCriticalSection(&g_cs);
    Entry& e = g_table[i];
    e.freeze = !e.freeze;
    if (e.freeze) Rd(e.addr, e.fv, TS[e.type]);
    LeaveCriticalSection(&g_cs);
    RefreshTable();
}
static void CmdDel() {
    int i = TableSel();
    if (i < 0) return;
    EnterCriticalSection(&g_cs);
    g_table.erase(g_table.begin() + i);
    LeaveCriticalSection(&g_cs);
    RefreshTable();
}
static void CmdFreezeAll() {
    EnterCriticalSection(&g_cs);
    bool on = true;
    for (size_t i = 0; i < g_table.size(); i++) if (g_table[i].freeze) on = false;
    for (size_t i = 0; i < g_table.size(); i++) {
        g_table[i].freeze = on;
        if (on) Rd(g_table[i].addr, g_table[i].fv, TS[g_table[i].type]);
    }
    LeaveCriticalSection(&g_cs);
    RefreshTable();
}

static void BuildUi() {
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 4, 4, 512, 492, g_wnd,
                            (HMENU)(INT_PTR)ID_TAB, g_self, NULL);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const wchar_t* names[4] = {L"Читы", L"Сканер", L"Таблица", L"Окно игры"};
    for (int i = 0; i < 4; i++) {
        TCITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)names[i];
        SendMessageW(g_tab, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    const DWORD EDIT = WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, BTN = BS_PUSHBUTTON | WS_TABSTOP;
    const DWORD LIST = WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT;


    // --- вкладка 0: читы (слоты)
    Mk(0, L"STATIC", L"Как пользоваться: введи текущее значение из игры → «Найти» → измени его в игре (потеряй жизнь, подбери монету) → "
                     L"введи новое значение → «Найти». Когда адрес найден, он привяжется сам — поставь галочку «Держать».",
       0, 14, 38, 488, 52, 0);
    const wchar_t* sn[3] = {L"Жизни", L"Энергия", L"Очки"};
    for (int s = 0; s < 3; s++) {
        int y = 98 + s * 118;
        Mk(0, L"BUTTON", sn[s], BS_GROUPBOX, 10, y, 496, 110, 0);
        HWND tc = Mk(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 20, y + 22, 80, 200, SlotId(s, S_TYPE));
        for (int i = 0; i < 4; i++) SendMessageW(tc, CB_ADDSTRING, 0, (LPARAM)TN[i]);
        SendMessageW(tc, CB_SETCURSEL, 0, 0);
        Mk(0, L"STATIC", L"Сейчас в игре:", 0, 110, y + 26, 100, 20, 0);
        Mk(0, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 212, y + 22, 70, 24, SlotId(s, S_VAL));
        Mk(0, L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, 292, y + 21, 90, 26, SlotId(s, S_FIND));
        Mk(0, L"BUTTON", L"Сброс", BS_PUSHBUTTON | WS_TABSTOP, 390, y + 21, 90, 26, SlotId(s, S_RESET));
        Mk(0, L"BUTTON", L"Держать значение:", BS_AUTOCHECKBOX | WS_TABSTOP, 20, y + 56, 150, 24, SlotId(s, S_CHECK));
        Mk(0, L"EDIT", g_slots[s].lockDef, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 172, y + 56, 70, 24, SlotId(s, S_LOCK));
        Mk(0, L"STATIC", L"", 0, 20, y + 86, 476, 20, SlotId(s, S_STATUS));
    }

    // --- вкладка 1: сканер
    Mk(1, L"STATIC", L"Тип:", 0, 14, 40, 36, 20, 0);
    HWND cb = Mk(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 52, 36, 90, 200, ID_TYPE);
    for (int i = 0; i < 4; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)TN[i]);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
    Mk(1, L"STATIC", L"Значение:", 0, 156, 40, 66, 20, 0);
    Mk(1, L"EDIT", L"", EDIT, 224, 36, 90, 24, ID_VAL);
    Mk(1, L"BUTTON", L"Первый скан", BTN, 326, 35, 110, 26, ID_FIRST);
    HWND cm = Mk(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 14, 72, 140, 200, ID_MODE);
    const wchar_t* modes[5] = {L"равно значению", L"изменилось", L"не изменилось", L"увеличилось", L"уменьшилось"};
    for (int i = 0; i < 5; i++) SendMessageW(cm, CB_ADDSTRING, 0, (LPARAM)modes[i]);
    SendMessageW(cm, CB_SETCURSEL, 0, 0);
    Mk(1, L"BUTTON", L"Следующий скан", BTN, 164, 71, 130, 26, ID_NEXT);
    Mk(1, L"BUTTON", L"Сброс", BTN, 304, 71, 70, 26, ID_RESET);
    Mk(1, L"STATIC", L"Найдено: 0", 0, 14, 104, 480, 20, ID_STATUS);
    Mk(1, L"LISTBOX", L"", LIST | WS_TABSTOP, 14, 126, 488, 230, ID_HITS, WS_EX_CLIENTEDGE);
    Mk(1, L"STATIC", L"Имя:", 0, 14, 368, 36, 20, 0);
    Mk(1, L"EDIT", L"Жизни", EDIT, 54, 364, 150, 24, ID_NAME);
    Mk(1, L"BUTTON", L"В таблицу", BTN, 214, 363, 110, 26, ID_ADD);
    Mk(1, L"STATIC",
       L"Как найти, например, жизни: введи текущее число → «Первый скан» → потеряй жизнь → введи новое число → "
       L"«Следующий скан» (режим «равно»). Если значение не видно, используй «изменилось» / «уменьшилось». "
       L"Когда останется 1–3 адреса — выбери и нажми «В таблицу».",
       0, 14, 400, 488, 84, 0);

    // --- вкладка 1: таблица
    Mk(2, L"LISTBOX", L"", LIST | WS_TABSTOP, 14, 36, 488, 280, ID_TLIST, WS_EX_CLIENTEDGE);
    Mk(2, L"EDIT", L"", EDIT, 14, 326, 100, 24, ID_NEWVAL);
    Mk(2, L"BUTTON", L"Записать", BTN, 122, 325, 90, 26, ID_WRITE);
    Mk(2, L"BUTTON", L"Заморозить", BTN, 218, 325, 100, 26, ID_FREEZE);
    Mk(2, L"BUTTON", L"Удалить", BTN, 324, 325, 80, 26, ID_DEL);
    Mk(2, L"BUTTON", L"Заморозить всё (F1)", BTN, 14, 360, 160, 26, ID_FREEZEALL);
    Mk(2, L"BUTTON", L"Сохранить", BTN, 184, 360, 100, 26, ID_SAVE);
    Mk(2, L"BUTTON", L"Загрузить", BTN, 290, 360, 100, 26, ID_LOAD);
    Mk(2, L"STATIC", L"", 0, 14, 396, 488, 20, ID_TSTATUS);
    Mk(2, L"STATIC",
       L"Заморозка каждые 20 мс записывает в адрес сохранённое значение — так можно сделать бесконечные жизни или энергию. "
       L"Адреса внутри exe сохраняются как exe+смещение, адреса в куче при перезапуске игры придётся искать заново.",
       0, 14, 420, 488, 64, 0);

    // --- вкладка 2: окно игры
    Mk(3, L"BUTTON", L"Окно игры поверх всех окон", BS_AUTOCHECKBOX | WS_TABSTOP, 14, 40, 300, 24, ID_TOPMOST);
    Mk(3, L"STATIC", L"Прозрачность окна игры:", 0, 14, 80, 300, 20, 0);
    HWND tb = Mk(3, L"msctls_trackbar32", L"", TBS_AUTOTICKS | WS_TABSTOP, 14, 102, 340, 34, ID_ALPHA);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(40, 255));
    SendMessageW(tb, TBM_SETPOS, TRUE, 255);
    Mk(3, L"BUTTON", L"Открыть папку игры", BTN, 14, 160, 200, 28, ID_OPENDIR);
    Mk(3, L"BUTTON", L"Выгрузить меню из игры", BTN, 14, 200, 200, 28, ID_EJECT);
    Mk(3, L"STATIC", L"Горячие клавиши:\r\nInsert — показать/скрыть меню\r\nF1 — заморозить/разморозить всю таблицу\r\n\r\n"
                     L"Закрытие окна крестиком только прячет меню.",
       0, 14, 250, 488, 90, 0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        BuildUi();
        SetTimer(h, 1, 250, NULL);
        SetTimer(h, 2, 50, NULL);
        return 0;
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)l;
        if (n->hwndFrom == g_tab && n->code == TCN_SELCHANGE) {
            ShowPage((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
            if (g_page == 2) RefreshTable();
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) >= SLOT_BASE && LOWORD(w) < SLOT_BASE + 3 * SLOT_STRIDE) {
            int s = (LOWORD(w) - SLOT_BASE) / SLOT_STRIDE, k = (LOWORD(w) - SLOT_BASE) % SLOT_STRIDE;
            if (k == S_FIND) SlotFind(s);
            else if (k == S_RESET) { g_slots[s].hits.clear(); SetStatus(SlotId(s, S_STATUS), L"Поиск сброшен."); }
            else if (k == S_CHECK) SlotCheck(s);
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_FIRST: CmdFirst(); break;
        case ID_NEXT: CmdNext(); break;
        case ID_RESET: g_hits.clear(); RefreshHits(); break;
        case ID_ADD: CmdAdd(); break;
        case ID_WRITE: CmdWrite(); break;
        case ID_FREEZE: CmdFreeze(); break;
        case ID_DEL: CmdDel(); break;
        case ID_FREEZEALL: CmdFreezeAll(); break;
        case ID_SAVE: SetStatus(ID_TSTATUS, SaveTable() ? L"Таблица сохранена: Hlebushek_cheats.txt (рядом с игрой)." : L"Не удалось сохранить файл."); break;
        case ID_LOAD: SetStatus(ID_TSTATUS, LoadTable() ? L"Таблица загружена." : L"Файл Hlebushek_cheats.txt не найден."); RefreshTable(); break;
        case ID_TOPMOST:
            if (g_game) SetWindowPos(g_game, SendMessageW(Ctl(ID_TOPMOST), BM_GETCHECK, 0, 0) == BST_CHECKED ? HWND_TOPMOST : HWND_NOTOPMOST,
                                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case ID_OPENDIR: {
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(NULL, p, MAX_PATH);
            wchar_t* s = wcsrchr(p, L'\\');
            if (s) *s = 0;
            ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_EJECT: g_eject = true; DestroyWindow(h); break;
        }
        return 0;
    case WM_HSCROLL:
        if ((HWND)l == Ctl(ID_ALPHA) && g_game) {
            int a = (int)SendMessageW(Ctl(ID_ALPHA), TBM_GETPOS, 0, 0);
            LONG ex = GetWindowLongW(g_game, GWL_EXSTYLE);
            SetWindowLongW(g_game, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            SetLayeredWindowAttributes(g_game, 0, (BYTE)a, LWA_ALPHA);
        }
        return 0;
    case WM_TIMER:
        if (w == 1) {
            if (g_page == 2 && IsWindowVisible(h)) RefreshTable();
        } else {
            if (!g_game || !IsWindow(g_game)) g_game = FindGameWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            bool ours = pid == GetCurrentProcessId();
            bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0, f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
            if (ours && ins && !g_keyIns) {
                if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
                else { ShowWindow(h, SW_SHOWNORMAL); SetForegroundWindow(h); }
            }
            if (ours && f1 && !g_keyF1) CmdFreezeAll();
            g_keyIns = ins;
            g_keyF1 = f1;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI MenuThread(LPVOID) {
    INITCOMMONCONTROLSEX ic;
    ic.dwSize = sizeof ic;
    ic.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&ic);
    InitializeCriticalSection(&g_cs);

    HMODULE exe = GetModuleHandleW(NULL);
    g_modBase = (uintptr_t)exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    g_modSize = nt->OptionalHeader.SizeOfImage;

    g_freezeThread = CreateThread(NULL, 0, FreezeThread, NULL, 0, NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"HlebushekModMenu";
    RegisterClassExW(&wc);

    HWND game = FindGameWindow();
    RECT want = {0, 0, 520, 500};
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    int x = 80, y = 80;
    if (game) { RECT r; GetWindowRect(game, &r); x = r.left + 30; y = r.top + 60; }
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Хлебушек — Mod Menu",
                              WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, want.right - want.left, want.bottom - want.top,
                              NULL, NULL, g_self, NULL);
    if (hw) {
        ShowWindow(hw, SW_SHOWNORMAL);
        UpdateWindow(hw);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
    }
    g_quit = true;
    WaitForSingleObject(g_freezeThread, 2000);
    CloseHandle(g_freezeThread);
    if (g_game && IsWindow(g_game)) {
        SetWindowPos(g_game, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LONG ex = GetWindowLongW(g_game, GWL_EXSTYLE);
        SetWindowLongW(g_game, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
    UnregisterClassW(wc.lpszClassName, g_self);
    DeleteObject(g_font);
    DeleteCriticalSection(&g_cs);
    if (g_eject) FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(NULL, 0, MenuThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
