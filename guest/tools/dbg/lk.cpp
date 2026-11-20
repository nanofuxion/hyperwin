// Local kernel debug driver for tests: lk.exe <script> <log>. Runs each line of the script in the local kernel.
#define INITGUID
#include <windows.h>
#include <dbgeng.h>
#include <stdio.h>
#include <string>
static FILE *g_out;
struct Out : IDebugOutputCallbacks {
    STDMETHODIMP QueryInterface(REFIID, void **p) { *p = this; return S_OK; }
    STDMETHODIMP_(ULONG) AddRef() { return 1; }
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP Output(ULONG, PCSTR t) { fputs(t, g_out); fflush(g_out); return S_OK; }
};
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    g_out = fopen(argv[2], "w");
    setvbuf(g_out, nullptr, _IONBF, 0);
    HMODULE m = LoadLibraryA(argc > 3 ? argv[3] : "dbgeng.dll");
    if (!m) { fprintf(g_out, "LoadLibrary dbgeng failed %lu\n", GetLastError()); return 3; }
    typedef HRESULT(WINAPI * Create)(REFIID, void **);
    Create dc = (Create)GetProcAddress(m, "DebugCreate");
    IDebugClient5 *cl = nullptr;
    HRESULT hr = dc(__uuidof(IDebugClient5), (void **)&cl);
    fprintf(g_out, "DebugCreate 0x%lx\n", hr);
    if (FAILED(hr)) return 4;
    IDebugControl4 *ctl = nullptr;
    hr = cl->QueryInterface(__uuidof(IDebugControl4), (void **)&ctl);
    fprintf(g_out, "QI control 0x%lx %p\n", hr, (void *)ctl);
    Out o;

    if (argc > 4) hr = cl->OpenDumpFile(argv[4]); else hr = cl->AttachKernel(DEBUG_ATTACH_LOCAL_KERNEL, nullptr);
    fprintf(g_out, "AttachKernel 0x%lx\n", hr);
    hr = ctl->WaitForEvent(0, 120000);
    fprintf(g_out, "WaitForEvent 0x%lx\n", hr);
    { std::string lg = std::string(".logopen /u ") + argv[2] + ".dbg"; ctl->Execute(DEBUG_OUTCTL_THIS_CLIENT, lg.c_str(), DEBUG_EXECUTE_DEFAULT); }
    FILE *f = fopen(argv[1], "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        fprintf(g_out, "\n> %s\n", s.c_str());
        hr = ctl->Execute(DEBUG_OUTCTL_THIS_CLIENT, s.c_str(), DEBUG_EXECUTE_DEFAULT);
        fprintf(g_out, "  -> 0x%lx\n", hr);
    }
    ctl->Execute(DEBUG_OUTCTL_THIS_CLIENT, ".logclose", DEBUG_EXECUTE_DEFAULT);
    fprintf(g_out, "\nDONE\n");
    fclose(g_out);
    return 0;
}
