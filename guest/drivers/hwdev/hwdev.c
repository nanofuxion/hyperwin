/* hwdev.exe: adds, updates and removes HyperWin's root devices, in place of Microsoft's devcon.exe,
 * which comes with the WDK and may not be passed on (its license forbids sharing it). Only what
 * install-driver.ps1 uses:
 *
 *   hwdev install <inf> <hardware id>   make a root device with that ID, then install the INF on it
 *   hwdev update  <inf> <hardware id>   install the INF on every device with that ID
 *   hwdev remove  <hardware id>         remove every present device with that ID; a leading '*'
 *                                       also matches IDs that end with the rest, like devcon
 *
 * Exit codes are devcon's: 0 done, 1 done but takes effect at the next restart, 2 failed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <wchar.h>

#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32 /* cfgmgr32.h */
#endif
#ifndef INSTALLFLAG_FORCE
#define INSTALLFLAG_FORCE 0x00000001
#endif

/* newdev.dll has no import library in llvm-mingw, so look the one function up. */
typedef BOOL(WINAPI *update_fn)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

static int fail(const char *what)
{
    fprintf(stderr, "hwdev: %s failed: error %lu\n", what, GetLastError());
    return 2;
}

static int update(const wchar_t *inf, const wchar_t *id)
{
    HMODULE m = LoadLibraryW(L"newdev.dll");
    update_fn f = m ? (update_fn)(void *)GetProcAddress(m, "UpdateDriverForPlugAndPlayDevicesW") : NULL;
    if (!f) return fail("loading UpdateDriverForPlugAndPlayDevicesW");
    BOOL reboot = FALSE;
    if (!f(NULL, id, inf, INSTALLFLAG_FORCE, &reboot)) return fail("UpdateDriverForPlugAndPlayDevices");
    printf("hwdev: installed %ls on %ls%s\n", inf, id, reboot ? ", restart needed" : "");
    return reboot ? 1 : 0;
}

static int install(const wchar_t *inf, const wchar_t *id)
{
    size_t n = wcslen(id);
    if (n == 0 || n > 200) { fprintf(stderr, "hwdev: bad hardware id\n"); return 2; }
    GUID cls;
    wchar_t name[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf, &cls, name, MAX_CLASS_NAME_LEN, NULL)) return fail("SetupDiGetINFClass");
    HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, NULL);
    if (set == INVALID_HANDLE_VALUE) return fail("SetupDiCreateDeviceInfoList");

    wchar_t ids[202] = {0}; /* REG_MULTI_SZ: the one ID, then an empty string */
    wmemcpy(ids, id, n);
    SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
    int r = 2;
    if (!SetupDiCreateDeviceInfoW(set, name, &cls, NULL, NULL, DICD_GENERATE_ID, &dev))
        fail("SetupDiCreateDeviceInfo");
    else if (!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE *)ids,
                                                (DWORD)((n + 2) * sizeof(wchar_t))))
        fail("SetupDiSetDeviceRegistryProperty");
    else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev))
        fail("DIF_REGISTERDEVICE");
    else if ((r = update(inf, id)) == 2)
        SetupDiCallClassInstaller(DIF_REMOVE, set, &dev); /* no driver took it: leave no empty device */
    SetupDiDestroyDeviceInfoList(set);
    return r;
}

static int matches(const wchar_t *multi, const wchar_t *pat)
{
    size_t pl = wcslen(pat);
    for (const wchar_t *p = multi; *p; p += wcslen(p) + 1) {
        if (!_wcsicmp(p, pat)) return 1;
        size_t l = wcslen(p);
        if (pat[0] == L'*' && l >= pl - 1 && !_wcsicmp(p + l - (pl - 1), pat + 1)) return 1;
    }
    return 0;
}

static int remove_all(const wchar_t *pat)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return fail("SetupDiGetClassDevs");
    int removed = 0, failed = 0, reboot = 0;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t buf[2048] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *)buf,
                                               sizeof buf - 2 * sizeof(wchar_t), NULL) ||
            !matches(buf, pat))
            continue;
        SP_REMOVEDEVICE_PARAMS rp = { { sizeof(SP_CLASSINSTALL_HEADER), DIF_REMOVE }, DI_REMOVEDEVICE_GLOBAL, 0 };
        if (SetupDiSetClassInstallParamsW(set, &dev, &rp.ClassInstallHeader, sizeof rp) &&
            SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) {
            removed++;
            SP_DEVINSTALL_PARAMS_W ip = { .cbSize = sizeof ip };
            if (SetupDiGetDeviceInstallParamsW(set, &dev, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
                reboot = 1;
        } else {
            fail("DIF_REMOVE");
            failed++;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    printf("hwdev: removed %d device(s) matching %ls%s\n", removed, pat, reboot ? ", restart needed" : "");
    return failed ? 2 : reboot ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t inf[MAX_PATH];
    if (argc == 4 && (!wcscmp(argv[1], L"install") || !wcscmp(argv[1], L"update"))) {
        if (!GetFullPathNameW(argv[2], MAX_PATH, inf, NULL)) return fail("GetFullPathName");
        return argv[1][0] == L'i' ? install(inf, argv[3]) : update(inf, argv[3]);
    }
    if (argc == 3 && !wcscmp(argv[1], L"remove") && argv[2][0])
        return remove_all(argv[2]);
    fprintf(stderr, "usage: hwdev install|update <inf> <hardware id>\n       hwdev remove <hardware id>\n");
    return 2;
}
