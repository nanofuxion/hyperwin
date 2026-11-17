/* HyperWin guest agent: a Windows service that gives the Android app a channel into the guest.
 *
 * AVF's crosvm on the target phone has no virtio-net, so networking is a Wintun adapter here whose
 * IP packets travel over virtio-vsock to the app, which NATs them onto Android sockets. The same
 * connection runs commands and receives files (driver installs, tests).
 *
 * The app connects to vsock port HW_VSOCK_PORT_AGENT. Frames: u32 payload length, u8 type, payload. */
#define WINVER 0x0A00          /* touch injection needs the Windows 8+ declarations */
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <objbase.h>
#include <shellapi.h>

#include "hw_abi.h"

#define MAX_FRAME (1u << 20)
#define FILE_DEVICE_SOCKET 0x0801
#define IOCTL_SOCKET_BIND CTL_CODE(FILE_DEVICE_SOCKET, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SOCKET_LISTEN CTL_CODE(FILE_DEVICE_SOCKET, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define AF_VSOCK_ 40

typedef struct { USHORT family, reserved; ULONG port, cid; } SockaddrVm;
typedef struct { ULONGLONG socket; ULONG type, pad; } VsockParams;
typedef struct { ULONG next; UCHAR flags, name_len; USHORT value_len; CHAR name[4]; } EaHeader;  /* 12 bytes */

/* Wintun, loaded from wintun.dll next to this program. */
typedef void *TunAdapter, *TunSession;
static TunAdapter (WINAPI *TunCreateAdapter)(LPCWSTR, LPCWSTR, const GUID *);
static TunSession (WINAPI *TunStartSession)(TunAdapter, DWORD);
static HANDLE (WINAPI *TunReadEvent)(TunSession);
static BYTE *(WINAPI *TunReceive)(TunSession, DWORD *);
static void (WINAPI *TunReleaseReceive)(TunSession, const BYTE *);
static BYTE *(WINAPI *TunAllocSend)(TunSession, DWORD);
static void (WINAPI *TunSend)(TunSession, const BYTE *);

static WCHAR home[MAX_PATH];          /* directory of this program */
static CRITICAL_SECTION write_lock, log_lock;
static volatile HANDLE conn;          /* current app connection, or NULL */
static TunSession tun;
static HANDLE push_file = INVALID_HANDLE_VALUE;
static SERVICE_STATUS_HANDLE svc_handle;

static void logf(const char *fmt, ...)
{
    WCHAR path[MAX_PATH];
    char line[600];
    SYSTEMTIME t;
    HANDLE f;
    DWORD n, wrote;
    va_list ap;
    GetLocalTime(&t);
    n = (DWORD)snprintf(line, sizeof line, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    n += (DWORD)vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (n > sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    swprintf(path, MAX_PATH, L"%ls\\agent.log", home);
    EnterCriticalSection(&log_lock);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, line, n, &wrote, NULL); CloseHandle(f); }
    LeaveCriticalSection(&log_lock);
}

/* ---- vsock through the viosock driver's device ---- */

static NTSTATUS open_socket(HANDLE parent, HANDLE *out)
{
    static WCHAR name[] = L"\\??\\Viosock";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    UCHAR ea[sizeof(EaHeader) + sizeof(VsockParams)];
    EaHeader *h = (EaHeader *)ea;
    VsockParams p = {(ULONGLONG)(ULONG_PTR)parent, 1, 0};
    memset(ea, 0, sizeof ea);
    h->name_len = 3;                       /* viosock reads the value right after a 3-byte name */
    h->value_len = sizeof p;
    memcpy(ea + sizeof(EaHeader), &p, sizeof p);
    us.Buffer = name;
    us.Length = sizeof name - sizeof(WCHAR);
    us.MaximumLength = sizeof name;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* No FILE_SYNCHRONOUS_IO: reads and writes run from different threads. */
    return NtCreateFile(out, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, ea, sizeof ea);
}

static BOOL sock_ioctl(HANDLE h, DWORD code, void *in, DWORD len)
{
    OVERLAPPED ov = {0};
    DWORD got;
    BOOL ok;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = DeviceIoControl(h, code, in, len, NULL, 0, &got, &ov) ||
         (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(h, &ov, &got, TRUE));
    CloseHandle(ov.hEvent);
    return ok;
}

