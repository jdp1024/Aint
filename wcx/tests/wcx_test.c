#define _CRT_SECURE_NO_WARNINGS
#include "..\wcx.h"
#include "..\resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    HANDLE (__stdcall *open)(tOpenArchiveData *);
    int (__stdcall *read)(HANDLE, tHeaderDataEx *);
    int (__stdcall *legacy)(HANDLE, tHeaderData *);
    int (__stdcall *process)(HANDLE, int, char *, char *);
    int (__stdcall *close)(HANDLE);
    int (__stdcall *pack)(char *, char *, char *, char *, int);
    int (__stdcall *remove)(char *, char *);
    void (__stdcall *progress)(HANDLE, tProcessDataProc);
    void (__stdcall *configure)(HWND, HINSTANCE);
    int (__stdcall *caps)(void);
    BOOL (__stdcall *detect)(char *);
} api;
static int failures, cancel_mode;
static unsigned long long progressed;
static char root[MAX_PATH], input[MAX_PATH], output[MAX_PATH];
static char all_files[] = "alpha.txt\0sub\\beta.bin\0empty.bin\0";
static HMODULE plugin_module;
static int expected_method = 3;

#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s (Windows error %lu)\n", __LINE__, #condition, GetLastError()); \
    ++failures; } } while (0)

static void path(char *dest, const char *dir, const char *name)
{
    snprintf(dest, MAX_PATH, "%s\\%s", dir, name);
}

static void write_file(const char *name, const void *data, size_t count)
{
    FILE *f = fopen(name, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(!count || fwrite(data, 1, count, f) == count);
        CHECK(!fclose(f));
    }
}

static int equal_files(const char *left, const char *right)
{
    FILE *a = fopen(left, "rb"), *b = fopen(right, "rb");
    int ok = a && b;
    if (ok) {
        int x, y;
        do {
            x = fgetc(a); y = fgetc(b);
            if (x != y) { ok = 0; break; }
        } while (x != EOF);
        if (ferror(a) || ferror(b))
            ok = 0;
    }
    if (a) fclose(a);
    if (b) fclose(b);
    return ok;
}

static void check_encrypted(const char *archive, int expected)
{
    unsigned char header[24];
    FILE *file = fopen(archive, "rb");
    CHECK(file != NULL);
    if (!file)
        return;
    CHECK(fread(header, 1, sizeof header, file) == sizeof header);
    CHECK(!!(header[3] & 0x80) == expected);
    CHECK(!fclose(file));
}

static int __stdcall progress(char *name, int size)
{
    if (cancel_mode == 1 || (cancel_mode == 2 && strstr(name, ".ain")) ||
        (cancel_mode == 3 && size > 0))
        return 0;
    if (size > 0)
        progressed += size;
    return 1;
}

typedef struct {
    int method, volume, threads, clicked;
} DialogInput;

static void check_centered(HWND dialog)
{
    MONITORINFO info = { sizeof info };
    RECT rect;
    CHECK(GetWindowRect(dialog, &rect));
    CHECK(GetMonitorInfo(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &info));
    CHECK(abs((rect.left + rect.right) - (info.rcWork.left + info.rcWork.right)) <= 1);
    CHECK(abs((rect.top + rect.bottom) - (info.rcWork.top + info.rcWork.bottom)) <= 1);
}

static BOOL CALLBACK find_dialog(HWND window, LPARAM arg)
{
    DialogInput *value = (DialogInput *)arg;
    char title[128];
    DWORD pid;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    GetWindowTextA(window, title, sizeof title);
    if (strcmp(title, "AIN packer configuration"))
        return TRUE;
    if (!IsWindowVisible(window))
        return TRUE;
    check_centered(window);
    CHECK(SendDlgItemMessageA(window, IDC_METHOD, CB_GETCURSEL, 0, 0) == expected_method - 1);
    CHECK(GetDlgItem(window, IDC_PASSWORD) == NULL);
    CHECK(GetDlgItem(window, IDC_PASSWORD_REPEAT) == NULL);
    SendDlgItemMessageA(window, IDC_METHOD, CB_SETCURSEL, value->method - 1, 0);
    SetDlgItemInt(window, IDC_VOLUME, value->volume, FALSE);
    SetDlgItemInt(window, IDC_THREADS, value->threads, FALSE);
    expected_method = value->method;
    value->clicked = 1;
    SendMessageA(window, WM_COMMAND, IDOK, 0);
    return FALSE;
}

