/* dispset: makes one display the only one at a given size (wddm/README.md, screen half).
 *   dispset \\.\DISPLAY2 1950 900     sets it primary at that size and detaches every other display
 *   dispset auto                       the same for the HyperWin screen, at its first mode; waits up to 90 s for it
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dispset.c -o build/dispset.exe -luser32 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    int w = argc > 3 ? atoi(argv[2]) : 0, h = argc > 3 ? atoi(argv[3]) : 0;
    int hz = argc > 4 ? atoi(argv[4]) : 0;
    char found[32];
    if (!strcmp(argv[1], "auto")) {
        found[0] = 0;
        for (int t = 0; t < 45 && !found[0]; t++) {
            DISPLAY_DEVICEA a = {sizeof a};
            char fallback[32] = {0};
            for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &a, 0); i++) {
                if (strstr(a.DeviceString, "HyperWin GPU")) {
                    strncpy(found, a.DeviceName, sizeof found - 1);
                    found[sizeof found - 1] = 0;
                    break;
                }
                if (!fallback[0] && strstr(a.DeviceString, "HyperWin screen")) {
                    strncpy(fallback, a.DeviceName, sizeof fallback - 1);
                    fallback[sizeof fallback - 1] = 0;
                }
            }
            if (!found[0] && fallback[0]) strcpy(found, fallback);
            if (!found[0]) Sleep(2000);
        }
        if (!found[0]) return 3;
        argv[1] = found;
        if (w <= 0 || h <= 0) {
            DEVMODEA m = {0};
            m.dmSize = sizeof m;
            if (!EnumDisplaySettingsA(found, 0, &m)) return 4;
            w = (int)m.dmPelsWidth; h = (int)m.dmPelsHeight;
        }
    }
    if (w <= 0 || h <= 0) return 2;
    DISPLAY_DEVICEA dd = {sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && strcmp(dd.DeviceName, argv[1])) continue;
        DEVMODEA dm = {0};
        dm.dmSize = sizeof dm;
        LONG r;
        if (!strcmp(dd.DeviceName, argv[1])) {
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_BITSPERPEL;
            dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
            if (hz > 0) {
                dm.dmFields |= DM_DISPLAYFREQUENCY;
                dm.dmDisplayFrequency = (DWORD)hz;
            }
            r = ChangeDisplaySettingsExA(dd.DeviceName, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL);
            printf("%s primary %dx%d@%dHz: %ld\n", dd.DeviceName, w, h, hz, r);
        } else {
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
            r = ChangeDisplaySettingsExA(dd.DeviceName, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
            printf("%s detach: %ld\n", dd.DeviceName, r);
        }
    }
    LONG r = ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
    printf("apply: %ld\n", r);
    return 0;
}
