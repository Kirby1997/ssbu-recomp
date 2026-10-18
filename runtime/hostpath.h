/* hostpath.h - host paths for the port (PORT_AUDIT Phase 6), and on Linux the Win32 file API subset the file services use.
 *
 * Everyone:
 *   host_exe_dir()        folder of the executable WITH a trailing separator ('/' on Linux, '\\' on Windows). The Windows folder
 *                         layout is kept relative to it: <exe dir>../romfs, ../userdata, ../config, ../sysdata/fonts.
 *   host_path_fix(p)      '\\' -> '/' in place (Linux; no-op on Windows, which takes both).
 *   host_path_resolve()   Linux: '\\' -> '/' plus a case-insensitive lookup of every component that does not exist with the case
 *                         given (Windows lookups are case-insensitive; romfs/save files moved from Windows keep their case).
 *   nnfs_handle_fd(h)     Linux: the descriptor behind an nn::fs FileHandle (nnlibc2_c5b.inc's SetFileSize), -1 if h is not one.
 *
 * Linux, opt-in (#define HOSTPATH_FILE_API before including): CreateFileA, ReadFile/WriteFile (OVERLAPPED = pread/pwrite),
 * GetFileSizeEx, SetFilePointerEx, SetEndOfFile, FlushFileBuffers (fsync), GetFileAttributes(Ex)A, SetFileAttributesA,
 * CreateDirectoryA, RemoveDirectoryA, DeleteFileA, MoveFileExA (WRITE_THROUGH = fsync of the file, rename, fsync of both folders), CopyFileA,
 * FindFirstFileA/FindNextFileA/FindClose and fopen, all taking Windows paths ('\\' or '/', any case). The real symbols are
 * hp_*: the Win32 names are macros, so a file that does not opt in cannot collide with them. File handles are plat.h custom
 * handles (CloseHandle closes them). DIFF from Win32:
 *   - case folding is ASCII only (a non-ASCII byte must match exactly); a name that exists in two cases (possible on Linux)
 *     resolves to the exact match first, else to the first in Windows (upper-case ordinal) order.
 *   - FindFirstFileA reads the whole folder at once and returns it sorted like NTFS (".", "..", then upper-case ordinal);
 *     patterns are fnmatch with case folding ("*.*" = "*"). cFileName/paths are UTF-8 (no ANSI code page, no W functions).
 *   - share modes are ignored (POSIX has no mandatory locks); a name with a literal '\\' cannot be reached.
 *   - directory listings used by the case-insensitive lookup are cached while the folder's mtime is unchanged and older than
 *     2 s (a coarse timestamp cannot hide a change); a folder changed in the last 2 s is always read again. */
#ifndef SSBU_HOSTPATH_H
#define SSBU_HOSTPATH_H
#include <stddef.h>
#include <stdint.h>
#include "plat.h"
#ifdef __cplusplus
extern "C" {
#endif

const char *host_exe_dir(void);
void host_path_fix(char *p);

#ifndef _WIN32
int nnfs_handle_fd(uint64_t h);
/* `in` -> `out` (n bytes): separators fixed, existing components matched case-insensitively; components after the first
 * missing one are kept as given. 1 = the result exists, 0 = it does not (out is still the best path to create it). */
int host_path_resolve(const char *in, char *out, size_t n);
int host_make_parents(const char *path);      /* creates the missing folders above `path` (Windows path ok); 0 or -errno */
int host_fsync_parent(const char *path);      /* fsync of the folder that holds `path`; 0 or -errno */
/* Fault injection for tests (tests/port/fs): called before every mutating file operation (open for writing, write,
 * truncate, fsync, rename, unlink, mkdir, rmdir, copy) with its name and path. NULL (the default) in the game. */
extern void (*hostpath_fault_hook)(const char *op, const char *path);

#ifdef HOSTPATH_FILE_API
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define FILE_SHARE_DELETE 0x4u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_HIDDEN 0x2u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_COPY_ALLOWED 0x2u
#define MOVEFILE_WRITE_THROUGH 0x8u
#ifndef ERROR_HANDLE_EOF
#define ERROR_HANDLE_EOF 38
#endif
#ifndef ERROR_FILE_EXISTS
#define ERROR_FILE_EXISTS 80
#endif
#ifndef ERROR_DISK_FULL
#define ERROR_DISK_FULL 112
#endif
#ifndef ERROR_DIR_NOT_EMPTY
#define ERROR_DIR_NOT_EMPTY 145
#endif
#ifndef ERROR_GEN_FAILURE
#define ERROR_GEN_FAILURE 31
#endif
typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[256 * 4];                  /* NAME_MAX bytes of UTF-8 + NUL (Win32: MAX_PATH UTF-16 units) */
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

HANDLE hp_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL hp_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov);
BOOL hp_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov);
BOOL hp_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
BOOL hp_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER pos, DWORD method);
BOOL hp_SetEndOfFile(HANDLE h);
BOOL hp_FlushFileBuffers(HANDLE h);
DWORD hp_GetFileAttributesA(LPCSTR path);
BOOL hp_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out);
BOOL hp_SetFileAttributesA(LPCSTR path, DWORD attrs);
BOOL hp_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL hp_RemoveDirectoryA(LPCSTR path);
BOOL hp_DeleteFileA(LPCSTR path);
BOOL hp_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL hp_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
HANDLE hp_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd);
BOOL hp_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd);
BOOL hp_FindClose(HANDLE h);
FILE *hp_fopen(const char *path, const char *mode);
#define CreateFileA hp_CreateFileA
#define ReadFile hp_ReadFile
#define WriteFile hp_WriteFile
#define GetFileSizeEx hp_GetFileSizeEx
#define SetFilePointerEx hp_SetFilePointerEx
#define SetEndOfFile hp_SetEndOfFile
#define FlushFileBuffers hp_FlushFileBuffers
#define GetFileAttributesA hp_GetFileAttributesA
#define GetFileAttributesExA hp_GetFileAttributesExA
#define SetFileAttributesA hp_SetFileAttributesA
#define CreateDirectoryA hp_CreateDirectoryA
#define RemoveDirectoryA hp_RemoveDirectoryA
#define DeleteFileA hp_DeleteFileA
#define MoveFileExA hp_MoveFileExA
#define MoveFileA(a, b) hp_MoveFileExA(a, b, 0)
#define CopyFileA hp_CopyFileA
#define FindFirstFileA hp_FindFirstFileA
#define FindNextFileA hp_FindNextFileA
#define FindClose hp_FindClose
#define fopen(p, m) hp_fopen(p, m)
#endif /* HOSTPATH_FILE_API */
#endif /* !_WIN32 */

#ifdef __cplusplus
}
#endif
#endif
