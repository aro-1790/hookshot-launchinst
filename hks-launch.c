#include "hks-common.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef HKS_LAUNCH_BITS
#error "Define HKS_LAUNCH_BITS to 32 or 64"
#endif

#if HKS_LAUNCH_BITS == 32
#define HKS_WORKER_SUBDIR      L"Win32"
#define HKS_WORKER_NAME        L"Hookshot.32.exe"
#define HKS_LAUNCHER_EXE_TITLE L"hks-launch32.exe"
#elif HKS_LAUNCH_BITS == 64
#define HKS_WORKER_SUBDIR      L"x64"
#define HKS_WORKER_NAME        L"Hookshot.64.exe"
#define HKS_LAUNCHER_EXE_TITLE L"hks-launch64.exe"
#else
#error "HKS_LAUNCH_BITS must be 32 or 64"
#endif

#define HKS_CMDLINE_INDICATOR L'|'

typedef struct {
    uint64_t processHandle;
    uint64_t threadHandle;
    uint8_t  enableDebugFeatures;
    uint8_t  _pad[7];
    uint64_t injectionResult;
    uint64_t extendedInjectionResult;
} HKS_INJECT_REQUEST;

_Static_assert(sizeof(HKS_INJECT_REQUEST) == 40,
               "SInjectRequest size mismatch");
_Static_assert(offsetof(HKS_INJECT_REQUEST, injectionResult) == 24,
               "SInjectRequest field offset mismatch");

#define HKS_INJECT_SUCCESS 0u

static void show_error(const wchar_t *msg);
static void show_err_str(const wchar_t *prefix, const wchar_t *path);
static void show_err_code(const wchar_t *action, DWORD err);
static void show_err_code2(const wchar_t *action, const wchar_t *path, DWORD err);
static DWORD ensure_marker(const wchar_t *marker_path);
static int prompt_and_store_hookshot_dir(wchar_t *out_dir, size_t dir_sz);
static int locate_worker(wchar_t *out, size_t outsz);
static int exe_has_compat_layer(const wchar_t *exe_path);
static void warn_if_compat_layer_present(const wchar_t *target_path);
static int relaunch_elevated(const wchar_t *self, const wchar_t *args);
static int run_worker(HANDLE target_process, HANDLE target_thread, const wchar_t *worker_path);
static int do_launcher(const wchar_t *self, const wchar_t *args);

static void show_error(const wchar_t *msg)
{
    MessageBoxW(NULL, msg, HKS_LAUNCHER_EXE_TITLE, MB_ICONERROR | MB_OK);
}

static void show_err_str(const wchar_t *prefix, const wchar_t *path)
{
    wchar_t msg[PATHBUF + 256];
    _snwprintf(msg, PATHBUF + 256, L"%ls:\n%ls", prefix, path);
    msg[PATHBUF + 255] = L'\0';
    show_error(msg);
}

static void show_err_code(const wchar_t *action, DWORD err)
{
    wchar_t msg[256];
    _snwprintf(msg, 256, L"Failed to %ls (error %lu).", action, err);
    msg[255] = L'\0';
    show_error(msg);
}

static void show_err_code2(const wchar_t *action, const wchar_t *path, DWORD err)
{
    wchar_t msg[PATHBUF + 256];
    _snwprintf(msg, PATHBUF + 256, L"Failed to %ls:\n%ls\n\nError: %lu", action, path, err);
    msg[PATHBUF + 255] = L'\0';
    show_error(msg);
}

