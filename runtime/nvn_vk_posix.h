/* nvn_vk_posix.h - the file-system Win32 names that nvn_vk_draw.c / nvn_vk_m4.c / nvn_vk_store.c use, for the Linux host
 * (included at the top of nvn_vk_draw.c, under #ifndef _WIN32). Paths that begin with "../" or "..\" are relative to the
 * folder of the executable (the Windows layout ..\cache, ..\romfs, ..\binaries), not to the working directory; '\' -> '/'. */
#ifndef NVN_VK_POSIX_H
#define NVN_VK_POSIX_H
#ifndef _WIN32
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

#if defined(__has_include)
#if __has_include("hostpath.h")
#include "hostpath.h"
#define VKP_HAVE_HOSTPATH 1
#endif
#endif
#include "vmem.h"

static const char *vkp_exe_dir(void) {
    static char dir[512];
    static int done;
    if (!done) {
#ifdef VKP_HAVE_HOSTPATH
        snprintf(dir, sizeof dir, "%s", host_exe_dir());
#else
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 2);
        if (n < 0) n = 0;
        dir[n] = 0;
        char *sl = strrchr(dir, '/');
        if (sl) sl[1] = 0; else dir[0] = 0;
#endif
        done = 1;
    }
    return dir;
}
/* `p` -> buf (thread-safe by the caller's buffer) */
static const char *vkp_path(const char *p, char *buf, size_t n) {
    size_t k = 0, i = 0;
    if (!p) return p;
    if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\')) { snprintf(buf, n, "%s", vkp_exe_dir()); k = strlen(buf); }
    else if (!strchr(p, '\\')) return p;
    for (; p[i] && k + 1 < n; i++) buf[k++] = p[i] == '\\' ? '/' : p[i];
    buf[k] = 0;
    return buf;
}
static inline FILE *vkp_fopen(const char *p, const char *m) { char b[512]; return fopen(vkp_path(p, b, sizeof b), m); }
static inline void vkp_into(char *buf, size_t n, const char *p) { const char *r = vkp_path(p, buf, n); if (r != buf) snprintf(buf, n, "%s", r); }
#define fopen(p, m) vkp_fopen(p, m)

/* CreateDirectoryA: mkdir -p (TRUE also when it exists is NOT Win32, but no caller looks at the result) */
static inline BOOL vkp_CreateDirectoryA(const char *p, void *sa) {
    char b[512]; (void)sa;
    vkp_into(b, sizeof b, p);
    for (char *q = b + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(b, 0755); *q = '/'; }
    return mkdir(b, 0755) == 0;
}
#define CreateDirectoryA vkp_CreateDirectoryA
static inline BOOL vkp_DeleteFileA(const char *p) { char b[512]; return unlink(vkp_path(p, b, sizeof b)) == 0; }
#define DeleteFileA vkp_DeleteFileA
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
static inline DWORD vkp_GetFileAttributesA(const char *p) { char b[512]; struct stat st; return stat(vkp_path(p, b, sizeof b), &st) == 0 ? (S_ISDIR(st.st_mode) ? 0x10u : 0x80u) : INVALID_FILE_ATTRIBUTES; }
#define GetFileAttributesA vkp_GetFileAttributesA
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
#define GetFileExInfoStandard 0
static inline BOOL vkp_GetFileAttributesExA(const char *p, int lvl, WIN32_FILE_ATTRIBUTE_DATA *d) {
    char b[512]; struct stat st; (void)lvl;
    if (stat(vkp_path(p, b, sizeof b), &st) != 0) return FALSE;
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); d->nFileSizeLow = (DWORD)st.st_size;
    uint64_t t = ((uint64_t)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100;      /* FILETIME */
    d->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32); d->ftLastWriteTime.dwLowDateTime = (DWORD)t;
    return TRUE;
}
#define GetFileAttributesExA vkp_GetFileAttributesExA
/* MoveFileExA(REPLACE_EXISTING) = rename(2) (atomic on one file system), after fsync of the new file and then of its folder:
 * a crash leaves the old cache or the new one, never a torn file */
