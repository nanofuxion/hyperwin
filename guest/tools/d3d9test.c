/* d3d9test: Direct3D 9 through Windows' own d3d9.dll and whatever driver the adapter names. Clears a
 * window's back buffer to red, draws a green triangle over its middle, reads the pixels back and
 * presents. Prints the adapter, both pixels and PASS or FAIL.
 * Build: <arch>-w64-mingw32-clang -O2 guest/tools/d3d9test.c -o build/d3d9test-<arch>.exe -ld3d9 -lgdi32 -luser32 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcA(w, m, a, b); }

struct vtx { float x, y, z, rhw; DWORD color; };

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d9test";
    RegisterClassA(&wc);
    RECT rc = {0, 0, 256, 256};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("d3d9test", "d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("FAIL Direct3DCreate9\n");
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id))) printf("adapter: %s (%s)\n", id.Description, id.Driver);
    D3DDISPLAYMODE dm = {0};
    HRESULT mh = IDirect3D9_GetAdapterDisplayMode(d3d, 0, &dm);
    printf("display mode 0x%08lx: %ux%u format %u\n", mh, dm.Width, dm.Height, dm.Format);
    printf("CheckDeviceType X8R8G8B8 windowed: 0x%08lx\n",
           IDirect3D9_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE));
    printf("CheckDeviceFormat D24S8 depth: 0x%08lx\n",
           IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8));
    D3DCAPS9 caps;
    printf("GetDeviceCaps 0x%08lx\n", IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps));
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    struct vtx tri[3] = {{128, 32, 0.5f, 1, 0xff00ff00}, {224, 224, 0.5f, 1, 0xff00ff00}, {32, 224, 0.5f, 1, 0xff00ff00}};
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff0000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof *tri);
    printf("DrawPrimitiveUP 0x%08lx\n", hr);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 256, 256, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
    printf("GetRenderTargetData 0x%08lx\n", hr);
    D3DLOCKED_RECT lr;
    int ok = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        DWORD mid = ((DWORD *)((BYTE *)lr.pBits + 160 * lr.Pitch))[128] & 0xffffff;
        DWORD corner = ((DWORD *)((BYTE *)lr.pBits + 4 * lr.Pitch))[4] & 0xffffff;
        printf("middle %06lx, corner %06lx\n", mid, corner);
        ok = mid == 0x00ff00 && corner == 0xff0000;
        IDirect3DSurface9_UnlockRect(sys);
    }
    for (int i = 0; i < 60; i++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof *tri);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(16);
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
