/* dumpstack: reads a crash dump with Windows' own debugger engine (System32\dbgeng.dll) and prints the
 * stop, the faulting thread's stack and the loaded modules, with no debugger installed.
 *   dumpstack <dump file> [commands]   default commands: .lastevent; .bugcheck; .ecxr; kv 40; lm
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/dumpstack.c -o build/dumpstack.exe -ldbgeng -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>
#include <stdio.h>

static HRESULT STDMETHODCALLTYPE qi(IDebugOutputCallbacks *This, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDebugOutputCallbacks)) {
        *out = This;
        return S_OK;
    }
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
    if (argc < 2) {
        printf("usage: dumpstack <dump> [commands]\n");
        return 2;
    }
    IDebugClient *client = NULL;
    IDebugControl *control = NULL;
    HRESULT hr = DebugCreate(&IID_IDebugClient, (void **)&client);
    if (FAILED(hr)) {
        printf("DebugCreate 0x%08lx\n", hr);
        return 1;
    }
    client->lpVtbl->QueryInterface(client, &IID_IDebugControl, (void **)&control);
    client->lpVtbl->SetOutputCallbacks(client, &cb);
    hr = client->lpVtbl->OpenDumpFile(client, argv[1]);
    if (FAILED(hr)) {
        printf("OpenDumpFile 0x%08lx\n", hr);
        return 1;
    }
    hr = control->lpVtbl->WaitForEvent(control, 0, INFINITE);
    if (FAILED(hr)) printf("WaitForEvent 0x%08lx\n", hr);
    const char *cmds = argc > 2 ? argv[2] : ".lastevent; .bugcheck; .ecxr; kv 40; lm";
    control->lpVtbl->Execute(control, DEBUG_OUTCTL_THIS_CLIENT, cmds, DEBUG_EXECUTE_ECHO);
    client->lpVtbl->EndSession(client, DEBUG_END_PASSIVE);
    return 0;
}
