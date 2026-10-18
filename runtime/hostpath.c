/* hostpath.c - host paths, and on Linux the Win32 file API subset of the file services (hostpath.h; PORT_AUDIT Phase 6).
 * Windows: host_exe_dir / host_path_fix only. Linux: + case-insensitive path resolution with a folder-listing cache, the
 * hp_* file functions (plat.h custom handles), nnfs_handle_fd and the test fault hook. */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOSTPATH_FILE_API
#include "hostpath.h"

static char g_exe_dir[MAX_PATH];
static INIT_ONCE g_exe_once = INIT_ONCE_STATIC_INIT;

#ifdef _WIN32
static BOOL CALLBACK exe_dir_init(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    DWORD n = GetModuleFileNameA(NULL, g_exe_dir, MAX_PATH);
    if (n >= MAX_PATH) n = 0;
    while (n && g_exe_dir[n - 1] != '\\' && g_exe_dir[n - 1] != '/') n--;
    g_exe_dir[n] = 0;
    if (!n) strcpy(g_exe_dir, ".\\");
    return TRUE;
}
const char *host_exe_dir(void) { InitOnceExecuteOnce(&g_exe_once, exe_dir_init, NULL, NULL); return g_exe_dir; }
void host_path_fix(char *p) { (void)p; }

#else /* ------------------------------------------------------------------------------------------- Linux */
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#undef fopen

void (*hostpath_fault_hook)(const char *op, const char *path);
#define FAULT(op, p) do { if (hostpath_fault_hook) hostpath_fault_hook(op, p); } while (0)

static BOOL CALLBACK exe_dir_init(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    ssize_t n = readlink("/proc/self/exe", g_exe_dir, MAX_PATH - 2);
    if (n < 0) n = 0;
    while (n > 0 && g_exe_dir[n - 1] != '/') n--;
    g_exe_dir[n] = 0;
    if (!n) strcpy(g_exe_dir, "./");
    return TRUE;
}
const char *host_exe_dir(void) { InitOnceExecuteOnce(&g_exe_once, exe_dir_init, NULL, NULL); return g_exe_dir; }
void host_path_fix(char *p) { for (; p && *p; p++) if (*p == '\\') *p = '/'; }

static DWORD win_error(int e) {
    switch (e) {
    case 0: return 0;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EISDIR: case EROFS: case EBUSY: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENOSPC: case EDQUOT: return ERROR_DISK_FULL;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    default: return ERROR_GEN_FAILURE;
    }
}
static BOOL fail_errno(void) { SetLastError(win_error(errno)); return FALSE; }