static DWORD WINAPI dialog_worker(void *arg)
{
    DialogInput *value = (DialogInput *)arg;
    int i;
    for (i = 0; i < 500 && !value->clicked; ++i) {
        Sleep(20);
        EnumWindows(find_dialog, (LPARAM)value);
    }
    return 0;
}

static void configure(int method, int volume, int threads)
{
    DialogInput value = { method, volume, threads, 0 };
    HANDLE thread = CreateThread(NULL, 0, dialog_worker, &value, 0, NULL);
    CHECK(thread != NULL);
    if (!thread)
        return;
    api.configure(NULL, NULL);
    CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CHECK(value.clicked);
}

typedef struct {
    const char *password;
    int cancel, clicked, phase, confirm;
} PasswordInput;

static BOOL CALLBACK find_password(HWND window, LPARAM arg)
{
    PasswordInput *value = (PasswordInput *)arg;
    char title[128];
    DWORD pid;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    GetWindowTextA(window, title, sizeof title);
    if (!strcmp(title, "AIN password mismatch") && value->phase == 1) {
        value->phase = 2;
        SendMessageA(window, WM_COMMAND, IDOK, 0);
        return FALSE;
    }
    if (strcmp(title, "AIN archive password"))
        return TRUE;
    if (!IsWindowVisible(window) || value->phase == 1)
        return TRUE;
    check_centered(window);
    {
        int visible = IsWindowVisible(GetDlgItem(window, IDC_PASSWORD_REPEAT)) != 0;
        CHECK(visible == (IsWindowVisible(GetDlgItem(window, IDC_PASSWORD_REPEAT_LABEL)) != 0));
        if (value->confirm >= 0)
            CHECK(visible == value->confirm);
        if (!visible)
            value->phase = 3;
    }
    SetDlgItemTextA(window, IDC_PASSWORD, value->password);
    if (!value->phase && !value->cancel && *value->password) {
        SetDlgItemTextA(window, IDC_PASSWORD_REPEAT, "deliberately different");
        value->phase = 1;
        PostMessageA(window, WM_COMMAND, IDOK, 0);
        return FALSE;
    }
    SetDlgItemTextA(window, IDC_PASSWORD_REPEAT, value->password);
    value->clicked = 1;
    SendMessageA(window, WM_COMMAND, value->cancel ? IDCANCEL : IDOK, 0);
    return FALSE;
}

static DWORD WINAPI password_worker(void *arg)
{
    PasswordInput *value = (PasswordInput *)arg;
    int i;
    for (i = 0; i < 500 && !value->clicked; ++i) {
        Sleep(20);
        EnumWindows(find_password, (LPARAM)value);
    }
    return 0;
}

static volatile LONG stop_passwords;

static DWORD WINAPI password_responder(void *arg)
{
    PasswordInput value = { "test-secret", 0, 0, 0, -1 };
    (void)arg;
    while (!InterlockedCompareExchange(&stop_passwords, 0, 0)) {
        Sleep(20);
        if (value.clicked) {
            CHECK(value.phase == 2 || value.phase == 3);
            value.clicked = value.phase = 0;
        }
        EnumWindows(find_password, (LPARAM)&value);
    }
    return 0;
}

static HANDLE open_archive(char *archive)
{
    tOpenArchiveData data = { 0 };
    HANDLE handle;
    data.ArcName = archive;
    data.OpenMode = 1;
    handle = api.open(&data);
    CHECK(handle != NULL && data.OpenResult == 0);
    if (handle)
        api.progress(handle, progress);
    return handle;
}

