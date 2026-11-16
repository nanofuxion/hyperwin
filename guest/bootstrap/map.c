#define FILE_MAP_WRITE 0x0002u
#define FILE_MAP_READ 0x0004u

#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
__declspec(dllimport) void *WINAPI OpenFileMappingW(DWORD access, BOOL inherit, const uint16_t *name);
__declspec(dllimport) void *WINAPI MapViewOfFile(void *mapping, DWORD access, DWORD hi, DWORD lo, size_t bytes);
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "map.h"

static const uint16_t k_global[] = {'G','l','o','b','a','l','\\','H','y','p','e','r','W','i','n',0};
static const uint16_t k_local[] = {'H','y','p','e','r','W','i','n',0};

struct hw_header *hw_guest_window(void)
{
    static struct hw_header *view;
    void *mapping;
    if (view) return view;
    mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, k_global);
    if (!mapping) mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, k_local);
    if (!mapping) return 0;
    view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, HW_WINDOW_SIZE);
    return view;
}
