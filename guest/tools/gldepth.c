/* gldepth: checks that OpenGL's depth buffer and face culling work. Draws a far quad first and a near quad
 * after it (so the near one must win), then a back-facing triangle with culling on (must vanish), and
 * reads pixels back. Prints the pixel format's depth bits and PASS or FAIL for each check.
 * Build: <arch>-w64-mingw32-clang -O2 guest/tools/gldepth.c -o build/gldepth-<arch>.exe -lopengl32 -lgdi32 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

static void px(int x, int y, unsigned char *o) { glReadPixels(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, o); }

int main(int argc, char **argv)
{
    int alt = argc > 1;   /* a second copy draws other colours, to tell whose pixels turn up */
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = {CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "gldepth"};
    RegisterClassA(&wc);
    HWND w = CreateWindowA("gldepth", "gldepth", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 200, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE};
    int pf = ChoosePixelFormat(dc, &pfd);
    PIXELFORMATDESCRIPTOR got;
    DescribePixelFormat(dc, pf, sizeof got, &got);
    printf("pixel format %d: color %d depth %d stencil %d flags 0x%lx\n", pf, got.cColorBits, got.cDepthBits, got.cStencilBits, got.dwFlags);
    if (!SetPixelFormat(dc, pf, &pfd)) return 2;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return 3;
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    glViewport(0, 0, 200, 200);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    /* far quad (z = 0.5) red, drawn first; near quad (z = -0.5) green, drawn second, covers the left half only */
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS); glVertex3f(-0.8f, -0.8f, 0.5f); glVertex3f(0.8f, -0.8f, 0.5f); glVertex3f(0.8f, 0.8f, 0.5f); glVertex3f(-0.8f, 0.8f, 0.5f); glEnd();
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS); glVertex3f(-0.8f, -0.8f, -0.5f); glVertex3f(0.0f, -0.8f, -0.5f); glVertex3f(0.0f, 0.8f, -0.5f); glVertex3f(-0.8f, 0.8f, -0.5f); glEnd();
    /* a far quad drawn after the near one must not cover it */
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS); glVertex3f(-0.8f, -0.8f, 0.9f); glVertex3f(0.8f, -0.8f, 0.9f); glVertex3f(0.8f, 0.8f, 0.9f); glVertex3f(-0.8f, 0.8f, 0.9f); glEnd();
    unsigned char a[3], b[3];
    px(60, 100, a);   /* left: near green must show */
    px(140, 100, b);  /* right: red (far) must show over blue (farther) */
    printf("left %d,%d,%d (want 0,255,0)  right %d,%d,%d (want 255,0,0)\n", a[0], a[1], a[2], b[0], b[1], b[2]);
    int depth_ok = a[1] > 200 && a[0] < 50 && b[0] > 200 && b[2] < 50;
    printf("depth test: %s\n", depth_ok ? "PASS" : "FAIL");
    /* culling: a counter-clockwise triangle is front; the clockwise one must vanish with GL_BACK culled */
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
    glColor3f(1, 1, 0);
    glBegin(GL_TRIANGLES); glVertex3f(-0.9f, -0.9f, 0); glVertex3f(-0.1f, -0.9f, 0); glVertex3f(-0.5f, 0.9f, 0); glEnd();   /* CCW, shown, left */
    glColor3f(0, 1, 1);
    glBegin(GL_TRIANGLES); glVertex3f(0.1f, -0.9f, 0); glVertex3f(0.5f, 0.9f, 0); glVertex3f(0.9f, -0.9f, 0); glEnd();        /* CW, culled, right */
    px(50, 60, a); px(150, 60, b);
    printf("cull: left %d,%d,%d (want 255,255,0) right %d,%d,%d (want 0,0,0)\n", a[0], a[1], a[2], b[0], b[1], b[2]);
    int cull_ok = a[0] > 200 && a[1] > 200 && b[0] < 50 && b[1] < 50 && b[2] < 50;
    printf("culling: %s\n", cull_ok ? "PASS" : "FAIL");
    /* frames: clear, draw far then near (z moves each frame), swap; the near quad must win every frame */
    glEnable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    int bad = 0, frames = 120;
    for (int f = 0; f < frames; f++) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        float zn = -0.9f + 0.015f * (f % 100), zf = zn + 0.4f;
        if (alt) glColor3f(0, 0, 1); else glColor3f(1, 0, 0);
        glBegin(GL_QUADS); glVertex3f(-0.8f, -0.8f, zf); glVertex3f(0.8f, -0.8f, zf); glVertex3f(0.8f, 0.8f, zf); glVertex3f(-0.8f, 0.8f, zf); glEnd();
        if (alt) glColor3f(1, 1, 0); else glColor3f(0, 1, 0);
        glBegin(GL_QUADS); glVertex3f(-0.5f, -0.5f, zn); glVertex3f(0.5f, -0.5f, zn); glVertex3f(0.5f, 0.5f, zn); glVertex3f(-0.5f, 0.5f, zn); glEnd();
        unsigned char c[3];
        if (argc > 2) glFinish();   /* second argument: wait for the GPU before reading */
        px(100, 100, c);
        int good = alt ? (c[0] > 200 && c[1] > 200 && c[2] < 50) : (c[1] > 200 && c[0] < 50 && c[2] < 50);
        if (!good) { bad++; if (bad < 6) printf("frame %d: centre %d,%d,%d\n", f, c[0], c[1], c[2]); }
        SwapBuffers(dc);
    }
    printf("frames with the near quad lost: %d of %d\n", bad, frames);
    printf("frames: %s\n", bad ? "FAIL" : "PASS");
    return depth_ok && cull_ok && !bad ? 0 : 1;
}
