/* dbgrun: starts a program under Windows' own debugger engine (System32\dbgeng.dll) and runs debugger
 * commands, printing everything. Used to see why Windows' Direct3D 9 refuses our adapter.
 *   dbgrun "<command line>" "<commands run at the first stop, then g>" [commands run at each later stop]
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dbgrun.c -o build/dbgrun.exe -ldbgeng -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>
#include <stdio.h>

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

/* dbgattach <pid> "<commands>": looks into a running program without stopping it for good
 * (noninvasive attach: its threads are suspended while the commands run, then it goes on). */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) return 2;
    IDebugClient *client = NULL;
    IDebugControl *control = NULL;
    if (FAILED(DebugCreate(&IID_IDebugClient, (void **)&client))) return 1;
    client->lpVtbl->QueryInterface(client, &IID_IDebugControl, (void **)&control);
    client->lpVtbl->SetOutputCallbacks(client, &cb);
    HRESULT hr = client->lpVtbl->AttachProcess(client, 0, strtoul(argv[1], NULL, 10), DEBUG_ATTACH_NONINVASIVE);
    printf("AttachProcess 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    hr = control->lpVtbl->WaitForEvent(control, 0, 30000);
    printf("WaitForEvent 0x%08lx\n", hr);
    control->lpVtbl->Execute(control, DEBUG_OUTCTL_THIS_CLIENT, argv[2], DEBUG_EXECUTE_ECHO);
    client->lpVtbl->DetachProcesses(client);
    return 0;
}
