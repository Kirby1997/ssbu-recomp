/* libc2_tu.c - nnlibc2.c (with nnlibc2_c5b.inc) compiled whole, so test_fs.c can call its static nn::fs::SetFileSize
 * handler (fs_SetFileSize_c5b: ftruncate + lseek on nnfs_handle_fd) exactly as the game reaches it through musl's fopen. */
#include "nnlibc2.c"
void test_SetFileSize(Ctx *c) { fs_SetFileSize_c5b(c); }
