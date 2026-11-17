// Checks OpenGL through whatever opengl32.dll sits beside it (use.cmd zink puts Mesa Zink there):
// makes a context on a hidden window, clears to green, reads a pixel back. Prints the renderer and
// PASS or FAIL.
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[MAX_PATH];
    GetModuleFileNameA(LoadLibraryA("opengl32.dll"), path, sizeof(path));
    printf("using %s\n", path);
    WNDCLASSA wc = {CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "gltest"};
    RegisterClassA(&wc);
    HWND w = CreateWindowA("gltest", "gltest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { printf("pixel format failed (%lu)\n", GetLastError()); return 2; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { printf("context failed (%lu)\n", GetLastError()); return 3; }
    printf("renderer: %s\nversion: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    unsigned char px[4] = {0};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("gl: pixel %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3]);
    int ok = px[0] == 0 && px[1] == 0xff && px[2] == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    return ok ? 0 : 1;
}
