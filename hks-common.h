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
#define TARGET_PREFIX  L"_hks_"

/* Markers that sit beside the real (prefixed) executable. The authorization
   file is Hookshot's and only exists while the target runs; the no-warn file is
   the user's opt-out of the compatibility-mode prompt and is deliberately
   persistent. */
#define AUTH_SUFFIX    L".hookshot"
#define NOWARN_SUFFIX  L".nowarn"

int  file_exists_w(const wchar_t *path);
void dirname_of(const wchar_t *path, wchar_t *out, size_t outsz);
void basename_of(const wchar_t *path, wchar_t *out, size_t outsz);
void apply_prefix(const wchar_t *basename, wchar_t *out, size_t outsz);
int  has_target_prefix(const wchar_t *basename);
void path_with_suffix(const wchar_t *path, const wchar_t *suffix, wchar_t *out, size_t outsz);
int  get_pe_machine(const wchar_t *path, wchar_t *out, size_t outsz);

#endif /* HKS_COMMON_H */