static BOOL sock_read(HANDLE h, void *buf, DWORD len)
{
    BYTE *p = buf;
    OVERLAPPED ov = {0};
    BOOL ok = TRUE;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (len && ok) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        ok = ReadFile(h, p, len, &got, &ov) || (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(h, &ov, &got, TRUE));
        if (ok && !got) ok = FALSE;
        p += got;
        len -= got;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static BOOL sock_write(HANDLE h, const void *buf, DWORD len)
{
    const BYTE *p = buf;
    OVERLAPPED ov = {0};
    BOOL ok = TRUE;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (len && ok) {
        DWORD put = 0;
        ResetEvent(ov.hEvent);
        ok = WriteFile(h, p, len, &put, &ov) || (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(h, &ov, &put, TRUE));
        if (ok && !put) ok = FALSE;
        p += put;
        len -= put;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static void send_frame(BYTE type, const void *a, DWORD alen, const void *b, DWORD blen)
{
    BYTE hdr[5];
    DWORD len = alen + blen;
    HANDLE h;
    memcpy(hdr, &len, 4);
    hdr[4] = type;
    EnterCriticalSection(&write_lock);
    h = conn;
    if (h && sock_write(h, hdr, 5) && (!alen || sock_write(h, a, alen)) && blen) sock_write(h, b, blen);
    LeaveCriticalSection(&write_lock);
}

/* ---- network: Wintun adapter <-> app ---- */

static DWORD run_wait(WCHAR *cmdline)
{
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return GetLastError();
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static DWORD WINAPI tun_reader(void *arg)
{
    HANDLE ev = TunReadEvent(tun);
    (void)arg;
    for (;;) {
        DWORD size;
        BYTE *pkt = TunReceive(tun, &size);
        if (pkt) {
            if (conn) send_frame(HW_AGENT_PACKET, pkt, size, NULL, 0);
            TunReleaseReceive(tun, pkt);
        } else if (GetLastError() == ERROR_NO_MORE_ITEMS) {
            WaitForSingleObject(ev, INFINITE);
        } else {
            logf("wintun receive failed %lu", GetLastError());
            return 1;
        }
    }
}

static void start_network(void)
{
    static const GUID id = {0x48575745, 0x4e45, 0x5431, {0x80, 0x01, 0x48, 0x59, 0x50, 0x45, 0x52, 0x57}};
    WCHAR path[MAX_PATH];
    HMODULE dll;
    TunAdapter adapter;
    swprintf(path, MAX_PATH, L"%ls\\wintun.dll", home);
    dll = LoadLibraryW(path);
    if (!dll) { logf("wintun.dll not loaded: %lu", GetLastError()); return; }
#define SYM(var, name) *(FARPROC *)&var = GetProcAddress(dll, name)
    SYM(TunCreateAdapter, "WintunCreateAdapter");
    SYM(TunStartSession, "WintunStartSession");
    SYM(TunReadEvent, "WintunGetReadWaitEvent");
    SYM(TunReceive, "WintunReceivePacket");
    SYM(TunReleaseReceive, "WintunReleaseReceivePacket");
    SYM(TunAllocSend, "WintunAllocateSendPacket");
    SYM(TunSend, "WintunSendPacket");
#undef SYM
    adapter = TunCreateAdapter(L"HyperWin", L"HyperWin", &id);
    if (!adapter) { logf("wintun adapter failed: %lu", GetLastError()); return; }
    tun = TunStartSession(adapter, 0x400000);
    if (!tun) { logf("wintun session failed: %lu", GetLastError()); return; }
    CloseHandle(CreateThread(NULL, 0, tun_reader, NULL, 0, NULL));
    logf("network up");
}

/* The app decides the addresses: its NAT subnet, or the phone's own LAN address in LAN mode. */
static char wanted_net[160];
static HANDLE net_event;

/* netsh takes seconds, so this runs beside the packet loop; the newest request wins. */
static DWORD WINAPI network_config_thread(void *arg)
{
    char applied[160] = "", cfg[160], ip[40], mask[40], gw[40], dns[40];
    WCHAR cmd[512];
    int mtu;
    (void)arg;
    for (;;) {
    WaitForSingleObject(net_event, INFINITE);
    EnterCriticalSection(&write_lock);
    strcpy(cfg, wanted_net);
    LeaveCriticalSection(&write_lock);
    mtu = 1500;
    if (!strcmp(cfg, applied) || sscanf(cfg, "%39s %39s %39s %39s %d", ip, mask, gw, dns, &mtu) != 5) continue;
    swprintf(cmd, 512, L"netsh interface ipv4 set address name=\"HyperWin\" static %hs %hs %hs", ip, mask, gw);
    logf("netsh address %s -> %lu", ip, run_wait(cmd));
    swprintf(cmd, 512, L"netsh interface ipv4 set dnsservers name=\"HyperWin\" static %hs primary validate=no", dns);
    logf("netsh dns %s -> %lu", dns, run_wait(cmd));
    swprintf(cmd, 512, L"netsh interface ipv4 set subinterface \"HyperWin\" mtu=%d store=active", mtu);
    logf("netsh mtu %d -> %lu", mtu, run_wait(cmd));
    strcpy(applied, cfg);
    }
}

static void configure_network(const char *text, DWORD len)
{
    if (len >= sizeof wanted_net) return;
    EnterCriticalSection(&write_lock);
    memcpy(wanted_net, text, len);
    wanted_net[len] = 0;
    LeaveCriticalSection(&write_lock);
    SetEvent(net_event);
}

/* Reports which ports Windows programs listen on, so the app can accept LAN traffic for them. */
static DWORD WINAPI port_watcher(void *arg)
{
    static BYTE table[1 << 18], msg[8192], last[8192];
    DWORD last_len = 0;
    (void)arg;
    for (;; Sleep(2000)) {
        DWORD size = sizeof table, i, n = 0, len = 2;
        USHORT count = 0;
        if (!conn) { last_len = 0; continue; }
        if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_BASIC_LISTENER, 0) == NO_ERROR) {
            MIB_TCPTABLE *t = (MIB_TCPTABLE *)table;
            for (i = 0; i < t->dwNumEntries && len + 2 < sizeof msg / 2; i++) {
                USHORT port = (USHORT)(((t->table[i].dwLocalPort & 0xff) << 8) | ((t->table[i].dwLocalPort >> 8) & 0xff));
                if ((t->table[i].dwLocalAddr & 0xff) == 127) continue;
                memcpy(msg + len, &port, 2);
                len += 2;
                count++;
            }
        }
        memcpy(msg, &count, 2);
        n = len;
        len += 2;
        count = 0;
        size = sizeof table;
        if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_BASIC, 0) == NO_ERROR) {
            MIB_UDPTABLE *t = (MIB_UDPTABLE *)table;
            for (i = 0; i < t->dwNumEntries && len + 2 < sizeof msg; i++) {
                USHORT port = (USHORT)(((t->table[i].dwLocalPort & 0xff) << 8) | ((t->table[i].dwLocalPort >> 8) & 0xff));
                if ((t->table[i].dwLocalAddr & 0xff) == 127) continue;
                memcpy(msg + len, &port, 2);
                len += 2;
                count++;
            }
        }
        memcpy(msg + n, &count, 2);
        if (len != last_len || memcmp(msg, last, len)) {
            send_frame(HW_AGENT_PORTS, msg, len, NULL, 0);
            memcpy(last, msg, len);
            last_len = len;
        }
    }
}

