#include "hks-common.h"
#include <stdio.h>

#define MAX_PE_OFFSET 1048576

int file_exists_w(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

void dirname_of(const wchar_t *path, wchar_t *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    lstrcpynW(out, path, (int)outsz);
    wchar_t *p = wcsrchr(out, L'\\');
    if (p) *p = 0;
}

void basename_of(const wchar_t *path, wchar_t *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    const wchar_t *p = wcsrchr(path, L'\\');
    lstrcpynW(out, p ? p + 1 : path, (int)outsz);
}

void apply_suffix(const wchar_t *basename, wchar_t *out, size_t outsz)
{
    wchar_t stem[PATHBUF];
    const wchar_t *ext = L"";
    const wchar_t *dot = wcsrchr(basename, L'.');

    if (!out || outsz == 0) return;

    if (dot) {
        size_t n = (size_t)(dot - basename);
        if (n >= PATHBUF) n = PATHBUF - 1;
        wcsncpy(stem, basename, n);
        stem[n] = 0;
        ext = dot;
    } else {
        lstrcpynW(stem, basename, PATHBUF);
    }

    _snwprintf(out, outsz, L"%ls%ls%ls", stem, TARGET_SUFFIX, ext);
    out[outsz - 1] = L'\0';
}

int has_target_suffix(const wchar_t *basename)
{
    size_t stem_len;
    size_t suffix_len = wcslen(TARGET_SUFFIX);
    const wchar_t *dot = wcsrchr(basename, L'.');

    if (dot)
        stem_len = (size_t)(dot - basename);
    else
        stem_len = wcslen(basename);

    if (stem_len < suffix_len) return 0;

    return wcsncmp(basename + stem_len - suffix_len,
                   TARGET_SUFFIX, suffix_len) == 0;
}

int get_pe_machine(const wchar_t *path, wchar_t *out, size_t outsz)
{
    HANDLE h;
    unsigned char buf[64];
    DWORD got = 0;
    DWORD peOffset;
    LARGE_INTEGER li;
    unsigned char hdr[6];
    unsigned int machine;

    if (!out || outsz == 0) return -1;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lstrcpynW(out, L"invalid (cannot open)", (int)outsz);
        return -1;
    }

    if (!ReadFile(h, buf, 64, &got, NULL) || got < 64) {
        CloseHandle(h);
        lstrcpynW(out, L"invalid (too small)", (int)outsz);
        return -1;
    }
    if (buf[0] != 'M' || buf[1] != 'Z') {
        CloseHandle(h);
        lstrcpynW(out, L"invalid (no MZ)", (int)outsz);
        return -1;
    }

    peOffset = (DWORD)buf[0x3C]
             | ((DWORD)buf[0x3D] << 8)
             | ((DWORD)buf[0x3E] << 16)
             | ((DWORD)buf[0x3F] << 24);

    if (peOffset < 64 || peOffset > MAX_PE_OFFSET) {
        CloseHandle(h);
        _snwprintf(out, outsz, L"invalid (peOffset=%lu)", peOffset);
        out[outsz - 1] = L'\0';
        return -1;
    }

    li.QuadPart = peOffset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        lstrcpynW(out, L"invalid (seek)", (int)outsz);
        return -1;
    }
    if (!ReadFile(h, hdr, 6, &got, NULL) || got < 6) {
        CloseHandle(h);
        lstrcpynW(out, L"invalid (short read)", (int)outsz);
        return -1;
    }
    CloseHandle(h);

    if (hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0) {
        lstrcpynW(out, L"invalid (no PE signature)", (int)outsz);
        return -1;
    }

    machine = (unsigned int)hdr[4] | ((unsigned int)hdr[5] << 8);

    switch (machine) {
        case 0x014C: lstrcpynW(out, L"x86",   (int)outsz); return 0;
        case 0x8664: lstrcpynW(out, L"x64",   (int)outsz); return 0;
        case 0xAA64: lstrcpynW(out, L"arm64", (int)outsz); return 0;
        case 0x01C4: lstrcpynW(out, L"arm",   (int)outsz); return 0;
        default:
            _snwprintf(out, outsz, L"unknown (0x%04X)", machine);
            out[outsz - 1] = L'\0';
            return -1;
    }
}