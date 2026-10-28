/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "fs.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int fs_error(const char *operation, const char *path)
{
    fprintf(stderr, "error: cannot %s %s: %s\n", operation, path, strerror(errno));
    return -1;
}

int ain_fs_file_size(const char *path, uint32_t *size)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st))
        return fs_error("read size of", path);
    if ((st.st_mode & _S_IFMT) != _S_IFREG) {
#else
    struct stat st;
    if (stat(path, &st))
        return fs_error("read size of", path);
    if (!S_ISREG(st.st_mode)) {
#endif
        fprintf(stderr, "error: not a regular file: %s\n", path);
        return -1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        fprintf(stderr, "error: file exceeds AIN member size limit: %s\n", path);
        return -1;
    }
    *size = (uint32_t)st.st_size;
    return 0;
}

char *ain_fs_fullpath(const char *path)
{
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = realpath(path, NULL);
#endif
    if (!full)
        fs_error("resolve full path of", path);
    return full;
}

void ain_fs_native_path(char *path)
{
    for (; *path; ++path) {
#ifdef _WIN32
        if (*path == '/') *path = '\\';
#else
        if (*path == '\\') *path = '/';
#endif
    }
}

const char *ain_fs_basename(const char *path)
{
    const char *base = path;
    for (; *path; ++path)
        if (*path == '/' || *path == '\\')
            base = path + 1;
    return base;
}

int ain_fs_same_member(const char *a, const char *b)
{
    for (;;) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb)
            return 0;
        if (!ca)
            return 1;
        ++a;
        ++b;
    }
}

static uint32_t pack_time(unsigned year, unsigned month, unsigned day,
                          unsigned hour, unsigned minute, unsigned second)
{
    uint32_t date = 0;
    if (year >= 1980 && year <= 2107 && month && day)
        date = day | (month << 5) | ((year - 1980) << 9);
    return (date << 16) | (hour << 11) | (minute << 5) | (second / 2);
}

#ifdef _WIN32
#include <windows.h>
#include <direct.h>

struct AinDir {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    int first;
};

static int win_error(const char *operation, const char *path)
{
    fprintf(stderr, "error: cannot %s %s (Windows error %lu)\n",
            operation, path, (unsigned long)GetLastError());
    return -1;
}

