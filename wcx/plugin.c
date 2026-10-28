/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

#define _CRT_SECURE_NO_WARNINGS
#include "wcx.h"
#include "settings.h"
#include "..\src\ain.h"
#include "..\src\fs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define PATH_CAP 1024
typedef struct {
    AinArchive archive;
    char path[PATH_CAP];
    int current, loaded;
    AinEnt *entries;
    int count;
    tProcessDataProc progress;
} WcxArchive;

static __declspec(thread) tProcessDataProc pack_progress;
static SRWLOCK rewrite_lock = SRWLOCK_INIT;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wcx_instance = instance;
        ain_quiet_level = 2;
    }
    return TRUE;
}

static int copy_text(char *dest, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        return 0;
    memmove(dest, src, n + 1);
    return 1;
}

static int join_path(char *dest, const char *dir, const char *name)
{
    size_t n = dir ? strlen(dir) : 0;
    return snprintf(dest, PATH_CAP, "%s%s%s", dir ? dir : "",
                    n && dir[n - 1] != '\\' && dir[n - 1] != '/' ? "\\" : "",
                    name ? name : "") < PATH_CAP;
}

/* Present DOS absolute members as relative paths; never expose traversal or ADS. */
static int member_name(char dest[PATH_CAP], const char *name)
{
    char *p, *start;
    if (((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':')
        name += 2;
    while (*name == '\\' || *name == '/')
        ++name;
    if (!*name || !copy_text(dest, PATH_CAP, name))
        return 0;
    for (p = dest; *p; ++p) {
        if (*p == '/')
            *p = '\\';
        if ((unsigned char)*p < 32 || strchr(":*?\"<>|", *p))
            return 0;
    }
    start = dest;
    for (p = dest;; ++p) {
        if (!*p || *p == '\\') {
            size_t n = (size_t)(p - start);
            char base[5] = { 0 };
            size_t i;
            if (!n || start[n - 1] == '.' || start[n - 1] == ' ')
                return 0;
            for (i = 0; i < n && i < 4 && start[i] != '.'; ++i)
                base[i] = start[i];
            if (!_stricmp(base, "CON") || !_stricmp(base, "PRN") ||
                !_stricmp(base, "AUX") || !_stricmp(base, "NUL") ||
                ((!_strnicmp(base, "COM", 3) || !_strnicmp(base, "LPT", 3)) &&
                 base[3] >= '1' && base[3] <= '9'))
                return 0;
            if (!*p)
                break;
            start = p + 1;
        }
    }
    return 1;
}

static int notify(tProcessDataProc callback, const char *name, int bytes)
{
    return !callback || callback((char *)name, bytes);
}

static int unlock(AinArchive *archive, char password[256])
{
    if (!archive->garbled || archive->gseed_ok)
        return 0;
    if (*password && !ain_set_password(archive, password))
        return 0;
    for (;;) {
        SecureZeroMemory(password, 256);
        if (!wcx_password(GetActiveWindow(), password, 0))
            return E_EABORTED;
        if (!ain_set_password(archive, password))
            return 0;
        MessageBoxA(GetActiveWindow(), "Incorrect AIN password.", "AIN", MB_OK | MB_ICONERROR);
    }
}

static int open_core(AinArchive *archive, const char *path, char password[256])
{
    int result, i;
    char name[PATH_CAP];
    if (ain_open(archive, path)) {
        ain_close(archive);
        return E_BAD_ARCHIVE;
    }
    if (ain_header_check(archive)) {
        ain_close(archive);
        return E_BAD_DATA;
    }
    /* Native split archives must be decoded to combine their member fragments. */
    if (archive->garbled && (archive->img[3] & AIN_FLAG_MORE_VOLUMES) &&
        archive->img[6] == 0 && archive->img[7] == 0) {
        result = unlock(archive, password);
        if (result) {
            ain_close(archive);
            return result;
        }
    }
    if (ain_load_index(archive)) {
        ain_close(archive);
        return E_BAD_DATA;
    }
    for (i = 0; i < archive->nrecs; ++i) {
        if (!member_name(name, archive->recs[i].name)) {
            ain_close(archive);
            return E_BAD_DATA;
        }
    }
    return 0;
}

static int checksums(const AinArchive *archive)
{
    const unsigned char *image = archive->enc ? archive->enc : archive->img;
    int i, start = 0;
    for (i = 0; i < archive->nrecs; ++i) {
        const AinRec *r = &archive->recs[i];
        size_t off, count, j;
        unsigned sum = 0;
        if (r->f22 & AIN_F_SPAN_START)
            start = i;
        if (!(r->f22 & AIN_F_SPAN_END) && (r->f22 & 0x18))
            continue;
        off = (r->f22 & AIN_F_SPAN_END) ? archive->recs[start].off : r->off;
        if (r->f22 == AIN_F_STANDALONE)
            off = r->off;
        count = r->comp;
        if (off > archive->n || count > archive->n - off)
            return 0;
        for (j = 0; j < count; ++j)
            sum += image[off + j];
        if ((sum & 0xffff) != r->chk)
            return 0;
    }
    return 1;
}

HANDLE __stdcall OpenArchive(tOpenArchiveData *data)
{
    WcxArchive *handle;
    char password[256] = { 0 };
    if (!data)
        return NULL;
    data->OpenResult = E_BAD_ARCHIVE;
    if (!data->ArcName || (data->OpenMode != 0 && data->OpenMode != 1))
        return NULL;
    handle = (WcxArchive *)calloc(1, sizeof *handle);
    if (!handle) {
        data->OpenResult = E_NO_MEMORY;
        return NULL;
    }
    if (!copy_text(handle->path, sizeof handle->path, data->ArcName)) {
        free(handle);
        data->OpenResult = E_SMALL_BUF;
        return NULL;
    }
    data->OpenResult = open_core(&handle->archive, handle->path, password);
    SecureZeroMemory(password, sizeof password);
    if (data->OpenResult) {
        free(handle);
        return NULL;
    }
    return (HANDLE)handle;
}

int __stdcall ReadHeaderEx(HANDLE archive, tHeaderDataEx *header)
{
    WcxArchive *handle = (WcxArchive *)archive;
    const AinRec *r;
    if (!handle || !header)
        return E_BAD_ARCHIVE;
    if (handle->current >= handle->archive.nrecs)
        return E_END_ARCHIVE;
    memset(header, 0, sizeof *header);
    r = &handle->archive.recs[handle->current];
    if (!copy_text(header->ArcName, sizeof header->ArcName, handle->path) ||
        !member_name(header->FileName, r->name))
        return E_BAD_DATA;
    header->PackSize = r->comp;
    header->UnpSize = r->orig;
    header->FileTime = (int)r->ts;
    header->FileAttr = r->attr;
    header->Method = handle->archive.native_volumes ?
                     handle->archive.native_method : handle->archive.method;
    header->Flags = handle->archive.garbled ? 1 : 0;
    /* AIN has a stored-byte sum, not a per-file CRC32. */
    return 0;
}

int __stdcall ReadHeader(HANDLE archive, tHeaderData *header)
{
    tHeaderDataEx extended;
    int result;
    if (!header)
        return E_BAD_ARCHIVE;
    result = ReadHeaderEx(archive, &extended);
    if (result)
        return result;
    memset(header, 0, sizeof *header);
    if (!copy_text(header->ArcName, sizeof header->ArcName, extended.ArcName) ||
        !copy_text(header->FileName, sizeof header->FileName, extended.FileName))
        return E_SMALL_BUF;
    header->Flags = extended.Flags;
    header->PackSize = (int)extended.PackSize;
    header->UnpSize = (int)extended.UnpSize;
    header->FileTime = extended.FileTime;
    header->FileAttr = extended.FileAttr;
    header->Method = extended.Method;
    return 0;
}

static int load_data(WcxArchive *handle)
{
    char password[256] = { 0 };
    int result;
    if (handle->loaded)
        return 0;
    result = unlock(&handle->archive, password);
    SecureZeroMemory(password, sizeof password);
    if (result)
        return result;
    if (!checksums(&handle->archive) ||
        ain_load_entries(&handle->archive, &handle->entries, &handle->count))
        return E_BAD_DATA;
    handle->loaded = 1;
    return 0;
}

static int make_parents(char *path)
{
    char *p = path;
    if (path[0] == '\\' && path[1] == '\\') {
        p = strchr(path + 2, '\\');
        if (!p || !(p = strchr(p + 1, '\\')))
            return 0;
        ++p;
    }
    for (; *p; ++p) {
        if ((*p == '\\' || *p == '/') && p != path && p[-1] != ':') {
            DWORD attr;
            char saved = *p;
            *p = 0;
            attr = GetFileAttributesA(path);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryA(path, NULL)) {
                    *p = saved;
                    return 0;
                }
            } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY) ||
                       (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                *p = saved;
                return 0;
            }
            *p = saved;
        }
    }
    return 1;
}

