// Prints a few raw input reports, and the feature reports SDL asks for, from the first HID device
// whose path holds the given text (gamepad/README.md).   hidpeek.exe [text] [reports]   default vid_054c, 5
// Writes hidpeek.txt beside the program.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    const char *want = argc > 1 ? argv[1] : "vid_054c";
    int count = argc > 2 ? atoi(argv[2]) : 5;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "hidpeek.txt");
    FILE *out = fopen(path, "w");
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        char buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *d = (void *)buf;
        d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, d, sizeof buf, NULL, NULL)) continue;
        if (!strstr(_strlwr(d->DevicePath), want)) continue;
        fprintf(out, "%s\n", d->DevicePath);
        HANDLE h = CreateFileA(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { fprintf(out, "open failed %lu\n", GetLastError()); continue; }
        UCHAR f[64];
        UCHAR ids[] = {0x02, 0x12, 0xa3};
        for (int k = 0; k < 3; k++) {
            UCHAR big[64] = {ids[k]};
            BOOL ok = HidD_GetFeature(h, big, ids[k] == 0x02 ? 37 : ids[k] == 0x12 ? 16 : 49);
            fprintf(out, "feature %02x: %s", ids[k], ok ? "" : "failed");
            for (int j = 0; ok && j < 20; j++) fprintf(out, " %02x", big[j]);
            fprintf(out, "\n");
        }
        for (int r = 0; r < count; r++) {
            DWORD got = 0;
            memset(f, 0, sizeof f);
            if (!ReadFile(h, f, sizeof f, &got, NULL)) { fprintf(out, "read failed %lu\n", GetLastError()); break; }
            fprintf(out, "report %lu:", got);
            for (DWORD j = 0; j < got && j < 32; j++) fprintf(out, " %02x", f[j]);
            fprintf(out, "\n");
        }
        CloseHandle(h);
        break;
    }
    fclose(out);
    return 0;
}