/* ---- case-insensitive lookup. Order and folding: ASCII upper case, then the exact bytes (NTFS order for ASCII names). */
static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}
static int name_order(const void *x, const void *y) {
    const char *a = *(const char *const *)x, *b = *(const char *const *)y;
    int r = ci_cmp(a, b);
    return r ? r : strcmp(a, b);
}
typedef struct DirCache { struct DirCache *next; char *path; dev_t dev; ino_t ino; struct timespec mtime; int n; char **names; } DirCache;
#define DC_BUCKETS 1024
#define DC_MAX 8192
static DirCache *g_dc[DC_BUCKETS];
static int g_dc_count;
static pthread_mutex_t g_dc_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned dc_hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static void dc_free(DirCache *d) { if (d) { free(d->names); free(d->path); free(d); } }
static DirCache *dc_read(const char *dir, const struct stat *st) {   /* names point into the same block as the array */
    DIR *dh = opendir(dir);
    if (!dh) return NULL;
    size_t cap = 64, n = 0, bytes = 0, bcap = 4096;
    char **tmp = malloc(cap * sizeof *tmp), *pool = malloc(bcap);
    struct dirent *de;
    while ((de = readdir(dh)) != NULL) {
        size_t l = strlen(de->d_name) + 1;
        if (n == cap) tmp = realloc(tmp, (cap *= 2) * sizeof *tmp);
        if (bytes + l > bcap) { while (bytes + l > bcap) bcap *= 2; pool = realloc(pool, bcap); }
        memcpy(pool + bytes, de->d_name, l);
        tmp[n++] = (char *)(uintptr_t)bytes;
        bytes += l;
    }
    closedir(dh);
    DirCache *d = calloc(1, sizeof *d);
    d->names = malloc(n * sizeof(char *) + bytes + 1);
    char *block = (char *)(d->names + n);
    memcpy(block, pool, bytes);
    for (size_t i = 0; i < n; i++) d->names[i] = block + (uintptr_t)tmp[i];
    free(tmp); free(pool);
    d->n = (int)n;
    qsort(d->names, n, sizeof(char *), name_order);
    d->path = strdup(dir);
    d->dev = st->st_dev; d->ino = st->st_ino; d->mtime = st->st_mtim;
    return d;
}
/* `name` in folder `dir`, any case: 1 and the name as stored in `real` (same length: ASCII folding). */
static int dc_lookup(const char *dir, const char *name, char *real) {
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    pthread_mutex_lock(&g_dc_mu);
    unsigned b = dc_hash(dir) & (DC_BUCKETS - 1);
    DirCache **pp = &g_dc[b], *d = NULL, *temp = NULL;
    for (; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->path, dir)) { d = *pp; break; }
    if (d && !(d->dev == st.st_dev && d->ino == st.st_ino && d->mtime.tv_sec == st.st_mtim.tv_sec && d->mtime.tv_nsec == st.st_mtim.tv_nsec)) {
        *pp = d->next; dc_free(d); d = NULL; g_dc_count--;
    }
    if (!d) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        d = dc_read(dir, &st);
        if (d && now.tv_sec - st.st_mtim.tv_sec > 2) {      /* stable folder: keep its listing (a coarse mtime cannot hide a later change) */
            if (g_dc_count >= DC_MAX) {
                for (int i = 0; i < DC_BUCKETS; i++) while (g_dc[i]) { DirCache *x = g_dc[i]; g_dc[i] = x->next; dc_free(x); }
                g_dc_count = 0;
            }
            d->next = g_dc[b]; g_dc[b] = d; g_dc_count++;
        } else temp = d;
    }
    int found = 0;
    if (d) {
        int lo = 0, hi = d->n - 1, at = -1;
        while (lo <= hi) {                                /* leftmost entry equal ignoring case */
            int mid = (lo + hi) / 2, r = ci_cmp(d->names[mid], name);
            if (r < 0) lo = mid + 1; else { if (!r) at = mid; hi = mid - 1; }
        }
        if (at >= 0) {
            int pick = at;
            for (int i = at; i < d->n && !ci_cmp(d->names[i], name); i++) if (!strcmp(d->names[i], name)) { pick = i; break; }
            if (strlen(d->names[pick]) == strlen(name)) { strcpy(real, d->names[pick]); found = 1; }
        }
    }
    pthread_mutex_unlock(&g_dc_mu);
    dc_free(temp);
    return found;
}
int host_path_resolve(const char *in, char *out, size_t n) {
    struct stat st;
    size_t len = in ? strlen(in) : 0;
    if (!n) return 0;
    if (!in || len >= n) { snprintf(out, n, "%s", in ? in : ""); host_path_fix(out); errno = ENAMETOOLONG; return 0; }
    memcpy(out, in, len + 1);
    host_path_fix(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    if (!len) { errno = ENOENT; return 0; }
    if (stat(out, &st) == 0) return 1;                     /* the common case: the case is right */
    if (errno != ENOENT && errno != ENOTDIR) return 0;
    size_t start = 0;                                      /* first byte after the deepest existing folder */
    for (size_t i = len; i-- > 0;) {
        if (out[i] != '/') continue;
        if (i == 0) { start = 1; break; }
        out[i] = 0;
        int ok = stat(out, &st) == 0 && S_ISDIR(st.st_mode);
        out[i] = '/';
        if (ok) { start = i + 1; break; }
    }
    char *p = out + start, name[1024], real[1024];
    while (*p) {
        char *e = strchr(p, '/');
        size_t cl = e ? (size_t)(e - p) : strlen(p);
        if (cl && !(cl == 1 && p[0] == '.') && !(cl == 2 && p[0] == '.' && p[1] == '.')) {
            if (cl >= sizeof name) { errno = ENAMETOOLONG; return 0; }
            memcpy(name, p, cl); name[cl] = 0;
            int ok;
            if (p == out) ok = dc_lookup(".", name, real);
            else if (p == out + 1 && out[0] == '/') ok = dc_lookup("/", name, real);
            else { p[-1] = 0; ok = dc_lookup(out, name, real); p[-1] = '/'; }
            if (!ok) { errno = ENOENT; return 0; }
            memcpy(p, real, cl);
        }
        p += cl;
        if (*p == '/') p++;
    }
    return stat(out, &st) == 0;
}
static int resolve(const char *in, char *out) { return host_path_resolve(in, out, MAX_PATH); }
int host_make_parents(const char *path) {
    char rp[MAX_PATH];
    struct stat st;
    resolve(path, rp);
    char *s = strrchr(rp, '/');
    if (!s || s == rp) return 0;
    *s = 0;
    if (stat(rp, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
    *s = '/';
    for (char *p = rp + 1; p < s; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (stat(rp, &st) != 0) { FAULT("mkdir", rp); if (mkdir(rp, 0777) != 0 && errno != EEXIST) { int e = errno; *p = '/'; return -e; } }
        *p = '/';
    }
    *s = 0;
    FAULT("mkdir", rp);
    if (mkdir(rp, 0777) != 0 && errno != EEXIST) return -errno;
    return 0;
}
static int fsync_dir(const char *dir) {
    int fd = open(dir[0] ? dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    FAULT("fsync-dir", dir);
    int r = fsync(fd) == 0 ? 0 : -errno;
    close(fd);
    return r;
}
static void dir_of(const char *rp, char *dir) {
    snprintf(dir, MAX_PATH, "%s", rp);
    char *s = strrchr(dir, '/');
    if (!s) strcpy(dir, ".");
    else if (s == dir) dir[1] = 0;
    else *s = 0;
}
int host_fsync_parent(const char *path) {
    char rp[MAX_PATH], dir[MAX_PATH];
    resolve(path, rp);
    dir_of(rp, dir);
    return fsync_dir(dir);
}

/* ---- open files: plat.h custom handles, and a registry of the live ones (a FileHandle value from the guest is checked
 * here before it is used, so a stale or made-up handle fails like an invalid handle instead of being dereferenced) */
typedef struct { HANDLE h; int fd; } HpFile;
static HANDLE *g_reg;
static size_t g_reg_cap, g_reg_n;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static HpFile **g_reg_val;
static size_t reg_slot(HANDLE h) { uintptr_t x = (uintptr_t)h; x ^= x >> 17; x *= 0x9e3779b97f4a7c15ull; return (size_t)(x >> 20); }
static void reg_put_locked(HANDLE h, HpFile *f) {
    size_t i = reg_slot(h) & (g_reg_cap - 1);
    while (g_reg[i] && g_reg[i] != INVALID_HANDLE_VALUE) i = (i + 1) & (g_reg_cap - 1);
    g_reg[i] = h; g_reg_val[i] = f;
}
static void reg_add(HANDLE h, HpFile *f) {
    pthread_mutex_lock(&g_reg_mu);
    if ((g_reg_n + 1) * 2 > g_reg_cap) {                  /* grow and drop tombstones */
        HANDLE *old = g_reg; HpFile **oldv = g_reg_val; size_t oc = g_reg_cap, live = 1;
        for (size_t i = 0; i < oc; i++) live += old[i] && old[i] != INVALID_HANDLE_VALUE;
        for (g_reg_cap = 256; g_reg_cap < live * 4; g_reg_cap *= 2) {}
        g_reg = calloc(g_reg_cap, sizeof *g_reg); g_reg_val = calloc(g_reg_cap, sizeof *g_reg_val);
        g_reg_n = 0;
        for (size_t i = 0; i < oc; i++) if (old[i] && old[i] != INVALID_HANDLE_VALUE) { reg_put_locked(old[i], oldv[i]); g_reg_n++; }
        free(old); free(oldv);
    }
    reg_put_locked(h, f);
    g_reg_n++;
    pthread_mutex_unlock(&g_reg_mu);
}
static HpFile *reg_find_locked(HANDLE h, size_t *at) {
    if (!g_reg_cap || !h || h == INVALID_HANDLE_VALUE) return NULL;
    for (size_t i = reg_slot(h) & (g_reg_cap - 1), k = 0; g_reg[i] && k < g_reg_cap; i = (i + 1) & (g_reg_cap - 1), k++)
        if (g_reg[i] == h) { if (at) *at = i; return g_reg_val[i]; }
    return NULL;
}
static HpFile *reg_get(HANDLE h) { pthread_mutex_lock(&g_reg_mu); HpFile *f = reg_find_locked(h, NULL); pthread_mutex_unlock(&g_reg_mu); return f; }
static void hp_file_destroy(void *payload) {
    HpFile *f = payload;
    size_t at;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_find_locked(f->h, &at)) { g_reg[at] = INVALID_HANDLE_VALUE; g_reg_val[at] = NULL; }   /* tombstone: still counted until the next grow */
    pthread_mutex_unlock(&g_reg_mu);
    close(f->fd);
    free(f);
}
static const plat_handle_ops hp_file_ops = { "file", hp_file_destroy };
int nnfs_handle_fd(uint64_t h) { HpFile *f = reg_get((HANDLE)(uintptr_t)h); return f ? f->fd : -1; }

HANDLE hp_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    char rp[MAX_PATH];
    int existed = resolve(path, rp), of = O_CLOEXEC;
    int w = (access & GENERIC_WRITE) != 0;
    of |= w ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_WRITE_THROUGH) of |= O_DSYNC;
    if (w || (of & (O_CREAT | O_TRUNC))) FAULT("open-w", rp);
    int fd = open(rp, of, 0666);
    if (fd < 0) {                                         /* Win32: a missing folder on the way is ERROR_PATH_NOT_FOUND */
        SetLastError(errno == EEXIST ? ERROR_FILE_EXISTS : errno == ENOENT && (of & O_CREAT) ? ERROR_PATH_NOT_FOUND : win_error(errno));
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    HpFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    HANDLE h = plat_handle_create(&hp_file_ops, f);
    if (!h) { close(fd); free(f); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    f->h = h;
    reg_add(h, f);
    SetLastError(existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return h;
}
static uint64_t ov_offset(const OVERLAPPED *ov) { return (uint64_t)ov->Offset | (uint64_t)ov->OffsetHigh << 32; }
BOOL hp_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    HpFile *f = reg_get(h);
    DWORD total = 0;
    if (got) *got = 0;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    while (total < n) {
        ssize_t r = ov ? pread(f->fd, (char *)buf + total, n - total, (off_t)(ov_offset(ov) + total)) : read(f->fd, (char *)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; if (got) *got = total; return fail_errno(); }
        if (r == 0) break;
        total += (DWORD)r;
    }
    if (got) *got = total;
    if (ov && n && !total) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }   /* synchronous handle, offset at or past the end */
    return TRUE;
}
BOOL hp_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov) {
    HpFile *f = reg_get(h);
    DWORD total = 0;
    if (put) *put = 0;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    FAULT("write", NULL);
    while (total < n) {
        ssize_t r = ov ? pwrite(f->fd, (const char *)buf + total, n - total, (off_t)(ov_offset(ov) + total)) : write(f->fd, (const char *)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; if (put) *put = total; return fail_errno(); }
        total += (DWORD)r;
    }
    if (put) *put = total;
    return TRUE;
}
BOOL hp_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
    HpFile *f = reg_get(h);
    struct stat st;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (fstat(f->fd, &st) != 0) return fail_errno();
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}
BOOL hp_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER pos, DWORD method) {
    HpFile *f = reg_get(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    off_t r = lseek(f->fd, (off_t)dist.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (pos) pos->QuadPart = (LONGLONG)r;
    return TRUE;
}
BOOL hp_SetEndOfFile(HANDLE h) {
    HpFile *f = reg_get(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    off_t at = lseek(f->fd, 0, SEEK_CUR);
    FAULT("truncate", NULL);
    if (at < 0 || ftruncate(f->fd, at) != 0) return fail_errno();
    return TRUE;
}
BOOL hp_FlushFileBuffers(HANDLE h) {
    HpFile *f = reg_get(h);
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    FAULT("fsync", NULL);
    return fsync(f->fd) == 0 ? TRUE : fail_errno();
}

/* ---- attributes, folders, names */
static FILETIME to_filetime(struct timespec t) {
    uint64_t v = ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100u;
    FILETIME ft = { (DWORD)v, (DWORD)(v >> 32) };
    return ft;
}
static DWORD attrs_of(const struct stat *st) {
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}
DWORD hp_GetFileAttributesA(LPCSTR path) {
    char rp[MAX_PATH];
    struct stat st;
    if (!resolve(path, rp) || stat(rp, &st) != 0) { SetLastError(win_error(errno ? errno : ENOENT)); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st);
}
BOOL hp_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    char rp[MAX_PATH];
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA *a = out;
    if (level != GetFileExInfoStandard || !a) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!resolve(path, rp) || stat(rp, &st) != 0) { SetLastError(win_error(errno ? errno : ENOENT)); return FALSE; }
    a->dwFileAttributes = attrs_of(&st);
    a->ftCreationTime = to_filetime(st.st_ctim); a->ftLastAccessTime = to_filetime(st.st_atim); a->ftLastWriteTime = to_filetime(st.st_mtim);
    uint64_t sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    a->nFileSizeHigh = (DWORD)(sz >> 32); a->nFileSizeLow = (DWORD)sz;
    return TRUE;
}
BOOL hp_SetFileAttributesA(LPCSTR path, DWORD attrs) {       /* only READONLY has a meaning here (the owner's write bit) */
    char rp[MAX_PATH];
    struct stat st;
    if (!resolve(path, rp) || stat(rp, &st) != 0) return fail_errno();
    mode_t m = (attrs & FILE_ATTRIBUTE_READONLY) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | S_IWUSR);
    if ((m & 07777) != (st.st_mode & 07777) && chmod(rp, m & 07777) != 0) return fail_errno();
    return TRUE;
}
BOOL hp_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa) {
    (void)sa;
    char rp[MAX_PATH];
    if (resolve(path, rp)) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    FAULT("mkdir", rp);
    if (mkdir(rp, 0777) != 0) return fail_errno();
    return TRUE;
}
BOOL hp_RemoveDirectoryA(LPCSTR path) {
    char rp[MAX_PATH];
    if (!resolve(path, rp)) return fail_errno();
    FAULT("rmdir", rp);
    return rmdir(rp) == 0 ? TRUE : fail_errno();
}
BOOL hp_DeleteFileA(LPCSTR path) {
    char rp[MAX_PATH];
    struct stat st;
    if (!resolve(path, rp)) return fail_errno();
    if (lstat(rp, &st) == 0 && S_ISDIR(st.st_mode)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    FAULT("unlink", rp);
    return unlink(rp) == 0 ? TRUE : fail_errno();
}
static int copy_fd(int in, int out) {
    char buf[1 << 16];
    for (;;) {
        ssize_t r = copy_file_range(in, NULL, out, NULL, 1 << 30, 0);
        if (r > 0) continue;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        break;                                            /* EXDEV / EINVAL / ENOSYS: plain copy from where it stopped */
    }
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) return 0;
        for (ssize_t o = 0; o < r;) { ssize_t w = write(out, buf + o, (size_t)(r - o)); if (w < 0) { if (errno == EINTR) continue; return -1; } o += w; }
    }
}
BOOL hp_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    char a[MAX_PATH], b[MAX_PATH];
    struct stat st;
    if (!resolve(from, a)) return fail_errno();
    resolve(to, b);
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    if (fstat(in, &st) != 0 || S_ISDIR(st.st_mode)) { close(in); SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    FAULT("copy", b);
    int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) { int e = errno; close(in); SetLastError(e == EEXIST ? ERROR_FILE_EXISTS : win_error(e)); return FALSE; }
    int r = copy_fd(in, out), e = errno;
    close(in);
    if (close(out) != 0 && !r) { r = -1; e = errno; }
    if (r) { unlink(b); errno = e; return fail_errno(); }
    return TRUE;
}
BOOL hp_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[MAX_PATH], b[MAX_PATH], da[MAX_PATH], db[MAX_PATH];
    if (!resolve(from, a)) return fail_errno();
    resolve(to, b);
    if (flags & MOVEFILE_WRITE_THROUGH) {                 /* the data first: the new name must never reach the disk before it */
        int fd = open(a, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            FAULT("fsync", a);
            if (fsync(fd) != 0) { int e = errno; close(fd); errno = e; return fail_errno(); }
        }
        if (fd >= 0) close(fd);
    }
    FAULT("rename", b);
    int r;
    if (flags & MOVEFILE_REPLACE_EXISTING) r = rename(a, b);
    else {
        r = renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE);
        if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {          /* a file system without RENAME_NOREPLACE */
            struct stat st;
            if (lstat(b, &st) == 0) { errno = EEXIST; r = -1; } else r = rename(a, b);
        }
    }
    if (r != 0 && errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
        if (!hp_CopyFileA(a, b, !(flags & MOVEFILE_REPLACE_EXISTING))) return FALSE;
        if (flags & MOVEFILE_WRITE_THROUGH) { int fd = open(b, O_RDONLY | O_CLOEXEC); if (fd >= 0) { fsync(fd); close(fd); } }
        r = unlink(a);
    }
    if (r != 0) return fail_errno();
    if (flags & MOVEFILE_WRITE_THROUGH) {                 /* the rename itself is on disk when this returns */
        dir_of(b, db); dir_of(a, da);
        if (fsync_dir(db) != 0) return fail_errno();
        if (strcmp(da, db) && fsync_dir(da) != 0) return fail_errno();
    }
    return TRUE;
}
FILE *hp_fopen(const char *path, const char *mode) {
    char rp[MAX_PATH];
    resolve(path, rp);
    if (strpbrk(mode, "wa+")) FAULT("fopen-w", rp);
    return fopen(rp, mode);
}

