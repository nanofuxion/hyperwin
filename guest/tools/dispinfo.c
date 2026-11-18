/* dispinfo: what Windows thinks of its adapters and screens. For each adapter the kernel's flags
 * (render, display, post device) and name, then each display device with its modes.
 * Output goes to stdout and C:\HyperWin\dispinfo.txt.
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dispinfo.c -o build/dispinfo.exe -lgdi32 -luser32 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The few D3DKMT calls and types needed, declared here so no driver kit header is involved. */
typedef struct { UINT hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPresentMoveRegionsPreferred; } KMT_ADAPTERINFO;
typedef struct { ULONG NumAdapters; KMT_ADAPTERINFO *pAdapters; } KMT_ENUMADAPTERS2;
typedef struct { UINT hAdapter; UINT Type; void *pData; UINT Size; } KMT_QUERYADAPTERINFO;
typedef struct { UINT Value; } KMT_ADAPTERTYPE;
typedef LONG (WINAPI *PFN_ENUM2)(KMT_ENUMADAPTERS2 *);
typedef LONG (WINAPI *PFN_QAI)(KMT_QUERYADAPTERINFO *);

static FILE *out;
static void say(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    vprintf(f, a);
    va_end(a);
    if (out) { va_start(a, f); vfprintf(out, f, a); va_end(a); fflush(out); }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    out = fopen("C:\\HyperWin\\dispinfo.txt", "w");
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    PFN_ENUM2 enum2 = (PFN_ENUM2)(void *)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    PFN_QAI qai = (PFN_QAI)(void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    KMT_ADAPTERINFO list[16];
    KMT_ENUMADAPTERS2 e = { 16, list };
    LONG st = enum2 ? enum2(&e) : -1;
    say("enum adapters: %08lx, %lu\n", (unsigned long)st, e.NumAdapters);
    for (ULONG i = 0; st == 0 && i < e.NumAdapters; i++) {
        KMT_ADAPTERTYPE t = { 0 };
        KMT_QUERYADAPTERINFO q = { list[i].hAdapter, 15 /* ADAPTERTYPE */, &t, sizeof t };
        LONG qs = qai(&q);
        say("adapter %lu luid %08lx:%08lx sources %lu type %08lx (%08lx): render=%u display=%u software=%u post=%u\n", i,
            (unsigned long)list[i].AdapterLuid.HighPart, (unsigned long)list[i].AdapterLuid.LowPart, list[i].NumOfSources,
            (unsigned long)t.Value, (unsigned long)qs, t.Value & 1, (t.Value >> 1) & 1, (t.Value >> 2) & 1, (t.Value >> 3) & 1);
    }
    if (argc > 2 && !strcmp(argv[1], "attach")) {
        /* dispinfo attach \\.\DISPLAY2 [x] [w] [h]: extends the desktop onto that display. */
        DEVMODEA dm = { 0 };
        dm.dmSize = sizeof dm;
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        dm.dmPosition.x = argc > 3 ? atoi(argv[3]) : 1280;
        dm.dmPelsWidth = argc > 4 ? atoi(argv[4]) : 1280;
        dm.dmPelsHeight = argc > 5 ? atoi(argv[5]) : 720;
        LONG r = ChangeDisplaySettingsExA(argv[2], &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        say("attach %s: %ld\n", argv[2], r);
        r = ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
        say("apply: %ld\n", r);
    }
    DISPLAY_DEVICEA dd;
    for (DWORD i = 0; ; i++) {
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        say("display %lu: %s | %s | flags %08lx\n", i, dd.DeviceName, dd.DeviceString, dd.StateFlags);
        DEVMODEA dm;
        char last[64] = "";
        int n = 0;
        for (DWORD k = 0; k < 400; k++) {
            dm.dmSize = sizeof dm;
            if (!EnumDisplaySettingsA(dd.DeviceName, k, &dm)) break;
            char b[64];
            snprintf(b, sizeof b, "%lux%lu@%lu", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
            if (strcmp(b, last) && n < 24) { say("   %s\n", b); n++; }
            strcpy(last, b);
        }
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            say("   current %lux%lu@%lu at %ld,%ld\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmPosition.x, dm.dmPosition.y);
    }
    return 0;
}
