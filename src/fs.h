/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* Host filesystem operations; archive names remain DOS-style. */
#ifndef AIN_FS_H
#define AIN_FS_H

#ifdef _WIN32
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS 1
#endif
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define ain_stricmp _stricmp
#define AIN_PATH_SEP "\\"
#else
#include <strings.h>
#define ain_stricmp strcasecmp
#define AIN_PATH_SEP "/"
#endif
#define ain_strdup strdup

typedef struct AinDir AinDir;
typedef struct {
    const char *name;       /* valid until next read/close */
    int isdir;
    int symlink;           /* do not recurse through directory symlinks */
} AinDirEntry;

/* Filesystem failures are reported on stderr. Probe: 1 exists, 0 missing,
 * -1 error. Directory read: 1 entry, 0 end, -1 error. */
int ain_fs_probe(const char *path, int *isdir);
/* Archive file sizes must fit the format's 32-bit member-size field. */
int ain_fs_file_size(const char *path, uint32_t *size);
AinDir *ain_fs_dir_open(const char *path);
int ain_fs_dir_read(AinDir *dir, AinDirEntry *entry);
void ain_fs_dir_close(AinDir *dir);
/* mkdir: 0 created, 1 already a directory, -1 error. */
int ain_fs_mkdir(const char *path);
uint32_t ain_fs_dostime(const char *path);
uint32_t ain_fs_now(void);
int ain_fs_set_dostime(const char *path, uint32_t ts);
uint8_t ain_fs_dosattr(const char *path, int *isdir);
int ain_fs_set_hidden(const char *path);

void ain_fs_native_path(char *path);
/* Resolve an existing path; caller frees the result. NULL reports an error. */
char *ain_fs_fullpath(const char *path);
const char *ain_fs_basename(const char *path);
int ain_fs_same_member(const char *a, const char *b);
void ain_fs_init(void);
/* Monotonic wall-clock seconds, or -1 with a diagnostic on failure. */
double ain_fs_time(void);

#endif
