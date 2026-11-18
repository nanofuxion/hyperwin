// Shows what Windows gets from HyperWin's mouse, touch screen and touchpad (mouse/README.md).
//   inputtest.exe [seconds] [look]   default 10; run it on the user's desktop. "look" acts like a
//                                    game in mouse-look: in front, cursor hidden and clipped.
// Writes inputtest.txt beside the program: the touch hardware Windows sees, then per second the raw
// mouse movement (relative counts and absolute packets apart), buttons and wheels, the touch
// contacts seen as WM_POINTER input, and where the cursor is.
#define WINVER 0x0A00
#include <windows.h>
#include <stdio.h>

static FILE *out;
static long rel_x, rel_y, abs_n, rel_n, wheel, hwheel, touch_n, touch_max;
static unsigned buttons_down;

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            RAWMOUSE *r = &ri.data.mouse;
            if (r->usFlags & MOUSE_MOVE_ABSOLUTE) abs_n++;
            else { rel_x += r->lLastX; rel_y += r->lLastY; rel_n++; }
            if (r->usButtonFlags & RI_MOUSE_WHEEL) wheel += (SHORT)r->usButtonData;
            if (r->usButtonFlags & RI_MOUSE_HWHEEL) hwheel += (SHORT)r->usButtonData;
            buttons_down |= r->usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN);
        }
    } else if (m == WM_POINTERDOWN || m == WM_POINTERUPDATE) {
        POINTER_INPUT_TYPE t;
        if (GetPointerType(GET_POINTERID_WPARAM(wp), &t) && t == PT_TOUCH) {
            UINT32 n = 0;
            touch_n++;
            if (GetPointerFrameInfo(GET_POINTERID_WPARAM(wp), &n, NULL) && (long)n > touch_max) touch_max = n;
        }
    }
    return DefWindowProcW(w, m, wp, lp);
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int seconds = argc > 1 ? atoi(argv[1]) : 10, s;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "inputtest.txt");
    out = fopen(path, "w");
    fprintf(out, "touch: digitizer 0x%x, up to %d contacts\n", GetSystemMetrics(SM_DIGITIZER), GetSystemMetrics(SM_MAXIMUMTOUCHES));
    {
        UINT n = 0, i;
        RAWINPUTDEVICELIST list[64];
        n = 64;
        n = GetRawInputDeviceList(list, &n, sizeof list[0]);
        for (i = 0; i < n && n != (UINT)-1; i++) {
            RID_DEVICE_INFO info = {sizeof info};
            UINT size = sizeof info;
            if (list[i].dwType != RIM_TYPEHID) continue;
            GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size);
            fprintf(out, "hid: vid %04lx pid %04lx usage page %x usage %x\n", info.hid.dwVendorId, info.hid.dwProductId,
                    info.hid.usUsagePage, info.hid.usUsage);
        }
    }
    fflush(out);
    WNDCLASSW wc = {0, proc, 0, 0, GetModuleHandleW(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"inputtest"};
    RegisterClassW(&wc);
    // A full-screen window, so touch contacts land on it; it does not take the cursor.
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"inputtest", L"inputtest", WS_POPUP | WS_VISIBLE,
                             0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(w, 0, 1, LWA_ALPHA);
    if (argc > 2 && !strcmp(argv[2], "look")) {
        RECT r = {100, 100, 900, 600};
        SetWindowLongPtrW(w, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_LAYERED);
        SetForegroundWindow(w);
        ShowCursor(FALSE);
        ClipCursor(&r);
    }
    RAWINPUTDEVICE rid = {1, 2, RIDEV_INPUTSINK, w};
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    for (s = 0; s < seconds; s++) {
        DWORD end = GetTickCount() + 1000;
        POINT p;
        MSG msg;
        while ((LONG)(end - GetTickCount()) > 0) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, end - GetTickCount(), QS_ALLINPUT);
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        GetCursorPos(&p);
        {
            CURSORINFO ci = {sizeof ci};
            RECT clip = {0};
            GetCursorInfo(&ci);
            GetClipCursor(&clip);
            fprintf(out, "     cursor flags %lu, clip %ld,%ld-%ld,%ld, in front %d\n", ci.flags, clip.left, clip.top,
                    clip.right, clip.bottom, GetForegroundWindow() == w);
        }
        fprintf(out, "%2d s: relative %ld packets dx %ld dy %ld, absolute %ld packets, wheel %ld hwheel %ld, buttons 0x%x, "
                "touch %ld updates up to %ld fingers, cursor %ld,%ld\n",
                s + 1, rel_n, rel_x, rel_y, abs_n, wheel, hwheel, buttons_down, touch_n, touch_max, p.x, p.y);
        fflush(out);
        rel_x = rel_y = abs_n = rel_n = wheel = hwheel = touch_n = touch_max = 0;
        buttons_down = 0;
    }
    return 0;
}