/* ---- touch screen ----
 * Touch can only be injected from inside the user's session, and the service lives in session 0.
 * So the service keeps a copy of this program running there ("hwagent.exe touch", as SYSTEM so it
 * also reaches elevated windows) and feeds it the app's contact frames through its standard input. */

#define TOUCH_RECORD (1 + 5 * HW_AGENT_TOUCH_MAX)

static CRITICAL_SECTION touch_lock;
static HANDLE touch_pipe, touch_proc;
static DWORD touch_session = 0xffffffff;

static void start_touch_helper(void)
{
    static WCHAR desktop[] = L"winsta0\\default";
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    HANDLE self = NULL, token = NULL, rd = NULL, wr = NULL;
    WCHAR cmd[MAX_PATH + 32];
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xffffffff) return;
    if (touch_proc && session == touch_session && WaitForSingleObject(touch_proc, 0) == WAIT_TIMEOUT) return;
    EnterCriticalSection(&touch_lock);
    if (touch_pipe) CloseHandle(touch_pipe);       /* end of input stops an old helper */
    touch_pipe = NULL;
    LeaveCriticalSection(&touch_lock);
    if (touch_proc) CloseHandle(touch_proc);
    touch_proc = NULL;
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\\hwagent.exe\" touch", home);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self) &&
        DuplicateTokenEx(self, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &token) &&
        SetTokenInformation(token, TokenSessionId, &session, sizeof session) &&
        CreatePipe(&rd, &wr, &sa, 0)) {
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.lpDesktop = desktop;
        if (CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            touch_proc = pi.hProcess;
            touch_session = session;
            EnterCriticalSection(&touch_lock);
            touch_pipe = wr;
            LeaveCriticalSection(&touch_lock);
            wr = NULL;
            logf("touch helper started in session %lu", session);
        } else {
            logf("touch helper failed %lu", GetLastError());
        }
    } else {
        logf("touch helper token failed %lu", GetLastError());
    }
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    if (token) CloseHandle(token);
    if (self) CloseHandle(self);
}

