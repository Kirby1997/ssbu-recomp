/* nnlibc2_c5b.inc on Linux: nn::fs::SetFileSize (what musl's fopen "w" calls for O_TRUNC) = ftruncate + lseek on the
 * descriptor nnfs.c's nnfs_handle_fd gives for a FileHandle. nnlibc2.c is compiled INTO this test. Built twice:
 *   -DWITH_NNFS_FD  this file defines nnfs_handle_fd (handle = 0x1000 + fd, a stand-in for whatever nnfs.c chooses)
 *   without         nnfs.c's Linux port has not defined it yet: SetFileSize must refuse (0x2ee202), not guess */
#include "nnlibc2.c"
#include "mt.h"
#include <fcntl.h>
#include <sys/stat.h>

/* what the rest of the runtime would provide (nothing of it is reached by this test) */
#define NOTREACHED(what) do { fprintf(stderr, "not expected: %s\n", what); abort(); } while (0)
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; NOTREACHED("call_guest"); }
int eh_find_function(uint64_t pc, EhFunction *out) { (void)pc; (void)out; return 0; }
void eh_enter_pad(Ctx *c, const EhPad *pad) { (void)c; (void)pad; NOTREACHED("eh_enter_pad"); }
uint64_t eh_in_flight(Ctx *c) { (void)c; return 0; }
const char *eh_type_name(uint64_t e) { (void)e; return ""; }
void eh_set_unwinder(EhRaiseFn raise) { (void)raise; }
ImportFn import_lookup(const char *name) { (void)name; return NULL; }
void import_set_address(const char *name, void *addr) { (void)name; (void)addr; }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
void register_unwind_imports(void) {}
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
int sdk_image_ready(void) { return 0; }
ImportFn sdk_lifted(const char *name) { (void)name; return NULL; }
uint64_t sdk_symbol(const char *name) { (void)name; return 0; }
void sdk_host_override(const char *name, ImportFn fn) { (void)name; (void)fn; }
int sdk_install(const char *name) { (void)name; return 0; }
uint32_t fs_mount_plain(const char *name, const char *host_dir) { (void)name; (void)host_dir; return 1; }

#ifdef WITH_NNFS_FD
int nnfs_handle_fd(uint64_t handle) { return handle >= 0x1000 && handle < 0x1000 + 0x10000 ? (int)(handle - 0x1000) : -1; }
#endif

static off_t fsize(int fd) { struct stat st; return fstat(fd, &st) == 0 ? st.st_size : -1; }

int main(void) {
    Ctx *c = calloc(1, sizeof *c);
    char path[256];
    snprintf(path, sizeof path, "%s/ssbu_libc2_%d.bin", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "0123456789abcdef", 16) == 16);
#ifdef WITH_NNFS_FD
    SECTION("SetFileSize: truncate to 0 (musl fopen \"w\"), grow to 100 (zero filled), shrink to 5; file position = new size");
    c->x[0] = 0x1000 + (uint64_t)fd; c->x[1] = 0; fs_SetFileSize_c5b(c);
    CHECK(c->x[0] == 0 && fsize(fd) == 0 && lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(pwrite(fd, "hello world", 11, 0) == 11);
    c->x[0] = 0x1000 + (uint64_t)fd; c->x[1] = 100; fs_SetFileSize_c5b(c);
    CHECK(c->x[0] == 0 && fsize(fd) == 100 && lseek(fd, 0, SEEK_CUR) == 100);
    {   char b[100]; int zero = 1; CHECK(pread(fd, b, 100, 0) == 100); for (int i = 11; i < 100; i++) zero &= b[i] == 0; CHECK(zero && !memcmp(b, "hello world", 11)); }
    c->x[0] = 0x1000 + (uint64_t)fd; c->x[1] = 5; fs_SetFileSize_c5b(c);
    CHECK(c->x[0] == 0 && fsize(fd) == 5 && lseek(fd, 0, SEEK_CUR) == 5);
    {   char b[8] = ""; CHECK(pread(fd, b, 8, 0) == 5 && !memcmp(b, "hello", 5)); }
    SECTION("SetFileSize: bad handle / negative size / read-only descriptor -> 0x2ee202, file unchanged");
    c->x[0] = 0x5; c->x[1] = 0; fs_SetFileSize_c5b(c); CHECK(c->x[0] == 0x2ee202 && fsize(fd) == 5);
    c->x[0] = 0x1000 + (uint64_t)fd; c->x[1] = (uint64_t)-1; fs_SetFileSize_c5b(c); CHECK(c->x[0] == 0x2ee202 && fsize(fd) == 5);
    {   int ro = open(path, O_RDONLY); c->x[0] = 0x1000 + (uint64_t)ro; c->x[1] = 0; fs_SetFileSize_c5b(c); CHECK(c->x[0] == 0x2ee202 && fsize(fd) == 5); close(ro); }
#else
    SECTION("no nnfs_handle_fd (weak reference is NULL): SetFileSize refuses, nothing is truncated");
    CHECK(&nnfs_handle_fd == NULL);
    c->x[0] = (uint64_t)fd; c->x[1] = 0; fs_SetFileSize_c5b(c);
    CHECK(c->x[0] == 0x2ee202 && fsize(fd) == 16);
#endif
    close(fd); unlink(path); free(c);
#ifdef WITH_NNFS_FD
    return mt_done("test_libc2");
#else
    return mt_done("test_libc2_nofs");
#endif
}