static DWORD ensure_marker(const wchar_t *marker_path)
{
    HANDLE h = CreateFileW(marker_path, GENERIC_WRITE, FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(h);
    return ERROR_SUCCESS;
}

static int prompt_and_store_hookshot_dir(wchar_t *out_dir, size_t dir_sz)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    HKEY hKey;

    (void)dir_sz;

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = L"Where's your Hookshot folder root?";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;

    if (!SHGetPathFromIDListW(pidl, out_dir)) {
        ILFree(pidl);
        return 0;
    }
    ILFree(pidl);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD bytes = (DWORD)((wcslen(out_dir) + 1) * sizeof(wchar_t));
        DWORD_PTR result;
        RegSetValueExW(hKey, L"HookshotDir", 0, REG_SZ, (const BYTE *)out_dir, bytes);
        RegCloseKey(hKey);

        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, &result);
    }

    SetEnvironmentVariableW(L"HookshotDir", out_dir);
    return 1;
}

static int locate_worker(wchar_t *out, size_t outsz)
{
    wchar_t dir[PATHBUF];
    DWORD n = GetEnvironmentVariableW(L"HookshotDir", dir, PATHBUF);

    if (n == 0 || n >= PATHBUF) {
        if (!prompt_and_store_hookshot_dir(dir, PATHBUF)) {
            show_error(L"Hookshot folder path was not provided.");
            return 0;
        }
    }

    _snwprintf(out, outsz, L"%ls\\%ls\\%ls",
               dir, HKS_WORKER_SUBDIR, HKS_WORKER_NAME);
    out[outsz - 1] = L'\0';

    if (!file_exists_w(out)) {
        show_err_str(L"Hookshot injection worker not found at", out);
        return 0;
    }
    return 1;
}

/* Returns non-zero if the specified executable has a Windows compatibility
   layer registered against it (per-user or machine-wide). Compatibility modes
   configured through the file's Properties dialog are stored in the
   AppCompatFlags\Layers key, keyed by full executable path. */
static int exe_has_compat_layer(const wchar_t *exe_path)
{
    static const wchar_t kLayersKey[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    static const HKEY kHives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    static const REGSAM kViews[] = { 0, KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    size_t i, j;

    if (exe_path == NULL) return 0;

    for (i = 0; i < sizeof(kHives) / sizeof(kHives[0]); ++i) {
        for (j = 0; j < sizeof(kViews) / sizeof(kViews[0]); ++j) {
            HKEY hKey = NULL;
            DWORD type = 0;
            DWORD size = 0;

            if (RegOpenKeyExW(kHives[i], kLayersKey, 0,
                              KEY_QUERY_VALUE | kViews[j], &hKey) != ERROR_SUCCESS)
                continue;

            if (RegQueryValueExW(hKey, exe_path, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                RegCloseKey(hKey);
                return 1;
            }

            RegCloseKey(hKey);
        }
    }

    return 0;
}

/* The game process ends up under compatibility shims either way: layers active
   on the launcher (for example, "Windows XP (Service Pack 2)") are inherited by
   the game, and layers registered directly against the real executable apply to
   it as well. Hookshot's own injection worker is isolated from the shims, but
   HookModules are loaded into the game and may not behave correctly under them.
   Warn the user, naming the source(s), and continue. */
static void warn_if_compat_layer_present(const wchar_t *target_path)
{
    wchar_t layer[PATHBUF];
    DWORD layer_len = GetEnvironmentVariableW(L"__COMPAT_LAYER", layer, PATHBUF);
    int on_launcher = (layer_len > 0 && layer_len < PATHBUF);
    int on_target = exe_has_compat_layer(target_path);
    const wchar_t *source;
    wchar_t msg[512];

    if (!on_launcher && !on_target) return;

    if (on_launcher && on_target)
        source = L"both this launcher and the real executable";
    else if (on_target)
        source = L"the real executable";
    else
        source = L"this launcher";

    _snwprintf(msg, 512,
               L"A compatibility mode is set on %ls. "
               L"Some HookModules might not play nice with it "
               L"(turn off if having issues).",
               source);
    msg[511] = L'\0';

    MessageBoxW(NULL, msg, HKS_LAUNCHER_EXE_TITLE, MB_OK | MB_ICONWARNING);
}

static int relaunch_elevated(const wchar_t *self, const wchar_t *args)
{
    SHELLEXECUTEINFOW sei;
    DWORD exit_code = 0;

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize  = sizeof(sei);
    sei.fMask   = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb  = L"runas";
    sei.lpFile  = self;
    sei.lpParameters = (args != NULL && args[0] != L'\0') ? args : NULL;
    sei.nShow   = SW_SHOWDEFAULT;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) return 1;

        show_err_code(L"re-launch elevated", err);
        return 1;
    }

    if (sei.hProcess == NULL || sei.hProcess == INVALID_HANDLE_VALUE)
        return 1;

    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &exit_code);
    CloseHandle(sei.hProcess);
    return (int)exit_code;
}