static DWORD WINAPI touch_watcher(void *arg)
{
    (void)arg;
    for (;; Sleep(2000)) start_touch_helper();
}

static void forward_touch(const BYTE *p, DWORD len)
{
    BYTE rec[TOUCH_RECORD] = {0};
    DWORD wrote;
    if (!len || p[0] > HW_AGENT_TOUCH_MAX || len < 1 + 5u * p[0]) return;
    memcpy(rec, p, 1 + 5u * p[0]);
    EnterCriticalSection(&touch_lock);
    if (touch_pipe) WriteFile(touch_pipe, rec, sizeof rec, &wrote, NULL);
    LeaveCriticalSection(&touch_lock);
}

/* The helper: reads contact frames, injects them. */

typedef struct { BYTE n, id[HW_AGENT_TOUCH_MAX]; USHORT x[HW_AGENT_TOUCH_MAX], y[HW_AGENT_TOUCH_MAX]; } Contacts;

static Contacts touch_queue[64];
static volatile LONG touch_head, touch_tail;
static HANDLE touch_event;

static DWORD WINAPI touch_reader(void *arg)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    (void)arg;
    for (;;) {
        BYTE rec[TOUCH_RECORD];
        DWORD have = 0, got, i;
        Contacts *c = &touch_queue[touch_head & 63];
        while (have < sizeof rec) {
            if (!ReadFile(in, rec + have, sizeof rec - have, &got, NULL) || !got) ExitProcess(0);   /* service went away */
            have += got;
        }
        c->n = rec[0] > HW_AGENT_TOUCH_MAX ? HW_AGENT_TOUCH_MAX : rec[0];
        for (i = 0; i < c->n; i++) {
            c->id[i] = rec[1 + 5 * i];
            memcpy(&c->x[i], rec + 2 + 5 * i, 2);
            memcpy(&c->y[i], rec + 4 + 5 * i, 2);
        }
        InterlockedIncrement(&touch_head);
        SetEvent(touch_event);
    }
}

/* Touch injection works on the user's desktop only. On the sign-in or consent screen the first
 * finger drives the mouse instead, which SYSTEM may do once its thread is on that desktop. */
static BOOL on_user_desktop(void)
{
    static HDESK current;
    WCHAR name[64] = L"";
    DWORD need;
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!d) return TRUE;
    GetUserObjectInformationW(d, UOI_NAME, name, sizeof name, &need);
    if (SetThreadDesktop(d)) {
        if (current) CloseDesktop(current);
        current = d;
    } else {
        CloseDesktop(d);
    }
    return !_wcsicmp(name, L"Default");
}

