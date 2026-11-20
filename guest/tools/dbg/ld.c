/* Makes a live kernel dump of the running system: ld.exe <file.dmp> */
#include <windows.h>
#include <stdio.h>
typedef LONG (NTAPI *NtSDC)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *RAP)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
struct live {
    ULONG Version, BugCheckCode;
    ULONG_PTR p1, p2, p3, p4;
    PVOID File, Cancel;
    ULONG Flags, AddPages;
};
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    NtSDC sdc = (NtSDC)GetProcAddress(nt, "NtSystemDebugControl");
    RAP rap = (RAP)GetProcAddress(nt, "RtlAdjustPrivilege");
    BOOLEAN old;
    printf("SeDebug %ld\n", rap(20, TRUE, FALSE, &old));
    HANDLE f = CreateFileA(argv[1], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("create %lu\n", GetLastError()); return 2; }
    struct live c = {1, 0x161, 0, 0, 0, 0, f, NULL, 0, 0};
    c.Flags = 1 | 2;   /* use dump storage stack off: UseDumpStorageStack=0 */
    c.Flags = 2;       /* compress */
    ULONG ret = 0;
    LONG st = sdc(37, &c, sizeof c, NULL, 0, &ret);
    printf("NtSystemDebugControl 0x%lx\n", (unsigned long)st);
    CloseHandle(f);
    return st ? 1 : 0;
}
