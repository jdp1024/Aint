/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

#define _CRT_SECURE_NO_WARNINGS
#include "wcx.h"
#include "settings.h"
#include "resource.h"
#include <stdio.h>
#include <string.h>

HINSTANCE wcx_instance;
static SRWLOCK settings_lock = SRWLOCK_INIT;
static WcxSettings settings = { 2, 0, 0 };
static char ini_path[MAX_PATH];
static int loaded;

static int center_dialog(HWND dialog)
{
    HWND owner = GetWindow(dialog, GW_OWNER);
    HMONITOR monitor = MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { sizeof info };
    RECT rect;
    if (!GetMonitorInfo(monitor, &info) || !GetWindowRect(dialog, &rect))
        return 0;
    return SetWindowPos(dialog, NULL,
                        info.rcWork.left + ((info.rcWork.right - info.rcWork.left) -
                                            (rect.right - rect.left)) / 2,
                        info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) -
                                           (rect.bottom - rect.top)) / 2,
                        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void load_settings(void)
{
    char dir[MAX_PATH];
    DWORD len;
    if (loaded)
        return;
    loaded = 1;
    len = GetEnvironmentVariableA("APPDATA", dir, sizeof dir);
    if (!len || len >= sizeof dir ||
        snprintf(ini_path, sizeof ini_path, "%s\\AinWCX\\settings.ini", dir) >= sizeof ini_path) {
        ini_path[0] = 0;
        return;
    }
    settings.method = GetPrivateProfileIntA("AIN", "Method", 2, ini_path);
    settings.volume_kib = GetPrivateProfileIntA("AIN", "VolumeKiB", 0, ini_path);
    settings.threads = GetPrivateProfileIntA("AIN", "Threads", 0, ini_path);
    if (settings.method < 1 || settings.method > 4)
        settings.method = 2;
    if (settings.volume_kib > 16383)
        settings.volume_kib = 0;
    if (settings.threads > 256)
        settings.threads = 0;
}

void wcx_settings_get(WcxSettings *out)
{
    AcquireSRWLockExclusive(&settings_lock);
    load_settings();
    *out = settings;
    ReleaseSRWLockExclusive(&settings_lock);
}

static int save_settings(const WcxSettings *value)
{
    char dir[MAX_PATH], text[32], *slash;
    if (!ini_path[0])
        return 0;
    strcpy(dir, ini_path);
    slash = strrchr(dir, '\\');
    *slash = 0;
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    snprintf(text, sizeof text, "%d", value->method);
    if (!WritePrivateProfileStringA("AIN", "Method", text, ini_path))
        return 0;
    snprintf(text, sizeof text, "%u", value->volume_kib);
    if (!WritePrivateProfileStringA("AIN", "VolumeKiB", text, ini_path))
        return 0;
    snprintf(text, sizeof text, "%u", value->threads);
    if (!WritePrivateProfileStringA("AIN", "Threads", text, ini_path))
        return 0;
    return WritePrivateProfileStringA("AIN", "RequirePassword", NULL, ini_path);
}