static void inject(const Contacts *was, const Contacts *now, BOOL touch)
{
    POINTER_TOUCH_INFO info[2 * HW_AGENT_TOUCH_MAX];
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN), n = 0, i, j;
    if (!touch) {
        INPUT in = {0};
        const Contacts *c = now->n ? now : was;
        if (!c->n) return;
        in.type = INPUT_MOUSE;
        in.mi.dx = c->x[0];
        in.mi.dy = c->y[0];
        in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE |
            (now->n && !was->n ? MOUSEEVENTF_LEFTDOWN : 0) | (!now->n && was->n ? MOUSEEVENTF_LEFTUP : 0);
        SendInput(1, &in, sizeof in);
        return;
    }
    memset(info, 0, sizeof info);
    for (i = 0; i < now->n + was->n; i++) {
        const Contacts *c = i < now->n ? now : was;
        int k = i < now->n ? i : i - now->n;
        const Contacts *other = i < now->n ? was : now;
        BOOL both = FALSE;
        POINTER_TOUCH_INFO *t = &info[n];
        for (j = 0; j < other->n; j++) if (other->id[j] == c->id[k]) both = TRUE;
        if (c == was && both) continue;                      /* still down: listed from now */
        t->pointerInfo.pointerType = PT_TOUCH;
        t->pointerInfo.pointerId = c->id[k];
        t->pointerInfo.ptPixelLocation.x = c->x[k] * w / 65536;
        t->pointerInfo.ptPixelLocation.y = c->y[k] * h / 65536;
        t->pointerInfo.pointerFlags = c == was ? POINTER_FLAG_UP
            : (both ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN) | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        t->touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        t->pressure = 512;
        t->rcContact.left = t->pointerInfo.ptPixelLocation.x - 2;
        t->rcContact.right = t->pointerInfo.ptPixelLocation.x + 2;
        t->rcContact.top = t->pointerInfo.ptPixelLocation.y - 2;
        t->rcContact.bottom = t->pointerInfo.ptPixelLocation.y + 2;
        n++;
    }
    if (n && !InjectTouchInput((UINT32)n, info)) {
        static DWORD logged;
        DWORD err = GetLastError();
        if (err != logged) { logged = err; logf("touch inject failed %lu", err); }
    }
}

static int touch_helper(void)
{
    Contacts was = {0};
    BOOL touch = TRUE;
    SetProcessDPIAware();
    if (!InitializeTouchInjection(HW_AGENT_TOUCH_MAX, TOUCH_FEEDBACK_DEFAULT)) logf("touch init failed %lu", GetLastError());
    touch_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, touch_reader, NULL, 0, NULL));
    for (;;) {
        /* A finger that rests must be repeated, or Windows lifts it after a moment. */
        if (WaitForSingleObject(touch_event, was.n ? 50 : INFINITE) == WAIT_TIMEOUT) inject(&was, &was, touch);
        while (touch_tail != touch_head) {
            Contacts now = touch_queue[touch_tail & 63];
            touch_tail++;
            if (!was.n && now.n) touch = on_user_desktop();
            inject(&was, &now, touch);
            was = now;
        }
    }
}

/* ---- Windows' own keyboard ----
 * "hwagent.exe keyboard", run as the signed-in user, shows or hides the Windows touch keyboard,
 * which docks along the bottom of the screen. Windows offers no command for that, only this COM
 * call on the keyboard's host (TabTip.exe). Without a touch keyboard, the classic one opens. */

static int toggle_windows_keyboard(void)
{
    static const GUID host = {0x4ce576fa, 0x83dc, 0x4f88, {0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76}};
    static const GUID invocation = {0x37c994e7, 0x432b, 0x4834, {0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b}};
    typedef struct Tip Tip;
    struct TipVtbl {
        HRESULT (WINAPI *QueryInterface)(Tip *, const GUID *, void **);
        ULONG (WINAPI *AddRef)(Tip *);
        ULONG (WINAPI *Release)(Tip *);
        HRESULT (WINAPI *Toggle)(Tip *, HWND);
    };
    struct Tip { const struct TipVtbl *v; };
    int tries;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    for (tries = 0; tries < 3; tries++) {
        Tip *tip = NULL;
        HRESULT hr = CoCreateInstance(&host, NULL, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, &invocation, (void **)&tip);
        if (SUCCEEDED(hr) && tip) {
            hr = tip->v->Toggle(tip, GetDesktopWindow());
            tip->v->Release(tip);
            if (SUCCEEDED(hr)) return 0;
        }
        if (tries == 0)      /* the host is not running yet: start it and ask again */
            ShellExecuteW(NULL, L"open", L"C:\\Program Files\\Common Files\\microsoft shared\\ink\\TabTip.exe", NULL, NULL, SW_SHOWNORMAL);
        Sleep(1200);
    }
    logf("touch keyboard not available, opening the classic one");
    ShellExecuteW(NULL, L"open", L"osk.exe", NULL, NULL, SW_SHOWNORMAL);
    return 1;
}

