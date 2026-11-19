/* d3d8test: Direct3D 8 through Windows' own d3d8.dll (32-bit only on Windows on ARM). Clears to red, draws a
 * green triangle, reads the back buffer and prints PASS or FAIL.
 * Build: i686-w64-mingw32-clang -O2 guest/tools/d3d8test.c -o build/d3d8test-x86.exe -ld3d8 -lgdi32 -luser32 */
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>

struct vtx { float x, y, z, rhw; DWORD color; };

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d8test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d8test", "d3d8test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 272, 295, NULL, NULL, wc.hInstance, NULL);
    IDirect3D8 *d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) { printf("FAIL Direct3DCreate8\n"); return 1; }
    D3DADAPTER_IDENTIFIER8 id;
    if (SUCCEEDED(IDirect3D8_GetAdapterIdentifier(d3d, 0, 0, &id))) printf("adapter: %s (%s)\n", id.Description, id.Driver);
    D3DDISPLAYMODE dm = {0};
    printf("display mode 0x%08lx: %ux%u format %u\n", IDirect3D8_GetAdapterDisplayMode(d3d, 0, &dm), dm.Width, dm.Height, dm.Format);
    D3DCAPS8 caps;
    printf("GetDeviceCaps 0x%08lx\n", IDirect3D8_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps));
    printf("CheckDeviceType 0x%08lx\n", IDirect3D8_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, dm.Format, dm.Format, TRUE));
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = 256; pp.BackBufferHeight = 256; pp.BackBufferFormat = dm.Format; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    IDirect3DDevice8 *dev = NULL;
    HRESULT hr = IDirect3D8_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    struct vtx tri[3] = {{128, 32, 0.5f, 1, 0xff00ff00}, {224, 224, 0.5f, 1, 0xff00ff00}, {32, 224, 0.5f, 1, 0xff00ff00}};
    IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
    IDirect3DDevice8_BeginScene(dev);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    printf("DrawPrimitiveUP 0x%08lx\n", IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof *tri));
    IDirect3DDevice8_EndScene(dev);
    IDirect3DSurface8 *bb = NULL;
    IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DLOCKED_RECT lr;
    int ok = 0;
    hr = bb ? IDirect3DSurface8_LockRect(bb, &lr, NULL, D3DLOCK_READONLY) : E_FAIL;
    printf("LockRect 0x%08lx\n", hr);
    if (SUCCEEDED(hr)) {
        DWORD mid = ((DWORD *)((BYTE *)lr.pBits + 160 * lr.Pitch))[128] & 0xffffff;
        DWORD corner = ((DWORD *)((BYTE *)lr.pBits + 4 * lr.Pitch))[4] & 0xffffff;
        printf("middle %06lx, corner %06lx\n", mid, corner);
        ok = mid == 0x00ff00 && corner == 0xff0000;
        IDirect3DSurface8_UnlockRect(bb);
    }
    for (int i = 0; i < 30; i++) { IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL); Sleep(16); }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