static int check_archive(char *archive, int operation, int expected)
{
    HANDLE handle = open_archive(archive);
    tHeaderDataEx header;
    int count = 0, result = 0;
    if (!handle)
        return -1;
    while ((result = api.read(handle, &header)) == 0) {
        CHECK(header.UnpSizeHigh == 0 && header.FileCRC == 0);
        if (operation == PK_EXTRACT) {
            char source[MAX_PATH], extracted[MAX_PATH];
            CHECK(api.process(handle, operation, output, header.FileName) == 0);
            path(source, input, header.FileName);
            path(extracted, output, header.FileName);
            CHECK(equal_files(source, extracted));
        } else {
            CHECK(api.process(handle, operation, NULL, NULL) == 0);
        }
        if (++count > 100) { CHECK(0); break; }
    }
    CHECK(result == E_END_ARCHIVE);
    if (expected >= 0)
        CHECK(count == expected);
    CHECK(api.close(handle) == 0);
    return count;
}

static void remove_tree(const char *dir)
{
    WIN32_FIND_DATAA data;
    char name[MAX_PATH], pattern[MAX_PATH];
    HANDLE search;
    path(pattern, dir, "*");
    search = FindFirstFileA(pattern, &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
                continue;
            path(name, dir, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(name);
            else {
                SetFileAttributesA(name, FILE_ATTRIBUTE_NORMAL);
                CHECK(DeleteFileA(name));
            }
        } while (FindNextFileA(search, &data));
        FindClose(search);
    }
    CHECK(RemoveDirectoryA(dir));
}

static int load_api(void)
{
    char dll[MAX_PATH], *slash;
    HMODULE module;
    GetModuleFileNameA(NULL, dll, sizeof dll);
    slash = strrchr(dll, '\\');
#ifdef _WIN64
    strcpy(slash + 1, "ain.wcx64");
#else
    strcpy(slash + 1, "ain.wcx");
#endif
    module = LoadLibraryA(dll);
    plugin_module = module;
    CHECK(module != NULL);
    if (!module)
        return 0;
#define LOAD(field, name) do { \
    FARPROC proc = GetProcAddress(module, name); \
    CHECK(proc != NULL); \
    memcpy(&api.field, &proc, sizeof proc); \
    if (!proc) return 0; \
} while (0)
    LOAD(open, "OpenArchive");
    LOAD(read, "ReadHeaderEx");
    LOAD(legacy, "ReadHeader");
    LOAD(process, "ProcessFile");
    LOAD(close, "CloseArchive");
    LOAD(pack, "PackFiles");
    LOAD(remove, "DeleteFiles");
    LOAD(progress, "SetProcessDataProc");
    LOAD(configure, "ConfigurePacker");
    LOAD(caps, "GetPackerCaps");
    LOAD(detect, "CanYouHandleThisFile");
