/* Minimal Windows types so the bootstrap DLLs build with mingw or a freestanding ARM64 link. */
#ifndef HYPERWIN_WIN_H
#define HYPERWIN_WIN_H

#include <stdint.h>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int16_t SHORT;
typedef int32_t HRESULT;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;

#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_FAIL ((HRESULT)0x80004005)
#define ERROR_SUCCESS 0u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_BAD_ARGUMENTS 160u
#define DLL_PROCESS_ATTACH 1u

#ifndef WINAPI
#define WINAPI
#endif

#endif