static int run_worker(HANDLE target_process, HANDLE target_thread,
                      const wchar_t *worker_path)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE mapping = INVALID_HANDLE_VALUE;
    HKS_INJECT_REQUEST *req = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE dup_proc = INVALID_HANDLE_VALUE;
    HANDLE dup_thread = INVALID_HANDLE_VALUE;
    wchar_t cmdline[PATHBUF * 2];
    DWORD wait_res, worker_exit = 0;
    int ok = 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                 0, sizeof(HKS_INJECT_REQUEST), NULL);
    if (mapping == NULL) {
        show_error(L"CreateFileMapping failed.");
        goto cleanup;
    }

    req = (HKS_INJECT_REQUEST *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
                                              0, 0, 0);
    if (req == NULL) {
        show_error(L"MapViewOfFile failed.");
        goto cleanup;
    }

    ZeroMemory(req, sizeof(*req));
    req->injectionResult = 1;

    _snwprintf(cmdline, PATHBUF * 2, L"\"%ls\" %lc%llx",
               worker_path, (wint_t)HKS_CMDLINE_INDICATOR,
               (unsigned long long)(uintptr_t)mapping);
    cmdline[(PATHBUF * 2) - 1] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* Windows applies compatibility shims (such as "Windows XP (Service Pack
       2)") to a process based on its image path and exposes the active shims
       through the inherited __COMPAT_LAYER variable. The worker performs the
       injection, so it must not run under those shims. Temporarily clear the
       variable for the worker spawn, then restore it. The game process is
       created separately and keeps its compatibility mode. */
    {
        wchar_t saved_layer[PATHBUF];
        DWORD layer_len = GetEnvironmentVariableW(L"__COMPAT_LAYER",
                                                  saved_layer, PATHBUF);
        int layer_present = (layer_len > 0 && layer_len < PATHBUF);
        BOOL started;
        DWORD spawn_err;

        if (layer_present)
            SetEnvironmentVariableW(L"__COMPAT_LAYER", NULL);

        started = CreateProcessW(worker_path, cmdline, NULL, NULL,
                                 TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);
        spawn_err = started ? ERROR_SUCCESS : GetLastError();

        if (layer_present)
            SetEnvironmentVariableW(L"__COMPAT_LAYER", saved_layer);

        if (!started) {
            show_err_code2(L"launch injection worker", worker_path, spawn_err);
            goto cleanup;
        }
    }

    if (!DuplicateHandle(GetCurrentProcess(), target_process,
                         pi.hProcess, &dup_proc, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), target_thread,
                         pi.hProcess, &dup_thread, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        show_error(L"DuplicateHandle failed.");
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        goto cleanup;
    }

    req->processHandle       = (uint64_t)(uintptr_t)dup_proc;
    req->threadHandle        = (uint64_t)(uintptr_t)dup_thread;
    req->enableDebugFeatures = 0;

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    wait_res = WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &worker_exit);
    if (wait_res != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, (UINT)-1);
    CloseHandle(pi.hProcess);

    if (wait_res != WAIT_OBJECT_0 || worker_exit != 0) {
        show_err_code(L"run injection worker", worker_exit);
        goto cleanup;
    }

    if ((uint32_t)req->injectionResult != HKS_INJECT_SUCCESS) {
        wchar_t msg[256];
        _snwprintf(msg, 256,
                   L"Hookshot failed to inject the game.\n\n"
                   L"Result code: %llu\nExtended: %llu",
                   (unsigned long long)req->injectionResult,
                   (unsigned long long)req->extendedInjectionResult);
        msg[255] = L'\0';
        show_error(msg);
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (req) UnmapViewOfFile(req);
    if (mapping != INVALID_HANDLE_VALUE) CloseHandle(mapping);
    return ok;
}