#undef LOAD
    CHECK(GetProcAddress(module, "SetChangeVolProc") != NULL);
    CHECK(GetProcAddress(module, "PackSetDefaultParams") != NULL);
    return 1;
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], file[MAX_PATH], archive[MAX_PATH], saved[MAX_PATH], volume[MAX_PATH];
    char profile[MAX_PATH], ini[MAX_PATH];
    unsigned char *data;
    unsigned random = 17;
    int i, method, encrypted;
    HANDLE handle;
    tHeaderData legacy;
    tHeaderDataEx header;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CHECK(GetTempPathA(sizeof temp, temp));
    CHECK(GetTempFileNameA(temp, "awt", 0, root));
    CHECK(DeleteFileA(root));
    CHECK(CreateDirectoryA(root, NULL));
    path(profile, root, "profile");
    CHECK(CreateDirectoryA(profile, NULL));
    CHECK(SetEnvironmentVariableA("APPDATA", profile));
    path(file, profile, "AinWCX");
    CHECK(CreateDirectoryA(file, NULL));
    path(ini, file, "settings.ini");
    CHECK(WritePrivateProfileStringA("AIN", "Method", "3", ini));
    if (!load_api())
        return 1;
    path(input, root, "input");
    path(output, root, "output");
    CHECK(CreateDirectoryA(input, NULL));
    CHECK(CreateDirectoryA(output, NULL));
    path(file, input, "sub");
    CHECK(CreateDirectoryA(file, NULL));
    data = (unsigned char *)malloc(300000);
    CHECK(data != NULL);
    if (!data)
        return 1;
    for (i = 0; i < 300000; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        data[i] = (unsigned char)random;
    }
    path(file, input, "sub\\beta.bin");
    write_file(file, data, 300000);
    memset(data, 'A', 180000);
    path(file, input, "alpha.txt");
    write_file(file, data, 180000);
    path(file, input, "empty.bin");
    write_file(file, "", 0);
    {
        int expected_caps = PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE |
                            PK_CAPS_DELETE | PK_CAPS_OPTIONS | PK_CAPS_BY_CONTENT |
                            PK_CAPS_ENCRYPT;
        CHECK((api.caps() & expected_caps) == expected_caps);
    }
    api.progress(INVALID_HANDLE_VALUE, progress);

    for (method = 1; method <= 4; ++method) {
        snprintf(temp, sizeof temp, "method%d.ain", method);
        path(archive, root, temp);
        configure(method, 0, 2);
        CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == 0);
        CHECK(api.detect(archive));
        progressed = 0;
        check_archive(archive, PK_EXTRACT, 3);
        CHECK(progressed == 480000);
        check_archive(archive, PK_TEST, 3);
        handle = open_archive(archive);
        if (handle) {
            CHECK(api.legacy(handle, &legacy) == 0 && !strcmp(legacy.FileName, "alpha.txt"));
            CHECK(api.process(handle, PK_SKIP, NULL, NULL) == 0);
            CHECK(api.close(handle) == 0);
        }
        printf("PASS method %d list/extract/test/legacy ABI\n", method);
    }

    path(archive, root, "method2.ain");
    path(file, input, "alpha.txt");
    write_file(file, "replacement", 11);
    configure(2, 0, 1);
    CHECK(api.pack(archive, NULL, input, "alpha.txt\0", PK_PACK_SAVE_PATHS) == 0);
    check_archive(archive, PK_EXTRACT, 3);
    CHECK(api.pack(archive, "prefix", input, "alpha.txt\0", PK_PACK_SAVE_PATHS) == 0);
    check_archive(archive, PK_SKIP, 4);
    CHECK(api.remove(archive, "prefix\\*\0") == 0);
    CHECK(api.pack(archive, NULL, input, "sub\\beta.bin\0", 0) == 0);
    check_archive(archive, PK_SKIP, 4);
    CHECK(api.remove(archive, "beta.bin\0") == 0);
    check_archive(archive, PK_TEST, 3);
    printf("PASS replacement, subpaths, flattening, deletion\n");

    for (encrypted = 0; encrypted <= 1; ++encrypted) {
        for (method = 1; method <= 4; ++method) {
            HANDLE responder = NULL;
            if (encrypted) {
                InterlockedExchange(&stop_passwords, 0);
                responder = CreateThread(NULL, 0, password_responder, NULL, 0, NULL);
                CHECK(responder != NULL);
            }
            snprintf(temp, sizeof temp, "split%d-%d.ain", encrypted, method);
            path(archive, root, temp);
            configure(method, 64, 2);
            CHECK(api.pack(archive, NULL, input, all_files,
                           PK_PACK_SAVE_PATHS | (encrypted ? PK_PACK_ENCRYPT : 0)) == 0);
            check_encrypted(archive, encrypted);
            check_archive(archive, PK_EXTRACT, 3);
            strcpy(volume, archive);
            strcpy(volume + strlen(volume) - 3, "001");
            CHECK(GetFileAttributesA(volume) != INVALID_FILE_ATTRIBUTES);
            for (i = 0; i < 999; ++i) {
                WIN32_FILE_ATTRIBUTE_DATA attr;
                snprintf(file, sizeof file, "%.*s.%03d", (int)strlen(archive) - 4, archive, i);
                if (!i)
                    strcpy(file, archive);
                if (!GetFileAttributesExA(file, GetFileExInfoStandard, &attr))
                    break;
                CHECK(attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= 65536);
            }
            CHECK(i > 1);
            configure(method, 32, 2);
            CHECK(api.pack(archive, NULL, input, "alpha.txt\0", PK_PACK_SAVE_PATHS) == 0);
            check_encrypted(archive, encrypted);
            check_archive(archive, PK_TEST, 3);
            configure(method, 0, 2);
            CHECK(api.pack(archive, NULL, input, "alpha.txt\0", PK_PACK_SAVE_PATHS) == 0);
            check_encrypted(archive, encrypted);
            CHECK(GetFileAttributesA(volume) == INVALID_FILE_ATTRIBUTES);
            check_archive(archive, PK_EXTRACT, 3);
            if (responder) {
                InterlockedExchange(&stop_passwords, 1);
                CHECK(WaitForSingleObject(responder, 15000) == WAIT_OBJECT_0);
                CloseHandle(responder);
            }
            printf("PASS method %d split, encryption %s, update-to-single\n",
                   method, encrypted ? "enabled" : "disabled");
        }
    }

    configure(2, 0, 2);
    CHECK(FreeLibrary(plugin_module));
    if (!load_api())
        return 1;
    api.progress(INVALID_HANDLE_VALUE, progress);
    path(archive, root, "reload-encrypted.ain");
    {
        PasswordInput value = { "", 1, 0, 0, 1 };
        HANDLE thread = CreateThread(NULL, 0, password_worker, &value, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS | PK_PACK_ENCRYPT) == E_EABORTED);
            CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && value.clicked);
            CloseHandle(thread);
            CHECK(GetFileAttributesA(archive) == INVALID_FILE_ATTRIBUTES);
        }
    }
    {
        PasswordInput value = { "", 0, 0, 0, 1 };
        HANDLE thread = CreateThread(NULL, 0, password_worker, &value, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS | PK_PACK_ENCRYPT) == E_BAD_DATA);
            CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && value.clicked);
            CloseHandle(thread);
            CHECK(GetFileAttributesA(archive) == INVALID_FILE_ATTRIBUTES);
        }
    }
    {
        PasswordInput value = { "test-secret", 0, 0, 0, 1 };
        HANDLE thread = CreateThread(NULL, 0, password_worker, &value, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS | PK_PACK_ENCRYPT) == 0);
            CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && value.clicked);
            CloseHandle(thread);
            check_encrypted(archive, 1);
        }
    }
    path(archive, root, "plain-after-password.ain");
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == 0);
    check_encrypted(archive, 0);
    printf("PASS per-operation encryption, confirmation and no cached configuration password\n");

    path(archive, root, "split1-2.ain");
    configure(2, 0, 2);
    handle = open_archive(archive);
    if (handle) {
        PasswordInput value = { "test-secret", 0, 0, 0, 0 };
        HANDLE thread;
        CHECK(api.read(handle, &header) == 0);
        thread = CreateThread(NULL, 0, password_worker, &value, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            CHECK(api.process(handle, PK_TEST, NULL, NULL) == 0);
            CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && value.clicked);
            CloseHandle(thread);
        }
        CHECK(api.close(handle) == 0);
    }
    handle = open_archive(archive);
    if (handle) {
        PasswordInput value = { "", 1, 0, 0, 0 };
        HANDLE thread;
        CHECK(api.read(handle, &header) == 0);
        thread = CreateThread(NULL, 0, password_worker, &value, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            CHECK(api.process(handle, PK_TEST, NULL, NULL) == E_EABORTED);
            CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && value.clicked);
            CloseHandle(thread);
        }
        CHECK(api.close(handle) == 0);
    }
    printf("PASS password prompt entry and cancellation\n");

    configure(2, 0, 0);
    path(archive, root, "method2.ain");
    path(saved, root, "saved.ain");
    CHECK(CopyFileA(archive, saved, FALSE));
    cancel_mode = 1;
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == E_EABORTED);
    CHECK(equal_files(archive, saved));
    cancel_mode = 2;
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == E_EABORTED);
    CHECK(equal_files(archive, saved));
    cancel_mode = 0;
    handle = open_archive(archive);
    if (handle) {
        path(file, output, "protected.txt");
        write_file(file, "keep", 4);
        CHECK(api.read(handle, &header) == 0);
        cancel_mode = 1;
        CHECK(api.process(handle, PK_EXTRACT, output, "protected.txt") == E_EABORTED);
        cancel_mode = 0;
        path(temp, root, "protected-copy.txt");
        CHECK(CopyFileA(file, temp, FALSE));
        cancel_mode = 3;
        CHECK(api.process(handle, PK_EXTRACT, output, "protected.txt") == E_EABORTED);
        cancel_mode = 0;
        CHECK(equal_files(file, temp));
        CHECK(api.process(handle, PK_EXTRACT, output, "..\\escape.txt") == E_ECREATE);
        CHECK(api.close(handle) == 0);
    }
    CHECK(api.pack(archive, NULL, input, "missing.bin\0", 0) == E_EREAD);
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_MOVE_FILES) == E_NOT_SUPPORTED);
    CHECK(equal_files(archive, saved));
    configure(4, 64, 1);
    strcpy(volume, archive);
    strcpy(volume + strlen(volume) - 3, "001");
    write_file(volume, "unrelated", 9);
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == E_EWRITE);
    CHECK(equal_files(archive, saved));
    CHECK(DeleteFileA(volume));
    printf("PASS cancellation, error paths, destination safety, multi-volume rollback\n");

    configure(4, 0, 1);
    CHECK(api.remove(archive, "*\0") == 0);
    check_archive(archive, PK_SKIP, 0);
    CHECK(api.pack(archive, NULL, input, all_files, PK_PACK_SAVE_PATHS) == 0);
    {
        FILE *f = fopen(archive, "r+b");
        CHECK(f != NULL);
        if (f) {
            int byte;
            CHECK(!fseek(f, 24, SEEK_SET));
            byte = fgetc(f);
            CHECK(!fseek(f, 24, SEEK_SET));
            CHECK(fputc(byte ^ 1, f) != EOF);
            CHECK(!fclose(f));
        }
        handle = open_archive(archive);
        if (handle) {
            CHECK(api.read(handle, &header) == 0);
            CHECK(api.process(handle, PK_TEST, NULL, NULL) == E_BAD_DATA);
            CHECK(api.close(handle) == 0);
        }
    }
    path(ini, profile, "AinWCX\\settings.ini");
    CHECK(GetPrivateProfileIntA("AIN", "Method", 0, ini) == 4);
    CHECK(GetPrivateProfileIntA("AIN", "Threads", 9, ini) == 1);
    CHECK(GetPrivateProfileIntA("AIN", "RequirePassword", 9, ini) == 9);
    CHECK(GetPrivateProfileStringA("AIN", "Password", "", temp, sizeof temp, ini) == 0);
    CHECK(FreeLibrary(plugin_module));
    if (!load_api())
        return 1;
    configure(4, 0, 1);
    printf("PASS empty archive, checksums, centered dialogs, password confirmation, persisted compression\n");
    for (i = 1; i < argc; ++i) {
        CHECK(api.detect(argv[i]));
        check_archive(argv[i], PK_TEST, -1);
        printf("PASS legacy fixture %s\n", argv[i]);
    }
    free(data);
    if (!failures)
        remove_tree(root);
    else
        printf("Test files retained: %s\n", root);
    printf("%s (%d failures)\n", failures ? "WCX TESTS FAILED" : "WCX ALL OK", failures);
    return failures ? 1 : 0;
}
