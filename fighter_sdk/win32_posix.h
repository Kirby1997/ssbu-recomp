/* win32_posix.h - the few Win32 file calls of fighter_host.c, hook_host.c and newmodule_host.c, for a POSIX host.
 * #included by nnro.c (Linux only) right before those files; plat.h does not have these names (yet: "wanted in plat.h").
 *
 * Paths: those sources build Windows paths ("fighters_bin\\mario.dll", "%s\\*.dll"). Every call here takes such a path:
 * '\\' becomes '/', and in the calls that handle libraries (FindFirstFileA, GetFileAttributesA, CopyFileA, DeleteFileA,
 * LoadLibraryA) a trailing ".dll" becomes ".so" - the fighter / mod libraries are <name>.so on Linux, like the modules
 * (build_nro.py: name + buildplat.SHARED_SUFFIX). fopen only gets the separator change.
 *
 * FLS (FlsAlloc / FlsGetValue / FlsSetValue, fighter_host.c's per-fiber FsFiber) is plat.h's: per guest fiber, also when
 * another thread resumes it (os_threads.c makes each guest fiber plat's current fiber). */
#ifndef SSBU_FIGHTER_WIN32_POSIX_H
#define SSBU_FIGHTER_WIN32_POSIX_H
#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x80u
#endif
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

/* "a\\b.dll" -> "a/b.so" (lib = 1) or "a/b.dll" (lib = 0) */
static const char *fsx_path(char *out, size_t cap, const char *in, int lib) {
    size_t n = 0;
    for (; in[n] && n + 1 < cap; n++) out[n] = in[n] == '\\' ? '/' : in[n];
    out[n] = 0;
    if (lib && n >= 4 && strcasecmp(out + n - 4, ".dll") == 0 && n - 1 < cap) strcpy(out + n - 4, ".so");
    return out;
}

typedef struct { DWORD dwFileAttributes; char cFileName[MAX_PATH]; } WIN32_FIND_DATAA;
typedef struct { DIR *d; char dir[MAX_PATH]; char pat[MAX_PATH]; } FsxFind;
static BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    FsxFind *f = (FsxFind *)h;
    struct dirent *e;
    while ((e = readdir(f->d)) != NULL) {
        if (fnmatch(f->pat, e->d_name, 0) != 0) continue;
        char full[MAX_PATH * 2]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        fd->dwFileAttributes = stat(full, &st) == 0 && S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
static BOOL FindClose(HANDLE h) { FsxFind *f = (FsxFind *)h; if (f) { closedir(f->d); free(f); } return TRUE; }
static HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd) {        /* "dir\\*.dll": the last component is the pattern */
    char p[MAX_PATH];
    fsx_path(p, sizeof p, pattern, 1);
    FsxFind *f = calloc(1, sizeof *f);
    if (!f) return INVALID_HANDLE_VALUE;
    char *slash = strrchr(p, '/');
    if (slash) { *slash = 0; snprintf(f->dir, sizeof f->dir, "%s", p[0] ? p : "/"); snprintf(f->pat, sizeof f->pat, "%s", slash + 1); }
    else { strcpy(f->dir, "."); snprintf(f->pat, sizeof f->pat, "%s", p); }
    if (!(f->d = opendir(f->dir))) { free(f); SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (!FindNextFileA(f, fd)) { FindClose(f); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return f;
}
static DWORD GetFileAttributesA(const char *path) {
    char p[MAX_PATH]; struct stat st;
    if (stat(fsx_path(p, sizeof p, path, 1), &st) != 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
static BOOL DeleteFileA(const char *path) { char p[MAX_PATH]; return unlink(fsx_path(p, sizeof p, path, 1)) == 0; }
static BOOL CreateDirectoryA(const char *path, void *sa) {
    char p[MAX_PATH]; (void)sa;
    if (mkdir(fsx_path(p, sizeof p, path, 0), 0777) == 0) return TRUE;
    SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND);
    return FALSE;
}
static BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists) {   /* to a temporary name, then rename: a reader never sees half a file */
    char s[MAX_PATH], d[MAX_PATH], tmp[MAX_PATH + 32];
    fsx_path(s, sizeof s, from, 1); fsx_path(d, sizeof d, to, 1);
    if (fail_if_exists && access(d, F_OK) == 0) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    int in = open(s, O_RDONLY | O_CLOEXEC);
    if (in < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", d, (long)getpid());
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755), ok = out >= 0;
    char buf[1 << 16];
    for (ssize_t r; ok && (r = read(in, buf, sizeof buf)) != 0;) {
        if (r < 0) { if (errno == EINTR) continue; ok = 0; break; }
        for (ssize_t w = 0, k; ok && w < r; w += k) if ((k = write(out, buf + w, (size_t)(r - w))) < 0) { if (errno == EINTR) k = 0; else ok = 0; }
    }
    close(in);
    if (out >= 0 && close(out) != 0) ok = 0;
    if (ok && rename(tmp, d) != 0) ok = 0;
    if (!ok) { if (out >= 0) unlink(tmp); SetLastError(ERROR_ACCESS_DENIED); }
    return ok;
}
static int _putenv_s(const char *name, const char *value) { return setenv(name, value, 1) == 0 ? 0 : errno; }
static BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {         /* plat.h OpenProcess: a handle signalled when the process is gone */
    *code = WaitForSingleObject(h, 0) == WAIT_TIMEOUT ? STILL_ACTIVE : 0;
    return TRUE;
}
static FILE *fsx_fopen(const char *path, const char *mode) { char p[MAX_PATH]; return fopen(fsx_path(p, sizeof p, path, 0), mode); }
static HMODULE fsx_LoadLibraryA(const char *path) { char p[MAX_PATH]; return LoadLibraryA(fsx_path(p, sizeof p, path, 1)); }
#define fopen fsx_fopen
#define LoadLibraryA fsx_LoadLibraryA

#endif /* SSBU_FIGHTER_WIN32_POSIX_H */
