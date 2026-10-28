/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* ANSI Total Commander packer ABI. Use default Windows structure alignment. */
#ifndef AIN_WCX_H
#define AIN_WCX_H
#include <windows.h>

#define E_END_ARCHIVE 10
#define E_NO_MEMORY 11
#define E_BAD_DATA 12
#define E_BAD_ARCHIVE 13
#define E_UNKNOWN_FORMAT 14
#define E_EOPEN 15
#define E_ECREATE 16
#define E_ECLOSE 17
#define E_EREAD 18
#define E_EWRITE 19
#define E_SMALL_BUF 20
#define E_EABORTED 21
#define E_NOT_SUPPORTED 24
#define PK_SKIP 0
#define PK_TEST 1
#define PK_EXTRACT 2
#define PK_PACK_MOVE_FILES 1
#define PK_PACK_SAVE_PATHS 2
#define PK_PACK_ENCRYPT 4
#define PK_CAPS_NEW 1
#define PK_CAPS_MODIFY 2
#define PK_CAPS_MULTIPLE 4
#define PK_CAPS_DELETE 8
#define PK_CAPS_OPTIONS 16
#define PK_CAPS_BY_CONTENT 64
#define PK_CAPS_ENCRYPT 512

typedef struct {
    char ArcName[260], FileName[260];
    int Flags, PackSize, UnpSize, HostOS, FileCRC, FileTime, UnpVer, Method, FileAttr;
    char *CmtBuf;
    int CmtBufSize, CmtSize, CmtState;
} tHeaderData;

typedef struct {
    char ArcName[1024], FileName[1024];
    int Flags;
    unsigned PackSize, PackSizeHigh, UnpSize, UnpSizeHigh;
    int HostOS, FileCRC, FileTime, UnpVer, Method, FileAttr;
    char *CmtBuf;
    int CmtBufSize, CmtSize, CmtState;
    char Reserved[1024];
} tHeaderDataEx;

typedef struct {
    char *ArcName;
    int OpenMode, OpenResult;
    char *CmtBuf;
    int CmtBufSize, CmtSize, CmtState;
} tOpenArchiveData;

typedef struct {
    int size, PluginInterfaceVersionLow, PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} PackDefaultParamStruct;

typedef int (__stdcall *tProcessDataProc)(char *FileName, int Size);
typedef int (__stdcall *tChangeVolProc)(char *ArcName, int Mode);

HANDLE __stdcall OpenArchive(tOpenArchiveData *data);
int __stdcall ReadHeader(HANDLE archive, tHeaderData *header);
int __stdcall ReadHeaderEx(HANDLE archive, tHeaderDataEx *header);
int __stdcall ProcessFile(HANDLE archive, int operation, char *dest_path, char *dest_name);
int __stdcall CloseArchive(HANDLE archive);
void __stdcall SetProcessDataProc(HANDLE archive, tProcessDataProc callback);
void __stdcall SetChangeVolProc(HANDLE archive, tChangeVolProc callback);
int __stdcall PackFiles(char *archive, char *subpath, char *source, char *list, int flags);
int __stdcall DeleteFiles(char *archive, char *list);
int __stdcall GetPackerCaps(void);
BOOL __stdcall CanYouHandleThisFile(char *name);
void __stdcall ConfigurePacker(HWND parent, HINSTANCE instance);
void __stdcall PackSetDefaultParams(PackDefaultParamStruct *params);
#endif