int __stdcall ProcessFile(HANDLE archive, int operation, char *dest_path, char *dest_name)
{
    WcxArchive *handle = (WcxArchive *)archive;
    AinEnt *entry;
    char path[PATH_CAP], name[PATH_CAP], temporary[PATH_CAP], parent[PATH_CAP], *slash;
    HANDLE output;
    size_t pos = 0;
    int result;
    if (!handle || handle->current >= handle->archive.nrecs)
        return E_BAD_ARCHIVE;
    if (operation == PK_SKIP) {
        ++handle->current;
        return 0;
    }
    if (operation != PK_TEST && operation != PK_EXTRACT)
        return E_NOT_SUPPORTED;
    if (!notify(handle->progress, handle->archive.recs[handle->current].name, 0))
        return E_EABORTED;
    result = load_data(handle);
    if (result)
        return result;
    entry = &handle->entries[handle->current];
    if (operation == PK_TEST) {
        while (pos < entry->n) {
            size_t chunk = entry->n - pos > 65536 ? 65536 : entry->n - pos;
            if (!notify(handle->progress, entry->name, (int)chunk))
                return E_EABORTED;
            pos += chunk;
        }
        ++handle->current;
        return 0;
    }
    if (!member_name(name, entry->name))
        return E_BAD_DATA;
    if (!dest_name)
        dest_name = name;
    if (dest_path && *dest_path) {
        if (!member_name(name, dest_name) || strcmp(name, dest_name) ||
            !join_path(path, dest_path, name))
            return E_ECREATE;
    } else if (!copy_text(path, sizeof path, dest_name)) {
        return E_SMALL_BUF;
    }
    if (!make_parents(path) || !copy_text(parent, sizeof parent, path))
        return E_ECREATE;
    slash = strrchr(parent, '\\');
    if (!slash)
        slash = strrchr(parent, '/');
    if (slash)
        slash[1] = 0;
    else
        strcpy(parent, ".");
    if (!GetTempFileNameA(parent, "ain", 0, temporary))
        return E_ECREATE;
    output = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        DeleteFileA(temporary);
        return E_ECREATE;
    }
    result = 0;
    while (pos < entry->n) {
        DWORD written, chunk = (DWORD)(entry->n - pos > 65536 ? 65536 : entry->n - pos);
        if (!WriteFile(output, entry->data + pos, chunk, &written, NULL) || written != chunk) {
            result = E_EWRITE;
            break;
        }
        pos += chunk;
        if (!notify(handle->progress, entry->name, (int)chunk)) {
            result = E_EABORTED;
            break;
        }
    }
    if (!result && entry->ts) {
        FILETIME local, utc;
        if (!DosDateTimeToFileTime((WORD)(entry->ts >> 16), (WORD)entry->ts, &local) ||
            !LocalFileTimeToFileTime(&local, &utc) ||
            !SetFileTime(output, NULL, NULL, &utc))
            result = E_EWRITE;
    }
    if (!CloseHandle(output) && !result)
        result = E_ECLOSE;
    if (!result && !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = E_EWRITE;
    if (result) {
        DeleteFileA(temporary);
        return result;
    }
    if (!SetFileAttributesA(path, (entry->attr & (FILE_ATTRIBUTE_READONLY |
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)) ?
        entry->attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                       FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE) : FILE_ATTRIBUTE_NORMAL))
        return E_EWRITE;
    ++handle->current;
    return 0;
}

