/* dbgrun: starts a program under Windows' own debugger engine (System32\dbgeng.dll) and runs debugger
 * commands, printing everything. Used to see why Windows' Direct3D 9 refuses our adapter.
 *   dbgrun "<command line>" | "pid:<n>" "<commands run at the first stop, then g>" [commands run at each later stop]
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dbgrun.c -o build/dbgrun.exe -ldbgeng -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HRESULT STDMETHODCALLTYPE qi(IDebugOutputCallbacks *This, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDebugOutputCallbacks)) { *out = This; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ref(IDebugOutputCallbacks *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE out_text(IDebugOutputCallbacks *This, ULONG mask, PCSTR text)
{
    (void)This; (void)mask;
    fputs(text, stdout);
    return S_OK;
}
static IDebugOutputCallbacksVtbl vt = {qi, ref, ref, out_text};
static IDebugOutputCallbacks cb = {&vt};

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) return 2;
    IDebugClient *client = NULL;
    IDebugControl *control = NULL;
    if (FAILED(DebugCreate(&IID_IDebugClient, (void **)&client))) return 1;
    client->lpVtbl->QueryInterface(client, &IID_IDebugControl, (void **)&control);
    client->lpVtbl->SetOutputCallbacks(client, &cb);
    control->lpVtbl->AddEngineOptions(control, 0x20);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s", argv[1]);
    /* "pid:<n>" attaches to a running program instead (invasive: it stops at the attach, detaches at the end). */
    int attach = strncmp(cmd, "pid:", 4) == 0;
    HRESULT hr = attach ? client->lpVtbl->AttachProcess(client, 0, strtoul(cmd + 4, NULL, 10), DEBUG_ATTACH_DEFAULT)
                        : client->lpVtbl->CreateProcess(client, 0, cmd, DEBUG_ONLY_THIS_PROCESS);
    printf("%s 0x%08lx\n", attach ? "AttachProcess" : "CreateProcess", hr);
    if (FAILED(hr)) return 1;
    DWORD start = GetTickCount();
    int stop = 0;
    for (;;) {
        hr = control->lpVtbl->WaitForEvent(control, 0, 60000);
        if (hr != S_OK) { printf("WaitForEvent 0x%08lx\n", hr); break; }
        const char *c = stop == 0 ? argv[2] : argc > 3 ? argv[3] : "k";
        ULONG st0 = 0;
        control->lpVtbl->GetExecutionStatus(control, &st0);
        HRESULT eh = control->lpVtbl->Execute(control, DEBUG_OUTCTL_THIS_CLIENT, c, DEBUG_EXECUTE_ECHO);
        printf("[stop %d status %lu execute 0x%08lx]\n", stop, st0, eh);
        stop++;
        if (GetTickCount() - start > (getenv("DBG_MS") ? (DWORD)atol(getenv("DBG_MS")) : 240000)) break;
        ULONG status = 0;
        control->lpVtbl->GetExecutionStatus(control, &status);
        if (status == DEBUG_STATUS_NO_DEBUGGEE) break;
    }
    if (attach) {
        control->lpVtbl->Execute(control, DEBUG_OUTCTL_THIS_CLIENT, "bc *", DEBUG_EXECUTE_ECHO);
        client->lpVtbl->DetachProcesses(client);
        client->lpVtbl->EndSession(client, DEBUG_END_ACTIVE_DETACH);
        return 0;
    }
    client->lpVtbl->TerminateProcesses(client);
    client->lpVtbl->EndSession(client, DEBUG_END_ACTIVE_TERMINATE);
    return 0;
}