/* ---- FindFirstFile: the whole folder, filtered and sorted at once */
#define FIND_MAGIC 0x444e4946u                             /* "FIND" */
typedef struct { char *name; DWORD attrs; uint64_t size; FILETIME c, a, w; } FindEnt;
typedef struct { uint32_t magic; int n, pos; FindEnt *e; } HpFind;
static int find_order(const void *x, const void *y) {
    const FindEnt *a = x, *b = y;
    int da = !strcmp(a->name, ".") ? 0 : !strcmp(a->name, "..") ? 1 : 2, db = !strcmp(b->name, ".") ? 0 : !strcmp(b->name, "..") ? 1 : 2;
    if (da != db) return da - db;
    int r = ci_cmp(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}
static void find_fill(const FindEnt *e, LPWIN32_FIND_DATAA fd) {
    memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = e->attrs;
    fd->ftCreationTime = e->c; fd->ftLastAccessTime = e->a; fd->ftLastWriteTime = e->w;
    fd->nFileSizeHigh = (DWORD)(e->size >> 32); fd->nFileSizeLow = (DWORD)e->size;
    snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->name);
}
static void find_free(HpFind *f) { for (int i = 0; i < f->n; i++) free(f->e[i].name); free(f->e); f->magic = 0; free(f); }
HANDLE hp_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd) {
    char dir[MAX_PATH], rp[MAX_PATH], pat[1024];
    snprintf(dir, sizeof dir, "%s", pattern ? pattern : "");
    char *s1 = strrchr(dir, '\\'), *s2 = strrchr(dir, '/'), *s = s1 > s2 ? s1 : s2;
    if (s) { snprintf(pat, sizeof pat, "%s", s + 1); if (s == dir) s[1] = 0; else *s = 0; }
    else { snprintf(pat, sizeof pat, "%s", dir); strcpy(dir, "."); }
    if (!pat[0]) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (!strcmp(pat, "*.*")) strcpy(pat, "*");
    for (char *q = pat; *q; q++)                           /* Win32 wildcards are only '*' and '?': '[' is a plain character */
        if (*q == '[' && strlen(pat) + 2 < sizeof pat) { memmove(q + 1, q, strlen(q) + 1); *q++ = '\\'; }
    if (!resolve(dir, rp)) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    DIR *dh = opendir(rp);
    if (!dh) { SetLastError(errno == ENOTDIR ? ERROR_PATH_NOT_FOUND : win_error(errno)); return INVALID_HANDLE_VALUE; }
    HpFind *f = calloc(1, sizeof *f);
    int cap = 0, dfd = dirfd(dh);
    struct dirent *de;
    while ((de = readdir(dh)) != NULL) {
        if (fnmatch(pat, de->d_name, FNM_CASEFOLD) != 0) continue;
        struct stat st;
        if (fstatat(dfd, de->d_name, &st, 0) != 0 && fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (f->n == cap) f->e = realloc(f->e, sizeof(FindEnt) * (size_t)(cap = cap ? cap * 2 : 64));
        FindEnt *e = &f->e[f->n++];
        e->name = strdup(de->d_name);
        e->attrs = attrs_of(&st);
        e->size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        e->c = to_filetime(st.st_ctim); e->a = to_filetime(st.st_atim); e->w = to_filetime(st.st_mtim);
    }
    closedir(dh);
    if (!f->n) { find_free(f); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    qsort(f->e, (size_t)f->n, sizeof(FindEnt), find_order);
    f->magic = FIND_MAGIC;
    find_fill(&f->e[0], fd);
    f->pos = 1;
    return (HANDLE)f;
}
BOOL hp_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) {
    HpFind *f = h;
    if (!f || h == INVALID_HANDLE_VALUE || f->magic != FIND_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (f->pos >= f->n) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    find_fill(&f->e[f->pos++], fd);
    return TRUE;
}
BOOL hp_FindClose(HANDLE h) {
    HpFind *f = h;
    if (!f || h == INVALID_HANDLE_VALUE || f->magic != FIND_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    find_free(f);
    return TRUE;
}
#endif
