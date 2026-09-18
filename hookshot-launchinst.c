#include "hks-common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTALLER_NAME   L"hookshot-launchinst.exe"

#define IDR_LAUNCHER_32 101
#define IDR_LAUNCHER_64 102
#define RES_TYPE_EXE    256

/* Forward declarations for functions defined later but used above. */
static int validate_and_prepare_target(wchar_t **argv,
                                      wchar_t *target, size_t target_sz,
                                      wchar_t *dir, size_t dir_sz,
                                      wchar_t *base, size_t base_sz,
                                      wchar_t *suffixed, size_t suffixed_sz,
                                      wchar_t *hs_target, size_t hs_target_sz,
                                      wchar_t *machine, size_t machine_sz,
                                      int *resource_id);

static void finalize_operation(const wchar_t *target);

static int get_architecture_and_resource_id(const wchar_t *target,
                                           wchar_t *machine, size_t machine_sz,
                                           int *resource_id);

static int basename_is_ours(const wchar_t *base)
{
    return _wcsicmp(base, INSTALLER_NAME) == 0;
}

static void report_err_msg(const wchar_t *msg)
{
    fwprintf(stderr, L"%ls\n", msg);
}

static void report_err_code(const wchar_t *action, DWORD err)
{
    fwprintf(stderr, L"Failed to %ls (error %lu)\n", action, err);
}

static void report_move_failure(const wchar_t *action, DWORD err)
{
    report_err_code(action, err);
    if (err == ERROR_ACCESS_DENIED)
        fwprintf(stderr, L"  Access denied - is the game running? Try running as administrator\n");
}

static void print_inherited_resources_failed(void)
{
    wprintf(L"  Failed to write inherited resources\n");
}

static void report_already_installed(void)
{
    report_err_msg(L"Target already has Hookshot installed");
}

static void report_invalid_target_path(void)
{
    report_err_msg(L"Invalid target path");
}

static void report_target_not_found(void)
{
    report_err_msg(L"Target not found");
}

static int validate_target_base(const wchar_t *base)
{
    if (basename_is_ours(base)) {
        report_err_msg(L"You targeted me!\nPass another executable instead");
        return 1;
    }
    if (has_target_suffix(base)) {
        report_already_installed();
        return 1;
    }
    return 0;
}

typedef struct {
    HMODULE hSrc;
    HANDLE hUpdate;
    int copied;
} COPY_CTX;

static BOOL CALLBACK LangProc(HMODULE hMod, LPCWSTR type, LPCWSTR name,
                              WORD lang, LONG_PTR param)
{
    COPY_CTX *ctx = (COPY_CTX *)param;
    HRSRC     hRes;
    DWORD     size;
    HGLOBAL   hData;
    void     *data;

    (void)hMod;

    hRes = FindResourceExW(ctx->hSrc, type, name, lang);
    if (!hRes) return TRUE;

    size = SizeofResource(ctx->hSrc, hRes);
    if (size == 0) return TRUE;

    hData = LoadResource(ctx->hSrc, hRes);
    if (!hData) return TRUE;

    data = LockResource(hData);
    if (!data) return TRUE;

    if (UpdateResourceW(ctx->hUpdate, type, name, lang, data, size))
        ctx->copied++;

    return TRUE;
}

static BOOL CALLBACK NameProc(HMODULE hMod, LPCWSTR type, LPWSTR name,
                              LONG_PTR param)
{
    (void)hMod;
    EnumResourceLanguagesW(((COPY_CTX *)param)->hSrc,
                           type, name, LangProc, param);
    return TRUE;
}

static int copy_type(HMODULE hSrc, HANDLE hUpdate, LPCWSTR type)
{
    COPY_CTX ctx;
    ctx.hSrc    = hSrc;
    ctx.hUpdate = hUpdate;
    ctx.copied  = 0;

    EnumResourceNamesW(hSrc, type, NameProc, (LONG_PTR)&ctx);
    return ctx.copied;
}