void ain_fs_init(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

double ain_fs_time(void)
{
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        win_error("read elapsed-time clock", "");
        return -1;
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

int ain_fs_probe(const char *path, int *isdir)
{
    DWORD attr = GetFileAttributesA(path);
    *isdir = 0;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return 0;
        return win_error("inspect", path);
    }
    *isdir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
}

AinDir *ain_fs_dir_open(const char *path)
{
    char spec[1100];
    AinDir *dir = (AinDir *)calloc(1, sizeof *dir);
    if (!dir) {
        fs_error("allocate directory reader for", path);
        return NULL;
    }
    snprintf(spec, sizeof spec, "%s%s*", *path ? path : ".",
             *path && (path[strlen(path) - 1] == '/' || path[strlen(path) - 1] == '\\') ? "" : "\\");
    dir->handle = FindFirstFileA(spec, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        win_error("read directory", path);
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

int ain_fs_dir_read(AinDir *dir, AinDirEntry *entry)
{
    if (!dir->first && !FindNextFileA(dir->handle, &dir->data)) {
        if (GetLastError() == ERROR_NO_MORE_FILES)
            return 0;
        return win_error("read directory", "");
    }
    dir->first = 0;
    entry->name = dir->data.cFileName;
    entry->isdir = (dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry->symlink = (dir->data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return 1;
}

void ain_fs_dir_close(AinDir *dir)
{
    if (!FindClose(dir->handle))
        win_error("close directory", "");
    free(dir);
}

int ain_fs_mkdir(const char *path)
{
    int isdir;
    if (!_mkdir(path))
        return 0;
    if (errno == EEXIST && ain_fs_probe(path, &isdir) == 1 && isdir)
        return 1;
    return fs_error("create directory", path);
}

static uint32_t pack_systemtime(const SYSTEMTIME *st)
{
    return pack_time(st->wYear, st->wMonth, st->wDay,
                     st->wHour, st->wMinute, st->wSecond);
}

uint32_t ain_fs_now(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return pack_systemtime(&st);
}

uint32_t ain_fs_dostime(const char *path)
{
    HANDLE file = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    FILETIME ft;
    SYSTEMTIME st, local;
    uint32_t result = 0;
    if (file == INVALID_HANDLE_VALUE) {
        win_error("read timestamp of", path);
        return 0;
    }
    if (GetFileTime(file, NULL, NULL, &ft) && FileTimeToSystemTime(&ft, &st) &&
        SystemTimeToTzSpecificLocalTime(NULL, &st, &local))
        result = pack_systemtime(&local);
    else
        win_error("read timestamp of", path);
    CloseHandle(file);
    return result;
}

int ain_fs_set_dostime(const char *path, uint32_t ts)
{
    SYSTEMTIME local = {0}, utc;
    FILETIME ft;
    HANDLE file;
    int rc;
    unsigned date = ts >> 16;
    if (!date)
        return 0;
    local.wYear = (WORD)(1980 + (date >> 9));
    local.wMonth = (WORD)((date >> 5) & 15);
    local.wDay = (WORD)(date & 31);
    local.wHour = (WORD)((ts >> 11) & 31);
    local.wMinute = (WORD)((ts >> 5) & 63);
    local.wSecond = (WORD)((ts & 31) * 2);
    if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &ft))
        return win_error("convert timestamp for", path);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return win_error("set timestamp of", path);
    rc = SetFileTime(file, NULL, NULL, &ft) ? 0 : win_error("set timestamp of", path);
    CloseHandle(file);
    return rc;
}

uint8_t ain_fs_dosattr(const char *path, int *isdir)
{
    DWORD attr = GetFileAttributesA(path);
    *isdir = 0;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        win_error("read attributes of", path);
        return 0x20;
    }
    *isdir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return (uint8_t)(0x20 | (attr & 1 ? 1 : 0) | (attr & 2 ? 2 : 0));
}

int ain_fs_set_hidden(const char *path)
{
    return SetFileAttributesA(path, FILE_ATTRIBUTE_HIDDEN) ? 0 : win_error("hide", path);
}

#else
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct AinDir {
    DIR *handle;
};

void ain_fs_init(void) {}

double ain_fs_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        fs_error("read elapsed-time clock", "");
        return -1;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

int ain_fs_probe(const char *path, int *isdir)
{
    struct stat st;
    *isdir = 0;
    if (stat(path, &st)) {
        if (errno == ENOENT || errno == ENOTDIR)
            return 0;
        return fs_error("inspect", path);
    }
    *isdir = S_ISDIR(st.st_mode);
    return 1;
}

AinDir *ain_fs_dir_open(const char *path)
{
    AinDir *dir = (AinDir *)calloc(1, sizeof *dir);
    if (!dir) {
        fs_error("allocate directory reader for", path);
        return NULL;
    }
    dir->handle = opendir(*path ? path : ".");
    if (!dir->handle) {
        fs_error("read directory", path);
        free(dir);
        return NULL;
    }
    return dir;
}

int ain_fs_dir_read(AinDir *dir, AinDirEntry *entry)
{
    struct dirent *item;
    struct stat st;
    errno = 0;
    item = readdir(dir->handle);
    if (!item)
        return errno ? fs_error("read directory", "") : 0;
    if (fstatat(dirfd(dir->handle), item->d_name, &st, AT_SYMLINK_NOFOLLOW))
        return fs_error("inspect directory entry", item->d_name);
    entry->symlink = S_ISLNK(st.st_mode);
    if (entry->symlink) {
        if (fstatat(dirfd(dir->handle), item->d_name, &st, 0))
            return fs_error("follow symlink", item->d_name);
    }
    entry->name = item->d_name;
    entry->isdir = S_ISDIR(st.st_mode);
    if (!entry->isdir && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: unsupported file type: %s\n", item->d_name);
        return -1;
    }
    return 1;
}

void ain_fs_dir_close(AinDir *dir)
{
    if (closedir(dir->handle))
        fs_error("close directory", "");
    free(dir);
}

int ain_fs_mkdir(const char *path)
{
    int isdir;
    if (!mkdir(path, 0777))
        return 0;
    if (errno == EEXIST && ain_fs_probe(path, &isdir) == 1 && isdir)
        return 1;
    return fs_error("create directory", path);
}

static uint32_t pack_localtime(time_t timestamp)
{
    struct tm local;
    if (!localtime_r(&timestamp, &local)) {
        fs_error("convert local time", "");
        return 0;
    }
    return pack_time((unsigned)(local.tm_year + 1900), (unsigned)(local.tm_mon + 1),
                     (unsigned)local.tm_mday, (unsigned)local.tm_hour,
                     (unsigned)local.tm_min, (unsigned)local.tm_sec);
}

uint32_t ain_fs_now(void)
{
    time_t timestamp = time(NULL);
    if (timestamp == (time_t)-1) {
        fs_error("read current time", "");
        return 0;
    }
    return pack_localtime(timestamp);
}

uint32_t ain_fs_dostime(const char *path)
{
    struct stat st;
    if (stat(path, &st)) {
        fs_error("read timestamp of", path);
        return 0;
    }
    return pack_localtime(st.st_mtime);
}

int ain_fs_set_dostime(const char *path, uint32_t ts)
{
    struct tm local = {0};
    struct timespec times[2];
    unsigned date = ts >> 16;
    if (!date)
        return 0;
    local.tm_year = (int)(1980 + (date >> 9)) - 1900;
    local.tm_mon = (int)((date >> 5) & 15) - 1;
    local.tm_mday = (int)(date & 31);
    local.tm_hour = (int)((ts >> 11) & 31);
    local.tm_min = (int)((ts >> 5) & 63);
    local.tm_sec = (int)((ts & 31) * 2);
    local.tm_isdst = -1;
    if (local.tm_mon < 0 || local.tm_mon > 11 || !local.tm_mday ||
        local.tm_mday > 31 || local.tm_hour > 23 || local.tm_min > 59 || local.tm_sec > 59) {
        errno = EINVAL;
        return fs_error("convert timestamp for", path);
    }
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = mktime(&local);
    times[1].tv_nsec = 0;
    if (times[1].tv_sec == (time_t)-1 ||
        pack_time((unsigned)(local.tm_year + 1900), (unsigned)(local.tm_mon + 1),
                  (unsigned)local.tm_mday, (unsigned)local.tm_hour,
                  (unsigned)local.tm_min, (unsigned)local.tm_sec) != ts) {
        errno = EINVAL;
        return fs_error("convert timestamp for", path);
    }
    if (utimensat(AT_FDCWD, path, times, 0))
        return fs_error("set timestamp of", path);
    return 0;
}

uint8_t ain_fs_dosattr(const char *path, int *isdir)
{
    struct stat st;
    const char *base = ain_fs_basename(path);
    *isdir = 0;
    if (stat(path, &st)) {
        fs_error("read attributes of", path);
        return 0x20;
    }
    *isdir = S_ISDIR(st.st_mode);
    return (uint8_t)(0x20 | (!(st.st_mode & S_IWUSR) ? 1 : 0) |
                     (base[0] == '.' ? 2 : 0));
}

/* POSIX has no DOS hidden bit. Do not rename extracted archive members. */
int ain_fs_set_hidden(const char *path)
{
    (void)path;
    return 0;
}
#endif
