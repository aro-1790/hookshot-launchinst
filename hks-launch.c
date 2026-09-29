/* TaskDialogIndirect and the TaskDialog structures are only declared for
   Vista-and-later targets, and hks-common.h pulls in windows.h before anything
   else, so the target version has to be set here. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "hks-common.h"

#include <commctrl.h>
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

/* What the compatibility prompt decided. */
typedef enum {
    HKS_WARN_PROCEED = 0,   /* carry on (also the answer when no prompt is due) */
    HKS_WARN_CANCEL         /* the user asked for the launch to be abandoned */
} HKS_WARN_RESULT;

/* Button identifiers for the prompt, picked so they cannot collide with the
   IDOK/IDCANCEL that TaskDialog reports for a dismissed dialog. */
#define HKS_BTN_CONTINUE 101
#define HKS_BTN_NOWARN   102
#define HKS_BTN_CANCEL   103

static void show_error(const wchar_t *msg);
static void show_err_str(const wchar_t *prefix, const wchar_t *path);
static void show_err_code(const wchar_t *action, DWORD err);
static void show_err_code2(const wchar_t *action, const wchar_t *path, DWORD err);
static DWORD ensure_marker(const wchar_t *marker_path);
static int prompt_and_store_hookshot_dir(wchar_t *out_dir, size_t dir_sz);
static int locate_worker(wchar_t *out, size_t outsz);
static HKS_WARN_RESULT warn_if_compat_layer_present(const wchar_t *target_path,
                                                    const wchar_t *self_path);
static void append_quoted_argument(wchar_t *out, size_t outsz, size_t *len, const wchar_t *text);
static void build_target_command_line(const wchar_t *target, wchar_t *out, size_t outsz);
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

/* Compatibility tokens that can plausibly upset a HookModule: OS-version lies
   (WIN*, NT*, VISTA*) and the display and graphics shims. Elevation, DPI
   awareness and Windows' own Detectors* telemetry tokens are not listed, so
   they cannot raise the prompt. */
static int is_noteworthy_shim_token(const wchar_t *token, size_t len)
{
    static const wchar_t *kGraphics[] = {
        L"DWM8AND16BITMITIGATION", L"DISABLEDWM", L"DISABLETHEMES",
        L"256COLOR", L"16BITCOLOR", L"640X480",
        L"DISABLEDXMAXIMIZEDWINDOWEDMODE"
    };
    size_t i;

    if (len == 0) return 0;

    if ((len >= 3 && _wcsnicmp(token, L"WIN", 3) == 0) ||
        (len >= 2 && _wcsnicmp(token, L"NT", 2) == 0) ||
        (len >= 5 && _wcsnicmp(token, L"VISTA", 5) == 0))
        return 1;

    for (i = 0; i < sizeof(kGraphics) / sizeof(kGraphics[0]); ++i) {
        size_t n = wcslen(kGraphics[i]);
        if (len == n && _wcsnicmp(token, kGraphics[i], n) == 0)
            return 1;
    }

    return 0;
}

/* Copies the noteworthy tokens of a space-separated layer list into out,
   space-separated. The '$' and '~' markers are prefixes, not tokens. */
static void collect_shim_tokens(const wchar_t *layers, wchar_t *out, size_t outsz)
{
    const wchar_t *p = layers;
    size_t len;

    if (layers == NULL || out == NULL || outsz == 0) return;
    out[0] = L'\0';

    while (*p != L'\0') {
        while (*p == L' ' || *p == L'\t') ++p;
        if (*p == L'\0') break;

        len = 0;
        while (p[len] != L'\0' && p[len] != L' ' && p[len] != L'\t') ++len;
        while (len > 0 && (p[0] == L'$' || p[0] == L'~')) {
            ++p;
            --len;
        }

        if (is_noteworthy_shim_token(p, len) &&
            wcslen(out) + (out[0] ? 1u : 0u) + len + 1 <= outsz) {
            if (out[0]) wcscat(out, L" ");
            wcsncat(out, p, len);
        }

        p += len;
    }
}

/* Copies <exe_path>'s compatibility layer value data, if it has one. The value
   name is the full executable path, in the AppCompatFlags\Layers key. */
static int layer_value_for_exe(const wchar_t *exe_path, wchar_t *out, size_t outsz)
{
    static const wchar_t kLayersKey[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    static const HKEY kHives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    static const REGSAM kViews[] = { 0, KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    size_t i, j;
    DWORD cap = (DWORD)(outsz * sizeof(wchar_t));

    if (out == NULL || outsz == 0) return 0;
    out[0] = L'\0';
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
                (type == REG_SZ || type == REG_EXPAND_SZ) &&
                size >= sizeof(wchar_t) && size <= cap) {
                if (RegQueryValueExW(hKey, exe_path, NULL, &type, (LPBYTE)out, &size) == ERROR_SUCCESS) {
                    out[outsz - 1] = L'\0';
                    RegCloseKey(hKey);
                    return 1;
                }
            }

            RegCloseKey(hKey);
        }
    }

    out[0] = L'\0';
    return 0;
}

