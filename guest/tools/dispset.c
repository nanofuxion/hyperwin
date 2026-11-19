/* dispset: makes one display the only one at a given size (wddm/README.md, screen half).
 *   dispset \\.\DISPLAY2 1950 900     sets it primary at that size and detaches every other display
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dispset.c -o build/dispset.exe -luser32 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) return 2;
    int w = atoi(argv[2]), h = atoi(argv[3]);
    DISPLAY_DEVICEA dd = {sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && strcmp(dd.DeviceName, argv[1])) continue;
        DEVMODEA dm = {0};
        dm.dmSize = sizeof dm;
        LONG r;
        if (!strcmp(dd.DeviceName, argv[1])) {
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_BITSPERPEL;
            dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
            r = ChangeDisplaySettingsExA(dd.DeviceName, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL);
            printf("%s primary %dx%d: %ld\n", dd.DeviceName, w, h, r);
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
