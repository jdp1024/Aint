/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

#ifndef AIN_WCX_SETTINGS_H
#define AIN_WCX_SETTINGS_H
#include <windows.h>
typedef struct {
    int method;
    unsigned volume_kib, threads;
} WcxSettings;
extern HINSTANCE wcx_instance;
void wcx_settings_get(WcxSettings *settings);
int wcx_password(HWND parent, char password[256], int confirm);
#endif
