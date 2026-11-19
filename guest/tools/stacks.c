/* stacks: where each thread of a process is waiting, as module!export+offset, for hangs with no
 * debugger in Windows. Usage: stacks <pid or exe name>. Needs the debug privilege (the agent has it).
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/stacks.c -o build/stacks.exe -ldbghelp */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD find_pid(const char *arg)
{
    DWORD pid = strtoul(arg, NULL, 10);
    if (pid) return pid;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 e = {sizeof e};
    for (BOOL ok = Process32First(s, &e); ok; ok = Process32Next(s, &e))
        if (!_stricmp(e.szExeFile, arg)) pid = e.th32ProcessID;
    CloseHandle(s);
    return pid;
}

static void name_of(HANDLE p, DWORD64 a)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof *si;
    si->MaxNameLen = 255;
    IMAGEHLP_MODULE64 m = {sizeof m};
    const char *mod = SymGetModuleInfo64(p, a, &m) ? m.ModuleName : "?";
    DWORD64 off = 0;
    if (SymFromAddr(p, a, &off, si)) printf("    %s!%s+0x%llx\n", mod, si->Name, (unsigned long long)off);
    else printf("    %s+0x%llx\n", mod, (unsigned long long)(a - m.BaseOfImage));
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    DWORD pid = find_pid(argv[1]);
    HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!p) {
        printf("cannot open %s: %lu\n", argv[1], GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(p, NULL, TRUE);
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c = {0};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &c)) {
            printf("thread %lu\n", te.th32ThreadID);
            STACKFRAME64 f = {0};
            f.AddrPC.Offset = c.Pc;
            f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = c.Fp;
            f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = c.Sp;
            f.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_ARM64, p, t, &f, &c, NULL,
                                                   SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
                if (!f.AddrPC.Offset) break;
                name_of(p, f.AddrPC.Offset);
            }
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(s);
    return 0;
}