int __stdcall CloseArchive(HANDLE archive)
{
    WcxArchive *handle = (WcxArchive *)archive;
    if (!handle)
        return E_BAD_ARCHIVE;
    ain_free_entries(handle->entries, handle->count);
    ain_close(&handle->archive);
    free(handle);
    return 0;
}

void __stdcall SetProcessDataProc(HANDLE archive, tProcessDataProc callback)
{
    if (!archive || archive == INVALID_HANDLE_VALUE)
        pack_progress = callback;
    else
        ((WcxArchive *)archive)->progress = callback;
}

void __stdcall SetChangeVolProc(HANDLE archive, tChangeVolProc callback)
{
    /* The core resolves sibling volumes itself, without interactive disk swapping. */
    (void)archive;
    (void)callback;
}

BOOL __stdcall CanYouHandleThisFile(char *name)
{
    unsigned char header[AIN_HDR_SIZE];
    unsigned sum = 0;
    size_t i;
    FILE *file;
    if (!name || !(file = fopen(name, "rb")))
        return FALSE;
    i = fread(header, 1, sizeof header, file);
    fclose(file);
    if (i != sizeof header || header[0] != AIN_MAGIC ||
        (header[1] & 15) < 1 || (header[1] & 15) > 4)
        return FALSE;
    for (i = 0; i < 22; ++i)
        sum += header[i];
    return (WORD)(sum ^ AIN_HDRXOR) == (WORD)(header[22] | (header[23] << 8));
}

