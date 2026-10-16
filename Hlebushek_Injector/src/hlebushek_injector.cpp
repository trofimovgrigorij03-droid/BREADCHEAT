// Hlebushek Injector — один exe: внутри него вшита hlebushek_menu.dll. При запуске он сам
//   1) находит процесс Dizzy.exe (или запускает игру из соседней папки),
//   2) устанавливает DLL в папку игры,
//   3) загружает её в процесс игры — в игре появляется меню с читами.
// Запуск:   Hlebushek_Injector.exe            — установить и загрузить меню
//           Hlebushek_Injector.exe --eject    — выгрузить меню и удалить DLL из папки игры
//           --proc Имя.exe — другой процесс;  --dll путь — использовать внешнюю DLL вместо вшитой;  --no-pause
// Собирать как 32-bit: игра 32-битная, адрес LoadLibraryW в её процессе совпадает с нашим только при одинаковой разрядности.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>
#ifdef EMBED_DLL
#include "dll_blob.h"
#endif

#ifdef _WIN64
#error "Собирай инжектор как 32-bit (x86)"
#endif

static std::wstring DirOf(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static DWORD FindPid(const std::wstring& name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    if (Process32FirstW(s, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return pid;
}
static HMODULE FindModule(DWORD pid, const std::wstring& dllName) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    HMODULE found = NULL;
    if (Module32FirstW(s, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName.c_str()) == 0) { found = me.hModule; break; }
        } while (Module32NextW(s, &me));
    }
    CloseHandle(s);
    return found;
}
static bool RunRemote(HANDLE proc, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD* exitCode) {
    HANDLE t = CreateRemoteThread(proc, NULL, 0, fn, arg, 0, NULL);
    if (!t) return false;
    WaitForSingleObject(t, 10000);
    GetExitCodeThread(t, exitCode);
    CloseHandle(t);
    return true;
}

static std::wstring ProcessDir(HANDLE proc) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(proc, 0, buf, &n)) return DirOf(buf);
    return L"";
}
static bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    return ok;
}
static bool g_pause = true;
static int Done(int code) {
    if (g_pause) {
        printf("\nНажми Enter, чтобы закрыть окно...");
        getchar();
    }
    return code;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    std::wstring dir = DirOf(selfPath), procName = L"Dizzy.exe", extDll;
    const std::wstring dllName = L"hlebushek_menu.dll";
    bool eject = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--eject") eject = true;
        else if (a == L"--no-pause") g_pause = false;
        else if (a == L"--proc" && i + 1 < argc) procName = argv[++i];
        else if (a == L"--dll" && i + 1 < argc) extDll = argv[++i];
    }
    bool useEmbedded = false;
#ifdef EMBED_DLL
    useEmbedded = extDll.empty();
#endif
    if (!useEmbedded && extDll.empty()) extDll = dir + L"\\" + dllName;

    printf("=== Hlebushek Injector ===\n");
    if (!eject && !useEmbedded && !Exists(extDll)) {
        printf("DLL не найдена: %ls\nСобери инжектор через build.bat (DLL вшивается в exe) или положи DLL рядом.\n", extDll.c_str());
        return Done(1);
    }

    DWORD pid = FindPid(procName);
    if (!pid && !eject) {
        // игра не запущена: ищем Dizzy.exe рядом и запускаем сами
        const wchar_t* rel[] = {L"\\", L"\\..\\", L"\\Hlebushek_v0.5\\", L"\\Hlebushek_v0.5\\Hlebushek_v0.5\\", L"\\..\\Hlebushek_v0.5\\"};
        for (size_t i = 0; i < 5 && !pid; i++) {
            std::wstring exe = dir + rel[i] + procName;
            if (!Exists(exe)) continue;
            wchar_t fullExe[MAX_PATH];
            GetFullPathNameW(exe.c_str(), MAX_PATH, fullExe, NULL);
            std::wstring wd = DirOf(fullExe);
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof si);
            si.cb = sizeof si;
            std::wstring cmd = L"\"" + std::wstring(fullExe) + L"\"";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);
            printf("Игра не запущена, запускаю: %ls\n", fullExe);
            if (CreateProcessW(fullExe, &buf[0], NULL, NULL, FALSE, 0, NULL, wd.c_str(), &si, &pi)) {
                pid = pi.dwProcessId;
                WaitForInputIdle(pi.hProcess, 8000);
                Sleep(1500);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    if (!pid) {
        printf("Процесс %ls не найден. Запусти игру и повтори (или положи инжектор рядом с игрой).\n", procName.c_str());
        return Done(1);
    }
    printf("Найден процесс %ls, PID %lu\n", procName.c_str(), (unsigned long)pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        printf("Не удалось открыть процесс (ошибка %lu). Запусти инжектор от имени администратора.\n", GetLastError());
        return Done(1);
    }
    BOOL wow = FALSE, selfWow = FALSE;
    IsWow64Process(proc, &wow);
    IsWow64Process(GetCurrentProcess(), &selfWow);
    if (wow != selfWow) {
        printf("Разрядность инжектора и игры не совпадает. Игра 32-битная — собери инжектор как 32-bit.\n");
        CloseHandle(proc);
        return Done(1);
    }

    std::wstring gameDir = ProcessDir(proc);
    std::wstring installed = (gameDir.empty() ? dir : gameDir) + L"\\" + dllName;
    HMODULE existing = FindModule(pid, dllName);
    DWORD code = 0;

    if (eject) {
        if (existing) {
            LPTHREAD_START_ROUTINE fr = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary");
            if (RunRemote(proc, fr, (LPVOID)existing, &code)) printf("Меню выгружено из игры.\n");
            else printf("Не удалось выгрузить меню (ошибка %lu).\n", GetLastError());
            Sleep(300);
        } else printf("Меню в игре не загружено.\n");
        if (DeleteFileW(installed.c_str())) printf("DLL удалена из папки игры.\n");
        CloseHandle(proc);
        return Done(0);
    }
    if (existing) {
        printf("Меню уже загружено в игру. Нажми Insert в окне игры, чтобы показать его.\n");
        CloseHandle(proc);
        return Done(0);
    }

    // --- установка DLL в папку игры (если нельзя писать — во временную папку)
    std::wstring dllPath = extDll;
    if (useEmbedded) {
#ifdef EMBED_DLL
        dllPath = installed;
        if (!WriteAll(dllPath, DLL_DATA, DLL_SIZE)) {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            dllPath = std::wstring(tmp) + dllName;
            if (!WriteAll(dllPath, DLL_DATA, DLL_SIZE)) {
                printf("Не удалось записать DLL ни в папку игры, ни во временную папку.\n");
                CloseHandle(proc);
                return Done(1);
            }
        }
        printf("DLL установлена: %ls\n", dllPath.c_str());
#endif
    }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(dllPath.c_str(), MAX_PATH, full, NULL);
    dllPath = full;

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, NULL)) {
        printf("Не удалось записать путь к DLL в процесс (ошибка %lu).\n", GetLastError());
        CloseHandle(proc);
        return Done(1);
    }
    LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    bool ok = RunRemote(proc, ll, remote, &code);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    if (!ok || code == 0) {
        printf("Не удалось загрузить DLL в игру. Убедись, что всё собрано как 32-bit.\n%ls\n", dllPath.c_str());
        return Done(1);
    }
    printf("Готово! Меню загружено в игру. Если окно не видно — нажми Insert в окне игры.\n");
    return Done(0);
}