static int do_launcher(const wchar_t *self, const wchar_t *args)
{
    wchar_t selfname[PATHBUF];
    wchar_t dir[PATHBUF];
    wchar_t prefixed[PATHBUF];
    wchar_t target[PATHBUF];
    wchar_t worker[PATHBUF];
    wchar_t marker[PATHBUF];
    wchar_t cmdline[PATHBUF * 2];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 0;
    DWORD err;
    int ok;

    basename_of(self, selfname, PATHBUF);
    dirname_of(self, dir, PATHBUF);
    apply_prefix(selfname, prefixed, PATHBUF);
    _snwprintf(target, PATHBUF, L"%ls\\%ls", dir, prefixed);
    target[PATHBUF - 1] = L'\0';

    _snwprintf(marker, PATHBUF, L"%ls.hookshot", target);
    marker[PATHBUF - 1] = L'\0';

    /* Build the target's command line with the real executable as argv[0],
       then forward our own arguments. This mirrors what the official Hookshot
       launcher does and avoids feeding the game a mismatched module name. */
    if (args != NULL && args[0] != L'\0')
        _snwprintf(cmdline, PATHBUF * 2, L"\"%ls\" %ls", target, args);
    else
        _snwprintf(cmdline, PATHBUF * 2, L"\"%ls\"", target);
    cmdline[(PATHBUF * 2) - 1] = L'\0';

    if (!file_exists_w(target)) {
        wchar_t msg[PATHBUF + 256];
        _snwprintf(msg, PATHBUF + 256,
                   L"Real executable not found:\n%ls\n\n"
                   L"Expected alongside this launcher as:\n  %ls",
                   target, prefixed);
        msg[PATHBUF + 255] = L'\0';
        show_error(msg);
        return 1;
    }

    if (!locate_worker(worker, PATHBUF)) return 1;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(target, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        err = GetLastError();
        if (err == ERROR_ELEVATION_REQUIRED)
            return relaunch_elevated(self, args);

        show_err_code2(L"create target process", target, err);
        return 1;
    }

    err = ensure_marker(marker);
    if (err != ERROR_SUCCESS) {
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (err == ERROR_ACCESS_DENIED)
            return relaunch_elevated(self, args);

        show_err_code2(L"create authorization marker", marker, err);
        return 1;
    }

    /* Warn only once the launch is actually going to proceed. Placing this here
       rather than at startup means an elevation re-launch (which supersedes the
       instance that requested it) does not re-trigger the warning: only the
       instance that is about to inject the game reaches this point. */
    warn_if_compat_layer_present(target);

    ok = run_worker(pi.hProcess, pi.hThread, worker);

    if (!ok) {
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(marker);
        return 1;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    DeleteFileW(marker);
    return (int)exit_code;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR lpCmdLine, int nCmdShow)
{
    wchar_t self[PATHBUF];
    DWORD n;
    int rc;
    HRESULT hr;

    (void)hInstance; (void)hPrevInstance; (void)nCmdShow;

    /* SHBrowseForFolder with BIF_NEWDIALOGSTYLE requires COM to be initialized
       on the calling thread. */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    n = GetModuleFileNameW(NULL, self, PATHBUF);
    if (n == 0 || n >= PATHBUF) {
        show_error(L"Could not determine own path.");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    rc = do_launcher(self, lpCmdLine);
    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}