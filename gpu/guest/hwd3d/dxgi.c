/* HwD3D's dxgi.dll: hands each export to the d3d11.dll beside it, where the objects live, so a
 * process has one set of them (d3d11.c). */
#include <windows.h>

static FARPROC sibling(const char *name)
{
    static HMODULE d3d11;
    if (!d3d11) {
        WCHAR path[MAX_PATH];
        HMODULE self = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)sibling, &self);
        DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
        while (n && path[n - 1] != L'\\') n--;
        if (n + 10 >= MAX_PATH) return NULL;
        lstrcpyW(path + n, L"d3d11.dll");
        d3d11 = LoadLibraryW(path);
    }
    return d3d11 ? GetProcAddress(d3d11, name) : NULL;
}

typedef HRESULT(WINAPI *CREATE)(REFIID, void **);
typedef HRESULT(WINAPI *CREATE2)(UINT, REFIID, void **);

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **out)
{
    CREATE f = (CREATE)(void *)sibling("HwCreateDXGIFactory");
    return f ? f(riid, out) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out)
{
    CREATE f = (CREATE)(void *)sibling("HwCreateDXGIFactory1");
    return f ? f(riid, out) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **out)
{
    CREATE2 f = (CREATE2)(void *)sibling("HwCreateDXGIFactory2");
    return f ? f(flags, riid, out) : E_FAIL;
}