int __stdcall GetPackerCaps(void)
{
    return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE |
           PK_CAPS_OPTIONS | PK_CAPS_BY_CONTENT | PK_CAPS_ENCRYPT;
}

typedef struct {
    char path[PATH_CAP];
    const AinEnt *entries;
    int count, result;
    WcxSettings settings;
    char password[256];
} WriteJob;

static DWORD WINAPI write_worker(void *arg)
{
    WriteJob *job = (WriteJob *)arg;
    job->result = ain_write_mt(job->path, job->settings.method, job->entries, job->count,
                              (unsigned long long)job->settings.volume_kib * 1024,
                              job->settings.method > 3 ? 2 : job->settings.method,
                              job->password, job->settings.threads);
    return 0;
}

static int volume_path(char *dest, const char *first, int volume)
{
    size_t n = strlen(first);
    if (!volume)
        return copy_text(dest, PATH_CAP, first);
    if (n < 4 || _stricmp(first + n - 4, ".ain"))
        return 0;
    return snprintf(dest, PATH_CAP, "%.*s.%03d", (int)n - 4, first, volume) < PATH_CAP;
}

static int remove_staging(const char *dir)
{
    WIN32_FIND_DATAA data;
    HANDLE search;
    char path[PATH_CAP];
    int ok = 1;
    if (!join_path(path, dir, "*"))
        return 0;
    search = FindFirstFileA(path, &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
                continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                !join_path(path, dir, data.cFileName) || !DeleteFileA(path))
                ok = 0;
        } while (FindNextFileA(search, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            ok = 0;
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        ok = 0;
    }
    return ok && RemoveDirectoryA(dir);
}

/* Rename a staged volume set, backing up every old volume before promotion. */
static int commit_archive(const char *archive, const char *staged, const char *backup,
                          int old_count, int new_count, int *keep)
{
    char source[PATH_CAP], dest[PATH_CAP];
    int backed = 0, promoted = 0, i, ok = 1;
    for (i = 0; i < old_count; ++i) {
        if (!volume_path(source, archive, i) || !volume_path(dest, backup, i) ||
            !MoveFileExA(source, dest, MOVEFILE_WRITE_THROUGH))
            goto rollback;
        ++backed;
    }
    for (i = 0; i < new_count; ++i) {
        if (!volume_path(source, staged, i) || !volume_path(dest, archive, i) ||
            !MoveFileExA(source, dest, MOVEFILE_WRITE_THROUGH))
            goto rollback;
        ++promoted;
    }
    return 0;
rollback:
    for (i = promoted - 1; i >= 0; --i) {
        volume_path(source, archive, i);
        volume_path(dest, staged, i);
        if (!MoveFileExA(source, dest, MOVEFILE_WRITE_THROUGH))
            ok = 0;
    }
    for (i = backed - 1; i >= 0; --i) {
        volume_path(source, backup, i);
        volume_path(dest, archive, i);
        if (!MoveFileExA(source, dest, MOVEFILE_WRITE_THROUGH))
            ok = 0;
    }
    if (!ok) {
        *keep = 1;
        MessageBoxA(GetActiveWindow(), backup,
                    "AIN recovery required: original volumes retained here",
                    MB_OK | MB_ICONERROR);
    }
    return E_EWRITE;
}