static INT_PTR CALLBACK config_dialog(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    WcxSettings *value = (WcxSettings *)GetWindowLongPtr(dialog, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        value = (WcxSettings *)lp;
        SetWindowLongPtr(dialog, DWLP_USER, lp);
        SendDlgItemMessageA(dialog, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"1 - Best (slow)");
        SendDlgItemMessageA(dialog, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"2 - Normal");
        SendDlgItemMessageA(dialog, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"3 - Fast");
        SendDlgItemMessageA(dialog, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"4 - Store (uncompressed)");
        SendDlgItemMessage(dialog, IDC_METHOD, CB_SETCURSEL, value->method - 1, 0);
        SetDlgItemInt(dialog, IDC_VOLUME, value->volume_kib, FALSE);
        SetDlgItemInt(dialog, IDC_THREADS, value->threads, FALSE);
        if (!center_dialog(dialog))
            MessageBoxA(dialog, "Cannot center the AIN configuration dialog.", "AIN",
                        MB_OK | MB_ICONERROR);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        BOOL valid_volume, valid_threads;
        unsigned volume = GetDlgItemInt(dialog, IDC_VOLUME, &valid_volume, FALSE);
        unsigned threads = GetDlgItemInt(dialog, IDC_THREADS, &valid_threads, FALSE);
        int method = (int)SendDlgItemMessage(dialog, IDC_METHOD, CB_GETCURSEL, 0, 0) + 1;
        if (!valid_volume || volume > 16383 || !valid_threads || threads > 256 ||
            method < 1 || method > 4) {
            MessageBoxA(dialog, "Choose a method, a volume size of 0..16383 KiB, and 0..256 threads.",
                        "Invalid AIN settings", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        value->method = method;
        value->volume_kib = volume;
        value->threads = threads;
        AcquireSRWLockExclusive(&settings_lock);
        if (!save_settings(value)) {
            ReleaseSRWLockExclusive(&settings_lock);
            MessageBoxA(dialog, "Cannot save AIN settings to the user profile.",
                        "AIN settings error", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        settings = *value;
        ReleaseSRWLockExclusive(&settings_lock);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void __stdcall ConfigurePacker(HWND parent, HINSTANCE instance)
{
    WcxSettings value;
    INT_PTR result;
    (void)instance;
    wcx_settings_get(&value);
    result = DialogBoxParamA(wcx_instance, MAKEINTRESOURCEA(IDD_CONFIG), parent,
                             config_dialog, (LPARAM)&value);
    SecureZeroMemory(&value, sizeof value);
    if (result == -1)
        MessageBoxA(parent, "Cannot open the AIN configuration dialog.", "AIN", MB_OK | MB_ICONERROR);
}

typedef struct {
    char *password;
    int confirm;
} PasswordDialog;

static INT_PTR CALLBACK password_dialog(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtr(dialog, DWLP_USER, lp);
        SendDlgItemMessage(dialog, IDC_PASSWORD, EM_SETLIMITTEXT, 255, 0);
        SendDlgItemMessage(dialog, IDC_PASSWORD_REPEAT, EM_SETLIMITTEXT, 255, 0);
        if (!((PasswordDialog *)lp)->confirm) {
            RECT offset = { 0, 0, 0, 39 }, rect;
            int buttons[] = { IDOK, IDCANCEL };
            unsigned i;
            ShowWindow(GetDlgItem(dialog, IDC_PASSWORD_REPEAT_LABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_PASSWORD_REPEAT), SW_HIDE);
            MapDialogRect(dialog, &offset);
            for (i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
                HWND button = GetDlgItem(dialog, buttons[i]);
                GetWindowRect(button, &rect);
                MapWindowPoints(NULL, dialog, (POINT *)&rect, 2);
                SetWindowPos(button, NULL, rect.left, rect.top - offset.bottom,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            GetWindowRect(dialog, &rect);
            SetWindowPos(dialog, NULL, 0, 0, rect.right - rect.left,
                         rect.bottom - rect.top - offset.bottom,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (!center_dialog(dialog))
            MessageBoxA(dialog, "Cannot center the AIN password dialog.", "AIN",
                        MB_OK | MB_ICONERROR);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        PasswordDialog *value = (PasswordDialog *)GetWindowLongPtr(dialog, DWLP_USER);
        char entered[256] = { 0 }, repeat[256] = { 0 };
        int matches;
        GetDlgItemTextA(dialog, IDC_PASSWORD, entered, sizeof entered);
        GetDlgItemTextA(dialog, IDC_PASSWORD_REPEAT, repeat, sizeof repeat);
        matches = !value->confirm || !strcmp(entered, repeat);
        SecureZeroMemory(repeat, sizeof repeat);
        if (!matches) {
            SecureZeroMemory(entered, sizeof entered);
            MessageBoxA(dialog, "The passwords do not match. Please enter them again.",
                        "AIN password mismatch", MB_OK | MB_ICONERROR);
            SetFocus(GetDlgItem(dialog, IDC_PASSWORD_REPEAT));
            SendDlgItemMessage(dialog, IDC_PASSWORD_REPEAT, EM_SETSEL, 0, -1);
            return TRUE;
        }
        memcpy(value->password, entered, sizeof entered);
        SecureZeroMemory(entered, sizeof entered);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

int wcx_password(HWND parent, char password[256], int confirm)
{
    PasswordDialog value = { password, confirm };
    INT_PTR result = DialogBoxParamA(wcx_instance, MAKEINTRESOURCEA(IDD_PASSWORD), parent,
                                     password_dialog, (LPARAM)&value);
    if (result == -1)
        MessageBoxA(parent, "Cannot open the AIN password dialog.", "AIN", MB_OK | MB_ICONERROR);
    return result == IDOK;
}

void __stdcall PackSetDefaultParams(PackDefaultParamStruct *params)
{
    /* A user-writable profile is used instead of the plugin installation directory. */
    (void)params;
}