#define MOVEFILE_REPLACE_EXISTING 1
static inline BOOL vkp_MoveFileExA(const char *from, const char *to, DWORD fl) {
    char a[512], b[512]; int fd;
    (void)fl;
    vkp_into(a, sizeof a, from); vkp_into(b, sizeof b, to);
    if ((fd = open(a, O_RDONLY)) >= 0) { fsync(fd); close(fd); }
    if (rename(a, b) != 0) return FALSE;
    {   char *sl = strrchr(b, '/');
        if (sl) { *sl = 0; fd = open(sl == b ? "/" : b, O_RDONLY | O_DIRECTORY); if (fd >= 0) { fsync(fd); close(fd); } }
    }
    return TRUE;
}
#define MoveFileExA vkp_MoveFileExA
/* FindFirstFileA("<dir>/ * <suffix>", no spaces): names ending in <suffix> */
typedef struct { DWORD dwFileAttributes; DWORD nFileSizeHigh, nFileSizeLow; char cFileName[260]; } WIN32_FIND_DATAA;
typedef struct { DIR *d; char dir[512]; char suffix[256]; } VKP_FIND;
static inline BOOL vkp_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    VKP_FIND *f = (VKP_FIND *)h;
    struct dirent *e;
    while ((e = readdir(f->d)) != NULL) {
        size_t n = strlen(e->d_name), s = strlen(f->suffix);
        struct stat st;
        char full[1100];
        if (n < s || memcmp(e->d_name + n - s, f->suffix, s) != 0 || n >= sizeof fd->cFileName) continue;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        memset(fd, 0, sizeof *fd);
        if (stat(full, &st) == 0) { fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size; fd->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10u : 0x80u; }
        memcpy(fd->cFileName, e->d_name, n + 1);
        return TRUE;
    }
    return FALSE;
}
static inline HANDLE vkp_FindFirstFileA(const char *pat, WIN32_FIND_DATAA *fd) {
    char b[512], *sl, *star;
    VKP_FIND *f = (VKP_FIND *)calloc(1, sizeof *f);
    if (!f) return INVALID_HANDLE_VALUE;
    vkp_into(b, sizeof b, pat);
    sl = strrchr(b, '/');
    star = strchr(sl ? sl : b, '*');
    if (sl) { *sl = 0; snprintf(f->dir, sizeof f->dir, "%s", sl == b ? "/" : b); *sl = '/'; } else snprintf(f->dir, sizeof f->dir, ".");
    snprintf(f->suffix, sizeof f->suffix, "%s", star ? star + 1 : (sl ? sl + 1 : b));
    f->d = opendir(f->dir);
    if (!f->d || !vkp_FindNextFileA((HANDLE)f, fd)) { if (f->d) closedir(f->d); free(f); return INVALID_HANDLE_VALUE; }
    return (HANDLE)f;
}
static inline BOOL vkp_FindClose(HANDLE h) { VKP_FIND *f = (VKP_FIND *)h; if (f && f->d) closedir(f->d); free(f); return TRUE; }
#define FindFirstFileA vkp_FindFirstFileA
#define FindNextFileA vkp_FindNextFileA
#define FindClose vkp_FindClose
/* ReadProcessMemory on ourselves: a checked copy */
static inline BOOL vkp_ReadProcessMemory(HANDLE h, const void *src, void *dst, SIZE_T n, SIZE_T *got) {
    (void)h;
    if (got) *got = 0;
    if (!vmem_readable(src, n)) return FALSE;
    memcpy(dst, src, n);
    if (got) *got = n;
    return TRUE;
}
#define ReadProcessMemory vkp_ReadProcessMemory
/* the whole file as one read-only private mapping, placed below 2^47 (the guest's zstd decoder reads from it) */
static const uint8_t *vkp_map_file(int fd, uint64_t len) {
    static const uint64_t hints[] = { 0x300000000000ull, 0x200000000000ull, 0x100000000000ull, 0x080000000000ull, 0 };
    for (int i = 0; i < 5; i++) {
        void *p = mmap((void *)(uintptr_t)hints[i], (size_t)len, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
        if (p == MAP_FAILED) continue;
        if ((uint64_t)(uintptr_t)p + len <= (1ull << 47)) { madvise(p, (size_t)len, MADV_SEQUENTIAL); return (const uint8_t *)p; }
        munmap(p, (size_t)len);
    }
    return NULL;
}
#endif
#endif