static int rewrite(char *archive, char *subpath, char *source, char *list, int flags, int deleting)
{
    AinArchive original;
    AinEnt *entries = NULL;
    WriteJob job;
    char full[PATH_CAP], parent[PATH_CAP], staging[PATH_CAP] = "", backup[PATH_CAP];
    char disk[PATH_CAP], member[PATH_CAP], combined[PATH_CAP], *slash, *item;
    int count = 0, old_count = 0, new_count = 0, result = 0, keep = 0, i;
    DWORD attr;
    HANDLE worker;
    if (!archive || !list || (!deleting && !source))
        return E_BAD_DATA;
    if (flags & ~(PK_PACK_SAVE_PATHS | PK_PACK_ENCRYPT))
        return E_NOT_SUPPORTED;
    /* Resolve from the supplied name, never change Total Commander's working directory. */
    {
        DWORD n = GetFullPathNameA(archive, sizeof full, full, NULL);
        if (!n || n >= sizeof full)
            return E_SMALL_BUF;
    }
    if (strlen(full) < 4 || _stricmp(full + strlen(full) - 4, ".ain"))
        return E_NOT_SUPPORTED;
    memset(&job, 0, sizeof job);
    wcx_settings_get(&job.settings);
    attr = GetFileAttributesA(full);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        result = open_core(&original, full, job.password);
        if (result)
            goto done;
        old_count = original.nvols;
        result = unlock(&original, job.password);
        if (!result && (!checksums(&original) || ain_load_entries(&original, &entries, &count)))
            result = E_BAD_DATA;
        ain_close(&original);
        if (result)
            goto done;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        result = E_EOPEN;
        goto done;
    } else if (deleting) {
        result = E_EOPEN;
        goto done;
    }
    if ((flags & PK_PACK_ENCRYPT) && !*job.password &&
        !wcx_password(GetActiveWindow(), job.password, 1)) {
        result = E_EABORTED;
        goto done;
    }
    if ((flags & PK_PACK_ENCRYPT) && !*job.password) {
        result = E_BAD_DATA;
        goto done;
    }
    for (item = list; *item; item += strlen(item) + 1) {
        AinEnt added;
        char *new_name;
        size_t len = strlen(item);
        if (!notify(pack_progress, item, 0)) { result = E_EABORTED; goto done; }
        if (deleting) {
            for (i = count - 1; i >= 0; --i) {
                if (ain_match(item, entries[i].name)) {
                    free(entries[i].name);
                    free(entries[i].data);
                    memmove(entries + i, entries + i + 1, (size_t)(count - i - 1) * sizeof *entries);
                    --count;
                }
            }
            continue;
        }
        if (item[len - 1] == '\\' || item[len - 1] == '/')
            continue; /* AIN represents files, not empty directories. */
        if (!member_name(member, item) || strcmp(member, item) ||
            !join_path(disk, source, item)) {
            result = E_BAD_DATA;
            goto done;
        }
        if (!(flags & PK_PACK_SAVE_PATHS))
            copy_text(member, sizeof member, ain_fs_basename(item));
        if (!join_path(combined, subpath, member) || !member_name(member, combined)) {
            result = E_BAD_DATA;
            goto done;
        }
        memset(&added, 0, sizeof added);
        if (ain_read_file(disk, &added)) { result = E_EREAD; goto done; }
        new_name = _strdup(member);
        if (!new_name) {
            free(added.name); free(added.data);
            result = E_NO_MEMORY;
            goto done;
        }
        free(added.name);
        added.name = new_name;
        for (i = 0; i < count; ++i) {
            char existing[PATH_CAP];
            if (!member_name(existing, entries[i].name)) {
                free(added.name); free(added.data);
                result = E_BAD_DATA;
                goto done;
            }
            if (ain_fs_same_member(existing, member))
                break;
        }
        if (i == count) {
            AinEnt *grown;
            if (count == 65535) {
                free(added.name); free(added.data);
                result = E_NOT_SUPPORTED;
                goto done;
            }
            grown = (AinEnt *)realloc(entries, (size_t)(count + 1) * sizeof *entries);
            if (!grown) {
                free(added.name); free(added.data);
                result = E_NO_MEMORY;
                goto done;
            }
            entries = grown;
            ++count;
        } else {
            free(entries[i].name);
            free(entries[i].data);
        }
        entries[i] = added;
        {
            size_t pos = 0;
            while (pos < added.n) {
                int chunk = (int)(added.n - pos > 65536 ? 65536 : added.n - pos);
                if (!notify(pack_progress, item, chunk)) { result = E_EABORTED; goto done; }
                pos += chunk;
            }
        }
    }
    strcpy(parent, full);
    slash = strrchr(parent, '\\');
    if (!slash) { result = E_ECREATE; goto done; }
    slash[1] = 0;
    if (!GetTempFileNameA(parent, "ain", 0, staging)) { result = E_ECREATE; goto done; }
    if (!DeleteFileA(staging) || !CreateDirectoryA(staging, NULL)) {
        staging[0] = 0;
        result = E_ECREATE;
        goto done;
    }
    if (!join_path(job.path, staging, "new.ain") || !join_path(backup, staging, "old.ain")) {
        result = E_SMALL_BUF;
        goto done;
    }
    job.entries = entries;
    job.count = count;
    worker = CreateThread(NULL, 0, write_worker, &job, 0, NULL);
    if (!worker) { result = E_NO_MEMORY; goto done; }
    for (;;) {
        DWORD wait = WaitForSingleObject(worker, 100);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait != WAIT_TIMEOUT) {
            result = E_EWRITE;
            /* The worker owns stack-backed job data until it has exited. */
            WaitForSingleObject(worker, INFINITE);
            break;
        }
        if (!result && !notify(pack_progress, full, 0))
            result = E_EABORTED;
    }
    CloseHandle(worker);
    if (result)
        goto done;
    if (job.result) { result = E_EWRITE; goto done; }
    if (!notify(pack_progress, full, 0)) { result = E_EABORTED; goto done; }
    for (i = 0; i < 999; ++i) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        volume_path(disk, job.path, i);
        if (!GetFileAttributesExA(disk, GetFileExInfoStandard, &info)) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) { result = E_EREAD; goto done; }
            break;
        }
        if (info.nFileSizeHigh || info.nFileSizeLow > LONG_MAX) {
            result = E_NOT_SUPPORTED;
            goto done;
        }
        ++new_count;
    }
    if (!new_count) { result = E_EWRITE; goto done; }
    result = commit_archive(full, job.path, backup, old_count, new_count, &keep);
done:
    ain_free_entries(entries, count);
    SecureZeroMemory(job.password, sizeof job.password);
    if (*staging && !keep && !remove_staging(staging)) {
        MessageBoxA(GetActiveWindow(), staging, "AIN: cannot remove temporary files here",
                    MB_OK | MB_ICONWARNING);
        if (!result)
            result = E_EWRITE;
    }
    return result;
}

int __stdcall PackFiles(char *archive, char *subpath, char *source, char *list, int flags)
{
    int result;
    AcquireSRWLockExclusive(&rewrite_lock);
    result = rewrite(archive, subpath, source, list, flags, 0);
    ReleaseSRWLockExclusive(&rewrite_lock);
    return result;
}

int __stdcall DeleteFiles(char *archive, char *list)
{
    int result;
    AcquireSRWLockExclusive(&rewrite_lock);
    result = rewrite(archive, NULL, NULL, list, 0, 1);
    ReleaseSRWLockExclusive(&rewrite_lock);
    return result;
}