/* ---- commands ---- */

typedef struct { DWORD id; BYTE mode; WCHAR cmd[1]; } Exec;

/* Starts cmd on the signed-in user's desktop. */
static DWORD launch_in_session(WCHAR *cmd, BOOL hidden)
{
    HANDLE token = NULL, primary = NULL;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    void *env = NULL;
    DWORD err = 0;
    static WCHAR desktop[] = L"winsta0\\default";
    if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) return GetLastError();
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &primary)) {
        err = GetLastError();
        CloseHandle(token);
        return err;
    }
    CreateEnvironmentBlock(&env, primary, FALSE);
    si.lpDesktop = desktop;
    if (CreateProcessAsUserW(primary, NULL, cmd, NULL, NULL, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | (hidden ? CREATE_NO_WINDOW : 0), env, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        err = GetLastError();
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(primary);
    CloseHandle(token);
    return err;
}

static DWORD WINAPI exec_thread(void *arg)
{
    Exec *e = arg;
    DWORD code = 0;
    if (e->mode == HW_AGENT_EXEC_DESKTOP || e->mode == HW_AGENT_EXEC_HIDDEN) {
        code = launch_in_session(e->cmd, e->mode == HW_AGENT_EXEC_HIDDEN);
    } else {
        SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        HANDLE rd, wr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) { code = GetLastError(); goto done; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        if (!CreateProcessW(NULL, e->cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, home, &si, &pi)) {
            code = GetLastError();
            CloseHandle(rd);
            CloseHandle(wr);
            goto done;
        }
        CloseHandle(wr);
        for (;;) {
            char buf[4096];
            DWORD got;
            if (!ReadFile(rd, buf, sizeof buf, &got, NULL) || !got) break;
            send_frame(HW_AGENT_EXEC_OUT, &e->id, 4, buf, got);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
    }
done:
    send_frame(HW_AGENT_EXEC_DONE, &e->id, 4, &code, 4);
    HeapFree(GetProcessHeap(), 0, e);
    return 0;
}

static void on_frame(BYTE type, BYTE *p, DWORD len)
{
    if (type == HW_AGENT_PACKET) {
        /* A full ring means Windows is behind; wait rather than drop, the app's TCP side counts on it. */
        BYTE *out = NULL;
        int tries;
        for (tries = 0; tun && len && tries < 2000; tries++) {
            out = TunAllocSend(tun, len);
            if (out || GetLastError() != ERROR_BUFFER_OVERFLOW) break;
            Sleep(1);
        }
        if (out) { memcpy(out, p, len); TunSend(tun, out); }
    } else if (type == HW_AGENT_TOUCH) {
        forward_touch(p, len);
    } else if (type == HW_AGENT_CPU_MHZ && len == 4) {
        int cpu;
        for (cpu = 0; cpu < 256; cpu++) {
            WCHAR key[80];
            HKEY k;
            swprintf(key, 80, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", cpu);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &k)) break;
            RegSetValueExW(k, L"~MHz", 0, REG_DWORD, p, 4);
            RegCloseKey(k);
        }
    } else if (type == HW_AGENT_NET_CONFIG) {
        configure_network((const char *)p, len);
    } else if (type == HW_AGENT_EXEC && len > 5) {
        int chars = MultiByteToWideChar(CP_UTF8, 0, (char *)p + 5, (int)len - 5, NULL, 0);
        Exec *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e + (size_t)(chars + 1) * sizeof(WCHAR));
        if (!e) return;
        memcpy(&e->id, p, 4);
        e->mode = p[4];
        MultiByteToWideChar(CP_UTF8, 0, (char *)p + 5, (int)len - 5, e->cmd, chars);
        CloseHandle(CreateThread(NULL, 0, exec_thread, e, 0, NULL));
    } else if (type == HW_AGENT_FILE_OPEN && len) {
        WCHAR path[MAX_PATH];
        int chars = MultiByteToWideChar(CP_UTF8, 0, (char *)p, (int)len, path, MAX_PATH - 1);
        path[chars] = 0;
        if (push_file != INVALID_HANDLE_VALUE) CloseHandle(push_file);
        push_file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    } else if (type == HW_AGENT_FILE_DATA) {
        DWORD wrote;
        if (push_file != INVALID_HANDLE_VALUE && !WriteFile(push_file, p, len, &wrote, NULL)) {
            CloseHandle(push_file);
            push_file = INVALID_HANDLE_VALUE;
        }
    } else if (type == HW_AGENT_FILE_CLOSE) {
        DWORD ok = push_file != INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(push_file);
        push_file = INVALID_HANDLE_VALUE;
        send_frame(HW_AGENT_FILE_RESULT, &ok, 4, NULL, 0);
    }
}

static void serve(void)
{
    static BYTE buf[MAX_FRAME];
    for (;;) {
        HANDLE listener = NULL;
        SockaddrVm addr = {AF_VSOCK_, 0, HW_VSOCK_PORT_AGENT, 0xffffffffu};
        ULONG backlog = 1;
        NTSTATUS s = open_socket(NULL, &listener);
        if (s < 0) { logf("vsock open failed %08lx", (unsigned long)s); Sleep(2000); continue; }
        if (!sock_ioctl(listener, IOCTL_SOCKET_BIND, &addr, sizeof addr) ||
            !sock_ioctl(listener, IOCTL_SOCKET_LISTEN, &backlog, sizeof backlog)) {
            logf("vsock bind/listen failed %lu", GetLastError());
            CloseHandle(listener);
            Sleep(2000);
            continue;
        }
        logf("listening on vsock %d", HW_VSOCK_PORT_AGENT);
        for (;;) {
            HANDLE c = NULL;
            BYTE hello[2] = {HW_AGENT_VERSION, 0};
            s = open_socket(listener, &c);
            if (s < 0) { logf("accept failed %08lx", (unsigned long)s); break; }
            hello[1] = tun != NULL;
            conn = c;
            send_frame(HW_AGENT_HELLO, hello, sizeof hello, NULL, 0);
            for (;;) {
                BYTE hdr[5];
                DWORD len;
                if (!sock_read(c, hdr, 5)) break;
                memcpy(&len, hdr, 4);
                if (len > MAX_FRAME || (len && !sock_read(c, buf, len))) break;
                on_frame(hdr[4], buf, len);
            }
            EnterCriticalSection(&write_lock);
            conn = NULL;
            LeaveCriticalSection(&write_lock);
            CloseHandle(c);
        }
        CloseHandle(listener);
    }
}

static void setup(void)
{
    WCHAR *slash;
    GetModuleFileNameW(NULL, home, MAX_PATH);
    slash = wcsrchr(home, L'\\');
    if (slash) *slash = 0;
    InitializeCriticalSection(&write_lock);
    InitializeCriticalSection(&log_lock);
    InitializeCriticalSection(&touch_lock);
}

static void run(void)
{
    setup();
    logf("agent %d starting", HW_AGENT_VERSION);
    start_network();
    net_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, network_config_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, port_watcher, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, touch_watcher, NULL, 0, NULL));
    serve();
}

static DWORD WINAPI svc_ctrl(DWORD control, DWORD type, void *data, void *ctx)
{
    (void)type; (void)data; (void)ctx;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, SERVICE_STOPPED, 0, 0, 0, 0, 0};
        SetServiceStatus(svc_handle, &st);
        ExitProcess(0);
    }
    return NO_ERROR;
}

static void WINAPI svc_main(DWORD argc, WCHAR **argv)
{
    SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN, 0, 0, 0, 0};
    (void)argc; (void)argv;
    svc_handle = RegisterServiceCtrlHandlerExW(L"HyperWinAgent", svc_ctrl, NULL);
    SetServiceStatus(svc_handle, &st);
    run();
}

int main(int argc, char **argv)
{
    static WCHAR name[] = L"HyperWinAgent";
    if (argc > 1 && !strcmp(argv[1], "touch")) { setup(); return touch_helper(); }
    if (argc > 1 && !strcmp(argv[1], "keyboard")) { setup(); return toggle_windows_keyboard(); }
    SERVICE_TABLE_ENTRYW table[] = {{name, svc_main}, {NULL, NULL}};
    if (!StartServiceCtrlDispatcherW(table)) run();   /* started by hand, not by the service manager */
    return 0;
}
