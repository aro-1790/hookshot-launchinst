#ifndef HKS_COMMON_H
#define HKS_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wchar.h>

#define PATHBUF        2048
#define TARGET_SUFFIX  L"_hks_"

int  file_exists_w(const wchar_t *path);
void dirname_of(const wchar_t *path, wchar_t *out, size_t outsz);
void basename_of(const wchar_t *path, wchar_t *out, size_t outsz);
void apply_suffix(const wchar_t *basename, wchar_t *out, size_t outsz);
int  has_target_suffix(const wchar_t *basename);
int  get_pe_machine(const wchar_t *path, wchar_t *out, size_t outsz);

#endif /* HKS_COMMON_H */