typedef struct {
    WORD idReserved;
    WORD idType;
    WORD idCount;
} __attribute__((packed)) GRPICONDIR;

typedef struct {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    WORD nID;
} __attribute__((packed)) GRPICONDIRENTRY;

typedef struct {
    HMODULE hSrc;
    HANDLE  hUpdate;
    LPWSTR  best_group_name;
    WORD    best_lang;
    BOOL    has_group;
} FIND_BEST_GROUP_CTX;

static BOOL CALLBACK FindBestGroupLangProc(HMODULE hMod, LPCWSTR type, LPCWSTR name,
                                           WORD lang, LONG_PTR param)
{
    FIND_BEST_GROUP_CTX *ctx = (FIND_BEST_GROUP_CTX *)param;
    (void)hMod; (void)type; (void)name;

    if (!ctx->has_group) {
        ctx->best_lang = lang;
        ctx->has_group = TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK FindBestGroupNameProc(HMODULE hMod, LPCWSTR type, LPWSTR name,
                                            LONG_PTR param)
{
    FIND_BEST_GROUP_CTX *ctx = (FIND_BEST_GROUP_CTX *)param;

    if (!ctx->best_group_name) {
        if (IS_INTRESOURCE(name)) {
            ctx->best_group_name = name;
        } else {
            ctx->best_group_name = wcsdup(name);
        }
        EnumResourceLanguagesW(hMod, type, name, FindBestGroupLangProc, param);
    } else if (IS_INTRESOURCE(ctx->best_group_name) && IS_INTRESOURCE(name)) {
        if ((ULONG_PTR)name < (ULONG_PTR)ctx->best_group_name) {
            ctx->best_group_name = name;
            EnumResourceLanguagesW(hMod, type, name, FindBestGroupLangProc, param);
        }
    }
    return TRUE;
}

static int copy_primary_icon_group(HMODULE hSrc, HANDLE hUpdate)
{
    FIND_BEST_GROUP_CTX ctx = { hSrc, hUpdate, NULL, 0, FALSE };
    HRSRC hRes;
    HGLOBAL hData;
    GRPICONDIR *dir;
    GRPICONDIRENTRY *entries;
    int copied = 0;

    EnumResourceNamesW(hSrc, RT_GROUP_ICON, FindBestGroupNameProc, (LONG_PTR)&ctx);

    if (!ctx.has_group || !ctx.best_group_name)
        return 0;

    hRes = FindResourceExW(hSrc, RT_GROUP_ICON, ctx.best_group_name, ctx.best_lang);
    if (!hRes) goto cleanup;

    hData = LoadResource(hSrc, hRes);
    if (!hData) goto cleanup;

    dir = (GRPICONDIR *)LockResource(hData);
    if (!dir || dir->idType != 1) goto cleanup;

    if (UpdateResourceW(hUpdate, RT_GROUP_ICON, ctx.best_group_name, ctx.best_lang,
                        dir, SizeofResource(hSrc, hRes))) {
        copied++;
    }

    entries = (GRPICONDIRENTRY *)(dir + 1);

    for (WORD i = 0; i < dir->idCount; i++) {
        WORD icon_id = entries[i].nID;
        HRSRC hIconRes = FindResourceExW(hSrc, RT_ICON, MAKEINTRESOURCEW(icon_id), ctx.best_lang);
        if (!hIconRes)
            hIconRes = FindResourceW(hSrc, MAKEINTRESOURCEW(icon_id), RT_ICON);

        if (hIconRes) {
            HGLOBAL hIconData = LoadResource(hSrc, hIconRes);
            void *raw_bytes = LockResource(hIconData);
            DWORD size = SizeofResource(hSrc, hIconRes);

            if (raw_bytes && size > 0) {
                if (UpdateResourceW(hUpdate, RT_ICON, MAKEINTRESOURCEW(icon_id),
                                    ctx.best_lang, raw_bytes, size)) {
                    copied++;
                }
            }
        }
    }

cleanup:
    if (ctx.best_group_name && !IS_INTRESOURCE(ctx.best_group_name)) {
        free(ctx.best_group_name);
    }
    return copied;
}

static void copy_resources(const wchar_t *src_path, const wchar_t *dst_path)
{
    HMODULE hSrc;
    HANDLE  hUpdate;
    int icon_count = 0;
    int version_count = 0;

    hSrc = LoadLibraryExW(src_path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hSrc) {
        wprintf(L"  Failed to read resources from original executable\n");
        return;
    }

    hUpdate = BeginUpdateResourceW(dst_path, FALSE);
    if (!hUpdate) {
        FreeLibrary(hSrc);
        print_inherited_resources_failed();
        return;
    }

    icon_count += copy_primary_icon_group(hSrc, hUpdate);
    version_count += copy_type(hSrc, hUpdate, RT_VERSION);

    if (!EndUpdateResourceW(hUpdate, FALSE)) {
        FreeLibrary(hSrc);
        print_inherited_resources_failed();
        return;
    }
    FreeLibrary(hSrc);

    if (icon_count > 0 && version_count > 0) {
        wprintf(L"  Icon and version resources inherited from original executable\n");
    } else if (icon_count > 0) {
        wprintf(L"  Icon resources inherited from original executable\n");
    } else if (version_count > 0) {
        wprintf(L"  Version resources inherited from original executable\n");
    } else {
        wprintf(L"  No icon or version resources found to inherit\n");
    }
}

static void check_hookshot_dir(void)
{
    wchar_t buf[PATHBUF];
    DWORD n = GetEnvironmentVariableW(L"HookshotDir", buf, PATHBUF);

    if (n == 0 || n >= PATHBUF) {
        wprintf(L"  Warning: HookshotDir environment variable not set\n"
                L"  Launcher will ask you to set HookshotDir on first run\n");
    }
}

static void print_usage(const wchar_t *exe_name)
{
    wchar_t basename[PATHBUF];
    wchar_t stem[PATHBUF];
    const wchar_t *dot;
    size_t n;

    basename_of(exe_name, basename, PATHBUF);

    dot = wcsrchr(basename, L'.');
    if (dot) {
        n = (size_t)(dot - basename);
        if (n >= PATHBUF) n = PATHBUF - 1;
        wcsncpy(stem, basename, n);
        stem[n] = 0;
    } else {
        lstrcpynW(stem, basename, PATHBUF);
    }

    wprintf(L"Hookshot Launcher/Installer\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %ls [install | uninstall | update] <path-to-exe>\n\n", stem);
    wprintf(L"Commands:\n");
    wprintf(L"  install     Replaces target executable with a Hookshot-compatible entrypoint\n");
    wprintf(L"  uninstall   Restores original game executable\n");
    wprintf(L"  update      Bring an existing launchinst installation up to date with the current embedded launcher\n\n");
    wprintf(L"Note: All operations target the original executable NAME, not the suffixed _hks_!\n");
}

static int extract_launcher_resource(int resource_id, const wchar_t *dest_path)
{
    HMODULE hSelf = GetModuleHandleW(NULL);
    HRSRC hRes;
    HGLOBAL hMem;
    const void *data;
    DWORD size;
    HANDLE hFile;
    DWORD written = 0;

    hRes = FindResourceW(hSelf, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(RES_TYPE_EXE));
    if (!hRes) return 0;

    hMem = LoadResource(hSelf, hRes);
    if (!hMem) return 0;

    data = LockResource(hMem);
    if (!data) return 0;

    size = SizeofResource(hSelf, hRes);
    if (size == 0) return 0;

    hFile = CreateFileW(dest_path, GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    if (!WriteFile(hFile, data, size, &written, NULL) || written != size) {
        CloseHandle(hFile);
        return 0;
    }

    CloseHandle(hFile);
    return 1;
}

static int do_install(wchar_t **argv)
{
    wchar_t target[PATHBUF];
    wchar_t dir[PATHBUF];
    wchar_t base[PATHBUF];
    wchar_t suffixed[PATHBUF];
    wchar_t hs_target[PATHBUF];
    wchar_t machine[64];
    int resource_id = 0;

    if (validate_and_prepare_target(argv, target, PATHBUF, dir, PATHBUF, base, PATHBUF, 
                                   suffixed, PATHBUF, hs_target, PATHBUF, machine, 64, &resource_id))
        return 1;

    if (file_exists_w(hs_target)) {
        report_already_installed();
        return 1;
    }

    wprintf(L"Installing launcher\n");
    wprintf(L"  %ls architecture detected\n", machine);

    if (!MoveFileW(target, hs_target)) {
        DWORD err = GetLastError();
        report_move_failure(L"rename target executable", err);
        return 1;
    }

    if (!extract_launcher_resource(resource_id, target)) {
        DWORD err = GetLastError();
        fwprintf(stderr, L"Failed to extract launcher (error %lu)\nRolling back rename\n", err);
        MoveFileW(hs_target, target);
        return 1;
    }
    wprintf(L"  Launcher executable in place\n");

    copy_resources(hs_target, target);

    finalize_operation(target);
    return 0;
}

static int do_update(wchar_t **argv)
{
    wchar_t target[PATHBUF];
    wchar_t dir[PATHBUF];
    wchar_t base[PATHBUF];
    wchar_t suffixed[PATHBUF];
    wchar_t hs_target[PATHBUF];
    wchar_t machine[64];
    int resource_id = 0;

    if (validate_and_prepare_target(argv, target, PATHBUF, dir, PATHBUF, base, PATHBUF, 
                                   suffixed, PATHBUF, hs_target, PATHBUF, machine, 64, &resource_id))
        return 1;

    if (!file_exists_w(hs_target)) {
        wprintf(L"No Hookshot installation found\n");
        return 0;
    }

    wprintf(L"Updating launcher\n");
    wprintf(L"  %ls architecture detected\n", machine);

    if (!extract_launcher_resource(resource_id, target)) {
        DWORD err = GetLastError();
        fwprintf(stderr, L"Failed to extract launcher (error %lu)\n", err);
        return 1;
    }
    wprintf(L"  Launcher executable in place\n");

    copy_resources(hs_target, target);

    finalize_operation(target);
    return 0;
}

static void report_ini_leftover(const wchar_t *dir)
{
    wchar_t ini_path[PATHBUF];
    _snwprintf(ini_path, PATHBUF, L"%ls\\Hookshot.ini", dir);
    ini_path[PATHBUF - 1] = L'\0';
    if (file_exists_w(ini_path)) {
        wprintf(L"  Hookshot.ini left over\n");
    }
}

static void report_hookmodule_dlls_leftover(const wchar_t *dir)
{
    wchar_t pattern[PATHBUF];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    _snwprintf(pattern, PATHBUF, L"%ls\\*.HookModule.*.dll", dir);
    pattern[PATHBUF - 1] = L'\0';

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    FindClose(h);
    wprintf(L"  HookModules left over\n");
}

static int do_uninstall(wchar_t **argv)
{
    wchar_t target[PATHBUF];
    wchar_t dir[PATHBUF];
    wchar_t base[PATHBUF];
    wchar_t suffixed[PATHBUF];
    wchar_t hs_target[PATHBUF];
    wchar_t marker[PATHBUF];
    DWORD len;

    len = GetFullPathNameW(argv[2], PATHBUF, target, NULL);
    if (len == 0 || len >= PATHBUF) {
        report_invalid_target_path();
        return 1;
    }

    if (!file_exists_w(target)) {
        report_target_not_found();
        return 1;
    }

    dirname_of(target, dir, PATHBUF);
    basename_of(target, base, PATHBUF);
    apply_suffix(base, suffixed, PATHBUF);
    _snwprintf(hs_target, PATHBUF, L"%ls\\%ls", dir, suffixed);
    hs_target[PATHBUF - 1] = L'\0';

    if (!file_exists_w(hs_target)) {
        wprintf(L"No Hookshot installation found\n");
        return 0;
    }

    wprintf(L"Uninstalling launcher\n");

    if (!MoveFileExW(hs_target, target, MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        report_move_failure(L"restore original exe", err);
        return 1;
    }
    wprintf(L"  Original executable restored\n");

    _snwprintf(marker, PATHBUF, L"%ls.hookshot", hs_target);
    marker[PATHBUF - 1] = L'\0';
    if (file_exists_w(marker)) {
        if (DeleteFileW(marker)) {
            wprintf(L"  Hookshot authorization file deleted\n");
        }
    }

    report_ini_leftover(dir);
    report_hookmodule_dlls_leftover(dir);

    finalize_operation(target);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (_wcsicmp(argv[1], L"install") == 0) {
        if (argc < 3) {
            report_err_msg(L"Install requires a target path\n");
            print_usage(argv[0]);
            return 1;
        }
        return do_install(argv);
    }

    if (_wcsicmp(argv[1], L"uninstall") == 0) {
        if (argc < 3) {
            report_err_msg(L"Uninstall requires a target path\n");
            print_usage(argv[0]);
            return 1;
        }
        return do_uninstall(argv);
    }

    if (_wcsicmp(argv[1], L"update") == 0) {
        if (argc < 3) {
            report_err_msg(L"Update requires a target path\n");
            print_usage(argv[0]);
            return 1;
        }
        return do_update(argv);
    }

    report_err_msg(L"Unknown verb\n");
    print_usage(argv[0]);
    return 1;
}

static int validate_and_prepare_target(wchar_t **argv, 
                                      wchar_t *target, size_t target_sz,
                                      wchar_t *dir, size_t dir_sz,
                                      wchar_t *base, size_t base_sz,
                                      wchar_t *suffixed, size_t suffixed_sz,
                                      wchar_t *hs_target, size_t hs_target_sz,
                                      wchar_t *machine, size_t machine_sz,
                                      int *resource_id)
{
    DWORD len = GetFullPathNameW(argv[2], target_sz, target, NULL);
    if (len == 0 || len >= target_sz) {
        report_invalid_target_path();
        return 1;
    }

    if (!file_exists_w(target)) {
        report_target_not_found();
        return 1;
    }

    dirname_of(target, dir, dir_sz);
    basename_of(target, base, base_sz);

    if (validate_target_base(base))
        return 1;

    apply_suffix(base, suffixed, suffixed_sz);
    _snwprintf(hs_target, hs_target_sz, L"%ls\\%ls", dir, suffixed);
    hs_target[hs_target_sz - 1] = L'\0';

    if (get_architecture_and_resource_id(target, machine, machine_sz, resource_id))
        return 1;

    return 0;
}

static void finalize_operation(const wchar_t *target)
{
    check_hookshot_dir();
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, target, NULL);
}

static int get_architecture_and_resource_id(const wchar_t *target, 
                                           wchar_t *machine, size_t machine_sz,
                                           int *resource_id)
{
    if (get_pe_machine(target, machine, machine_sz) != 0) {
        report_err_msg(L"Cannot determine architecture");
        return 1;
    }

    if (wcscmp(machine, L"x86") == 0) {
        *resource_id = IDR_LAUNCHER_32;
    } else if (wcscmp(machine, L"x64") == 0) {
#ifdef HKS_INSTALLER_32ONLY
        report_err_msg(L"  Unsupported architecture (64 on 32)");
        return 1;
#else
        *resource_id = IDR_LAUNCHER_64;
#endif
    } else {
        report_err_msg(L"Unsupported architecture");
        return 1;
    }

    return 0;
}