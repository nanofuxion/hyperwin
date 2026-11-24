/* qtest9: Direct3D 9 through Windows' own d3d9.dll and whatever driver the adapter names. Clears a
 * window's back buffer to red, draws a green triangle over its middle, reads the pixels back and
 * presents. Prints the adapter, both pixels and PASS or FAIL.
 * Build: <arch>-w64-mingw32-clang -O2 guest/tools/d3d9test.c -o build/d3d9test-<arch>.exe -ld3d9 -lgdi32 -luser32 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcA(w, m, a, b); }

struct vtx { float x, y, z, rhw; DWORD color; };

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qtest9";
    RegisterClassA(&wc);
    RECT rc = {0, 0, 640, 360};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("qtest9", "qtest9", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 360; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    IDirect3DQuery9 *qs[4] = {0};
    for (int i = 0; i < 4; i++) {
        hr = IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &qs[i]);
        if (FAILED(hr)) { printf("CreateQuery 0x%08lx\n", hr); return 1; }
    }
    long frame_no = 0;
    struct vtx tri[3] = {{320, 10, 0.5f, 1, 0xff00ff00}, {620, 340, 0.5f, 1, 0xff00ff00}, {20, 340, 0.5f, 1, 0xff00ff00}};
    int load = argc > 1 ? atoi(argv[1]) : 40;      /* small triangles a frame (CPU side) */
    int quads = argc > 2 ? atoi(argv[2]) : 0;      /* full-window alpha-blended quads a frame (GPU side) */
    int late = argc > 3 ? atoi(argv[3]) : 2;       /* read the answer this many frames late (0 = same frame) */
    int flush = argc > 4 ? atoi(argv[4]) : 0;      /* D3DGETDATA_FLUSH */
    int ue3 = argc > 5 ? atoi(argv[5]) : 0;        /* 1: Unreal Engine 3's pattern: an EVENT query ended each frame, spun on with FLUSH until it is S_OK, before Present */
    IDirect3DQuery9 *ev = NULL;
    if (ue3) IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &ev);
    printf("load %d triangles, %d quads, read %d frames late, flush %d\n", load, quads, late, flush);
    struct vtx quad[6] = {{0, 0, 0.5f, 1, 0x20ff0000}, {640, 0, 0.5f, 1, 0x20ff0000}, {0, 360, 0.5f, 1, 0x20ff0000},
                          {640, 0, 0.5f, 1, 0x20ff0000}, {640, 360, 0.5f, 1, 0x20ff0000}, {0, 360, 0.5f, 1, 0x20ff0000}};
    LARGE_INTEGER f, t0, t1, w0, w1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    long frames = 0, spins = 0;
    double wait_ms = 0, wait_max = 0;
    for (;;) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DQuery9 *q = qs[frame_no & 3];
        IDirect3DQuery9_Issue(q, D3DISSUE_BEGIN);
        for (int k = 0; k < load; k++) IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof *tri);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        for (int k = 0; k < quads; k++) IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad, sizeof *quad);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DQuery9_Issue(q, D3DISSUE_END);
        IDirect3DDevice9_EndScene(dev);
        if (ue3 && ev) {
            BOOL done = FALSE;
            LARGE_INTEGER e0, e1;
            IDirect3DQuery9_Issue(ev, D3DISSUE_END);
            QueryPerformanceCounter(&e0);
            while (IDirect3DQuery9_GetData(ev, &done, sizeof done, D3DGETDATA_FLUSH) == S_FALSE) spins++;
            QueryPerformanceCounter(&e1);
            double ew = (double)(e1.QuadPart - e0.QuadPart) * 1000.0 / f.QuadPart;
            wait_ms += ew;
            if (ew > wait_max) wait_max = ew;
        }
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        DWORD n = 0;
        QueryPerformanceCounter(&w0);
        if (frame_no >= late) {   /* the answer of an earlier frame */
            IDirect3DQuery9 *old = qs[(frame_no - late) & 3];
            while (IDirect3DQuery9_GetData(old, &n, sizeof n, flush ? D3DGETDATA_FLUSH : 0) == S_FALSE) spins++;
        }
        frame_no++;
        QueryPerformanceCounter(&w1);
        double w = (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / f.QuadPart;
        wait_ms += w;
        if (w > wait_max) wait_max = w;
        frames++;
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        QueryPerformanceCounter(&t1);
        double el = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        if (el >= 2.0) {
            printf("%.1f fps, query wait %.2f ms avg %.1f ms max, %ld spins/frame, samples %lu\n", frames / el, wait_ms / frames, wait_max, spins / frames, (unsigned long)n);
            frames = 0; spins = 0; wait_ms = 0; wait_max = 0;
            t0 = t1;
        }
    }
}