/* The target ends up under compatibility shims either way: layers active in the
   launcher process are inherited by the target, and layers registered directly
   against the real executable apply to it as well. Hookshot's own injection
   worker is isolated from the shims, but HookModules are loaded into the target
   and may not behave correctly under them.

   Warn the user, naming the source(s) and the tokens. The opt-out is an empty,
   persistent _hks_<exe>.nowarn marker: unlike the authorization marker the
   launcher never removes it, and it suppresses the prompt for both sources. */
static HKS_WARN_RESULT warn_if_compat_layer_present(const wchar_t *target_path,
                                                    const wchar_t *self_path)
{
    wchar_t nowarn[PATHBUF];
    wchar_t env_raw[PATHBUF];
    wchar_t self_raw[PATHBUF];
    wchar_t target_raw[PATHBUF];
    wchar_t env_tokens[512];
    wchar_t self_tokens[512];
    wchar_t target_tokens[512];
    wchar_t self_line[512];
    wchar_t target_line[512];
    wchar_t msg[1024];
    wchar_t details[PATHBUF * 3];
    wchar_t fallback[1152];
    TASKDIALOGCONFIG cfg;
    TASKDIALOG_BUTTON buttons[3];
    HRESULT hr;
    int pressed = 0;

    path_with_suffix(target_path, NOWARN_SUFFIX, nowarn, PATHBUF);
    if (file_exists_w(nowarn)) return HKS_WARN_PROCEED;

    env_raw[0] = L'\0';
    if (GetEnvironmentVariableW(L"__COMPAT_LAYER", env_raw, PATHBUF) >= PATHBUF)
        env_raw[0] = L'\0';

    layer_value_for_exe(self_path, self_raw, PATHBUF);
    layer_value_for_exe(target_path, target_raw, PATHBUF);

    collect_shim_tokens(env_raw, env_tokens, 512);
    collect_shim_tokens(self_raw, self_tokens, 512);
    collect_shim_tokens(target_raw, target_tokens, 512);

    if (!env_tokens[0] && !self_tokens[0] && !target_tokens[0])
        return HKS_WARN_PROCEED;

    self_line[0] = L'\0';
    target_line[0] = L'\0';

    if (self_tokens[0])
        _snwprintf(self_line, 512, L"The launcher is configured with: %ls",
                   self_tokens);
    else if (env_tokens[0])
        _snwprintf(self_line, 512, L"The launcher process is running with: %ls",
                   env_tokens);

    if (target_tokens[0])
        _snwprintf(target_line, 512, L"The real executable is configured with: %ls",
                   target_tokens);

    self_line[511] = L'\0';
    target_line[511] = L'\0';

    _snwprintf(msg, 1024,
               L"%ls%ls%ls\n\nSome HookModules might not play nice with them.\n"
               L"(Turn them off for this program if you have issues.)",
               self_line,
               (self_line[0] && target_line[0]) ? L"\n" : L"",
               target_line);
    msg[1023] = L'\0';

    _snwprintf(details, PATHBUF * 3,
               L"__COMPAT_LAYER = %ls\nLayers (launcher) = %ls\n"
               L"Layers (real executable) = %ls",
               env_raw[0] ? env_raw : L"(not set)",
               self_raw[0] ? self_raw : L"(none)",
               target_raw[0] ? target_raw : L"(none)");
    details[(PATHBUF * 3) - 1] = L'\0';

    ZeroMemory(&buttons, sizeof(buttons));
    buttons[0].nButtonID = HKS_BTN_CONTINUE;
    buttons[0].pszButtonText = L"Continue";
    buttons[1].nButtonID = HKS_BTN_NOWARN;
    buttons[1].pszButtonText = L"Don't warn for this executable again";
    buttons[2].nButtonID = HKS_BTN_CANCEL;
    buttons[2].pszButtonText = L"Cancel launch";

    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.pszWindowTitle = HKS_LAUNCHER_EXE_TITLE;
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszMainInstruction = L"Compatibility shims are active for this launch";
    cfg.pszContent = msg;
    cfg.pszExpandedInformation = details;
    cfg.pszExpandedControlText = L"Details";
    cfg.pszCollapsedControlText = L"Hide details";
    cfg.cButtons = 3;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = HKS_BTN_CONTINUE;

    hr = TaskDialogIndirect(&cfg, &pressed, NULL, NULL);

    /* A static import means Common Controls v6 exists whenever this process
       runs, so a failure is the call itself being refused, and an unexpected
       button is a dialog torn down without an answer: ask again plainly. */
    if (FAILED(hr) ||
        (pressed != HKS_BTN_CONTINUE && pressed != HKS_BTN_NOWARN &&
         pressed != HKS_BTN_CANCEL)) {
        /* Nothing of ours owns the dialog, so borrow the focused window: an owned
           dialog stays above the window the user launched from. */
        HWND owner = GetForegroundWindow();

        _snwprintf(fallback, 1152, L"%ls\n\n%ls", cfg.pszMainInstruction, msg);
        fallback[1151] = L'\0';

        return (IDCANCEL == MessageBoxW(owner, fallback, HKS_LAUNCHER_EXE_TITLE,
                                        MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND))
                   ? HKS_WARN_CANCEL
                   : HKS_WARN_PROCEED;
    }

    switch (pressed) {
    case HKS_BTN_NOWARN:
        /* Best effort: if the marker cannot be written, the prompt simply comes
           back on the next launch rather than blocking this one. */
        ensure_marker(nowarn);
        return HKS_WARN_PROCEED;
    case HKS_BTN_CANCEL:
        return HKS_WARN_CANCEL;
    default:
        return HKS_WARN_PROCEED;
    }
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
       variable for the worker spawn, then restore it. The target process is
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
                   L"Hookshot failed to inject the target.\n\n"
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

/* Appends one already-parsed argument to a command line being rebuilt, wrapped
   in quotes with any embedded quote characters escaped. Matches how the
   official Hookshot launcher re-emits each argument. */
static void append_quoted_argument(wchar_t *out, size_t outsz, size_t *len, const wchar_t *text)
{
    const wchar_t *p;

    if (*len + 1 < outsz) out[(*len)++] = L'"';

    for (p = text; *p != L'\0'; ++p) {
        if (L'"' == *p && *len + 1 < outsz) out[(*len)++] = L'\\';
        if (*len + 1 < outsz) out[(*len)++] = *p;
    }

    if (*len + 1 < outsz) out[(*len)++] = L'"';
    out[*len] = L'\0';
}

/* Builds the target's command line the way the official Hookshot launcher does:
   the real executable path first, then each of our own arguments, every element
   quoted with embedded quotes escaped. This keeps the raw command line (which
   some programs parse themselves) identical in shape to the stock launcher's. */
static void build_target_command_line(const wchar_t *target, wchar_t *out, size_t outsz)
{
    int argc = 0;
    wchar_t **argv;
    size_t len = 0;
    int i;

    if (out == NULL || outsz == 0) return;
    out[0] = L'\0';

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    append_quoted_argument(out, outsz, &len, target);

    if (argv != NULL) {
        /* argv[0] is this launcher's own path; forward everything after it. */
        for (i = 1; i < argc; ++i) {
            if (len + 1 < outsz) out[len++] = L' ';
            out[len] = L'\0';
            append_quoted_argument(out, outsz, &len, argv[i]);
        }
        LocalFree(argv);
    }
}

static int do_launcher(const wchar_t *self, const wchar_t *args)
{
    wchar_t selfname[PATHBUF];
    wchar_t dir[PATHBUF];
    wchar_t prefixed[PATHBUF];
    wchar_t target[PATHBUF];
    wchar_t worker[PATHBUF];
    wchar_t marker[PATHBUF];
    wchar_t dirwide_marker[PATHBUF];
    wchar_t cmdline[PATHBUF * 2];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 0;
    DWORD err;
    HKS_WARN_RESULT warn_result;
    int created_marker = 0;
    int ok;

    basename_of(self, selfname, PATHBUF);
    dirname_of(self, dir, PATHBUF);
    apply_prefix(selfname, prefixed, PATHBUF);
    _snwprintf(target, PATHBUF, L"%ls\\%ls", dir, prefixed);
    target[PATHBUF - 1] = L'\0';

    path_with_suffix(target, AUTH_SUFFIX, marker, PATHBUF);

    _snwprintf(dirwide_marker, PATHBUF, L"%ls\\%ls", dir, AUTH_SUFFIX);
    dirwide_marker[PATHBUF - 1] = L'\0';

    build_target_command_line(target, cmdline, PATHBUF * 2);

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

    /* Let the target inherit this launcher's working directory, as the official
       Hookshot launcher does, instead of forcing it to the executable's
       directory. */
    if (!CreateProcessW(target, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        err = GetLastError();
        if (err == ERROR_ELEVATION_REQUIRED)
            return relaunch_elevated(self, args);

        show_err_code2(L"create target process", target, err);
        return 1;
    }

    /* Hookshot accepts either an application-specific <exe>.hookshot or a
       directory-wide .hookshot as authorization. Only create the former when
       neither already exists, so an already-authorized directory is not
       written to (and never needs elevation just to authorize). */
    if (!file_exists_w(marker) && !file_exists_w(dirwide_marker)) {
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
        created_marker = 1;
    }

    /* Prompt only once the launch is actually going to proceed. Placing this
       here rather than at startup means an elevation re-launch (which supersedes
       the instance that requested it) does not re-trigger the prompt: only the
       instance that is about to inject the target reaches this point. */
    warn_result = warn_if_compat_layer_present(target, self);
    if (warn_result == HKS_WARN_CANCEL) {
        /* A deliberate choice rather than a failure, so this is quiet and
           reports success. The authorization marker we created goes with it. */
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (created_marker) DeleteFileW(marker);
        return 0;
    }

    ok = run_worker(pi.hProcess, pi.hThread, worker);

    if (!ok) {
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (created_marker) DeleteFileW(marker);
        return 1;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    if (created_marker) DeleteFileW(marker);
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