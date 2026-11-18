/* HyperWin phone storage: shows each storage volume the phone shares (internal storage, SD cards,
 * USB drives) as a Windows drive.
 *
 * A WinFsp file system per volume. Every request goes to the phone through the storage slots in
 * the shared window (include/hw_files.h), which hwgpu.sys maps into this process. Big reads and
 * writes are split over several slots, and the phone serves those in parallel.
 *
 * Runs as the SYSTEM service HyperWinFiles, so its drives show for every user. WinFsp is loaded
 * from where its installer put it; without WinFsp, or before the phone shares anything, it waits.
 */
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <assert.h>   /* static_assert, which WinFsp's headers use */
#include <stdio.h>
#include <winfsp/winfsp.h>
#include "hw_abi.h"
#include "hw_files.h"
#include "hwgpu_ioctl.h"

#define MAX_PARALLEL 16u         /* slots one read or write may use at once */
#define AHEAD_WINDOWS 2u         /* read-ahead per file: this many slots of what comes next */
#define AHEAD_MAX 8              /* read-ahead slots for all files together */
#define AHEAD_BELOW (128u << 10) /* only reads smaller than this start read-ahead */
#define DRIVE_PREFERRED L'P'     /* internal storage, "P" for phone */

/* WinFsp, loaded at run time from its install folder. */
static NTSTATUS (*fsp_create)(PWSTR, const FSP_FSCTL_VOLUME_PARAMS *, const FSP_FILE_SYSTEM_INTERFACE *, FSP_FILE_SYSTEM **);
static VOID (*fsp_delete)(FSP_FILE_SYSTEM *);
static NTSTATUS (*fsp_set_mount_point)(FSP_FILE_SYSTEM *, PWSTR);
static NTSTATUS (*fsp_start)(FSP_FILE_SYSTEM *, ULONG);
static VOID (*fsp_stop)(FSP_FILE_SYSTEM *);
static BOOLEAN (*fsp_acquire_dir)(PVOID *, BOOLEAN, PNTSTATUS);
static BOOLEAN (*fsp_fill_dir)(PVOID *, FSP_FSCTL_DIR_INFO *, PNTSTATUS);
static VOID (*fsp_release_dir)(PVOID *);
static VOID (*fsp_read_dir)(PVOID *, PWSTR, PVOID, ULONG, PULONG);
static VOID (*fsp_delete_dir)(PVOID *);

static void *region;             /* the storage slots, mapped by hwgpu.sys */
static PSECURITY_DESCRIPTOR sd;  /* everyone may do anything: the phone decides what is allowed */
static ULONG sd_size;
static SERVICE_STATUS_HANDLE svc_handle;

struct volume {
    uint32_t id;
    FSP_FILE_SYSTEM *fs;
    WCHAR drive[3];
};
static struct volume volumes[HWF_VOLUMES];

struct file {
    uint64_t handle;
    int dir;
    PVOID dir_buffer;
    /* Read-ahead: small reads in a row get the following data fetched before they ask. */
    SRWLOCK lock;
    uint64_t next;                    /* where the last read ended */
    unsigned ahead_n;
    int ahead[AHEAD_WINDOWS];         /* slots, in file order, each HWF_DATA long */
    uint64_t ahead_off;               /* file offset of the first */
    LONG ahead_gen;                   /* write_gen when they were fetched */
    WCHAR path[];
};
static volatile LONG ahead_total;
/* Bumped by every change to any file, so read-ahead never serves data from before it. */
static volatile LONG write_gen;
static void ahead_forget(struct file *f);
static void ahead_drop(struct file *f);

static int load_winfsp(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH + 40];
    DWORD size = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, NULL, dir,
                     &size) != ERROR_SUCCESS &&
        (size = sizeof dir, RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, NULL,
                                         dir, &size)) != ERROR_SUCCESS)
        return 0;
    swprintf(path, sizeof path / sizeof *path, L"%lsbin\\winfsp-a64.dll", dir);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return 0;
#define GET(var, name) if (!(*(FARPROC *)&var = GetProcAddress(m, name))) return 0
    GET(fsp_create, "FspFileSystemCreate");
    GET(fsp_delete, "FspFileSystemDelete");
    GET(fsp_set_mount_point, "FspFileSystemSetMountPoint");
    GET(fsp_start, "FspFileSystemStartDispatcher");
    GET(fsp_stop, "FspFileSystemStopDispatcher");
    GET(fsp_acquire_dir, "FspFileSystemAcquireDirectoryBuffer");
    GET(fsp_fill_dir, "FspFileSystemFillDirectoryBuffer");
    GET(fsp_release_dir, "FspFileSystemReleaseDirectoryBuffer");
    GET(fsp_read_dir, "FspFileSystemReadDirectoryBuffer");
    GET(fsp_delete_dir, "FspFileSystemDeleteDirectoryBuffer");
#undef GET
    return 1;
}

static int map_region(void)
{
    HANDLE h = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    struct hwgpu_map_out out = {0};
    DWORD got = 0;
    /* The handle stays open: closing it would unmap the slots. */
    if (!DeviceIoControl(h, IOCTL_HWGPU_FILES, NULL, 0, &out, sizeof out, &got, NULL) || !out.user) {
        CloseHandle(h);
        return 0;
    }
    region = (void *)(ULONG_PTR)out.user;
    return 1;
}

/* ---- slots ---- */

/* Waiting on the phone: it answers most requests in tens of microseconds, so yield first, then
 * sleep in short steps. */
static void pause_for(unsigned *misses)
{
    static __thread HANDLE timer;
    if (++*misses < 2000) {
        SwitchToThread();
        return;
    }
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = *misses < 4000 ? -2000 : -10000;  /* 200 us, then 1 ms */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(1);
}

static int try_claim(void)
{
    for (uint32_t i = 0; i < HWF_SLOTS; i++) {
        uint32_t expected = HWF_FREE;
        if (__atomic_compare_exchange_n(&hwf_slot(region, i)->state, &expected, HWF_CLAIMED, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return (int)i;
    }
    return -1;
}

/* A slot with its request fields cleared. */
static struct hwf_slot *begin(int *index, uint32_t op, uint32_t volume)
{
    unsigned misses = 0;
    int i;
    while ((i = try_claim()) < 0) pause_for(&misses);
    struct hwf_slot *s = hwf_slot(region, (uint32_t)i);
    s->op = op;
    s->volume = volume;
    s->flags = 0;
    s->handle = s->offset = s->length = 0;
    s->a = s->b = 0;
    s->path[0] = s->path2[0] = 0;
    *index = i;
    return s;
}

static void submit(struct hwf_slot *s) { __atomic_store_n(&s->state, HWF_REQUEST, __ATOMIC_RELEASE); }

static int wait_done(struct hwf_slot *s)
{
    unsigned misses = 0;
    while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HWF_DONE) pause_for(&misses);
    return s->err;
}

static void end(struct hwf_slot *s) { __atomic_store_n(&s->state, HWF_FREE, __ATOMIC_RELEASE); }

static int call(struct hwf_slot *s)
{
    submit(s);
    return wait_done(s);
}

static int put_path(char *dst, PCWSTR src)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, HWF_PATH, NULL, NULL);
    return n > 0;
}

static NTSTATUS status_of(int err)
{
    switch (err) {
    case 0: return STATUS_SUCCESS;
    case 2: return STATUS_OBJECT_NAME_NOT_FOUND;      /* ENOENT */
    case 1: case 13: return STATUS_ACCESS_DENIED;      /* EPERM, EACCES */
    case 17: return STATUS_OBJECT_NAME_COLLISION;      /* EEXIST */
    case 18: return STATUS_NOT_SAME_DEVICE;            /* EXDEV */
    case 20: return STATUS_NOT_A_DIRECTORY;            /* ENOTDIR */
    case 21: return STATUS_FILE_IS_A_DIRECTORY;        /* EISDIR */
    case 22: return STATUS_INVALID_PARAMETER;          /* EINVAL */
    case 24: case 23: return STATUS_TOO_MANY_OPENED_FILES;
    case 27: return STATUS_FILE_TOO_LARGE;             /* EFBIG */
    case 28: return STATUS_DISK_FULL;                  /* ENOSPC */
    case 30: return STATUS_MEDIA_WRITE_PROTECTED;      /* EROFS */
    case 36: return STATUS_OBJECT_NAME_INVALID;        /* ENAMETOOLONG */
    case 39: return STATUS_DIRECTORY_NOT_EMPTY;        /* ENOTEMPTY */
    case 16: return STATUS_SHARING_VIOLATION;          /* EBUSY */
    case 19: case 6: return STATUS_NO_SUCH_DEVICE;     /* ENODEV, ENXIO: the volume went away */
    case 116: case 9: return STATUS_FILE_INVALID;      /* ESTALE, EBADF */
    default: return STATUS_UNEXPECTED_IO_ERROR;
    }
}

/* ---- file information ---- */

static UINT64 filetime(int64_t ns) { return ns > 0 ? (UINT64)(ns / 100) + 116444736000000000ull : 0; }
static int64_t unix_ns(UINT64 ft) { return ft > 116444736000000000ull ? (int64_t)(ft - 116444736000000000ull) * 100 : 0; }

static void to_info(const struct hwf_info *i, FSP_FSCTL_FILE_INFO *f)
{
    memset(f, 0, sizeof *f);
    f->FileAttributes = i->kind == HWF_KIND_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    f->FileSize = i->kind == HWF_KIND_DIR ? 0 : i->size;
    f->AllocationSize = (f->FileSize + 4095) & ~4095ull;
    /* Linux keeps no creation time; the last write is the closest. */
    f->CreationTime = filetime(i->mtime < i->ctime || !i->ctime ? i->mtime : i->ctime);
    f->LastAccessTime = filetime(i->atime);
    f->LastWriteTime = filetime(i->mtime);
    f->ChangeTime = filetime(i->ctime);
    f->IndexNumber = i->ino;
}

static uint32_t vol_of(FSP_FILE_SYSTEM *fs) { return ((struct volume *)fs->UserContext)->id; }

static NTSTATUS give_sd(PSECURITY_DESCRIPTOR dst, SIZE_T *size)
{
    if (!size) return STATUS_SUCCESS;
    if (*size < sd_size) {
        *size = sd_size;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (dst) memcpy(dst, sd, sd_size);
    *size = sd_size;
    return STATUS_SUCCESS;
}

/* ---- WinFsp callbacks ---- */

static NTSTATUS get_volume_info(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
    int i;
    struct hwf_slot *s = begin(&i, HWF_VOLINFO, vol_of(fs));
    int err = call(s);
    UINT64 total = (UINT64)s->out_a, free_bytes = (UINT64)s->out_b;
    end(s);
    if (err) return status_of(err);
    memset(vi, 0, sizeof *vi);
    vi->TotalSize = total;
    vi->FreeSize = free_bytes;
    struct hwf_hdr *h = hwf_hdr(region);
    for (uint32_t v = 0; v < HWF_VOLUMES; v++) {
        if (h->volume[v].id != vol_of(fs)) continue;
        char label[33];
        memcpy(label, h->volume[v].label, 32);
        label[32] = 0;
        int n = MultiByteToWideChar(CP_UTF8, 0, label, -1, vi->VolumeLabel, 32);
        vi->VolumeLabelLength = (UINT16)(n > 0 ? (n - 1) * sizeof(WCHAR) : 0);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS set_volume_label(FSP_FILE_SYSTEM *fs, PWSTR label, FSP_FSCTL_VOLUME_INFO *vi)
{
    (void)fs; (void)label; (void)vi;
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS get_security_by_name(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attributes,
                                     PSECURITY_DESCRIPTOR dst, SIZE_T *size)
{
    int i;
    struct hwf_slot *s = begin(&i, HWF_STAT, vol_of(fs));
    if (!put_path(s->path, name)) { end(s); return STATUS_OBJECT_NAME_INVALID; }
    int err = call(s);
    uint32_t kind = s->info.kind;
    end(s);
    if (err) return status_of(err);
    if (attributes) *attributes = kind == HWF_KIND_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    return give_sd(dst, size);
}

static struct file *new_file(PCWSTR name, uint64_t handle, int dir)
{
    size_t n = wcslen(name) + 1;
    struct file *f = calloc(1, sizeof *f + n * sizeof(WCHAR));
    if (!f) return NULL;
    f->handle = handle;
    f->dir = dir;
    f->next = ~0ull;
    InitializeSRWLock(&f->lock);
    memcpy(f->path, name, n * sizeof(WCHAR));
    return f;
}

static void close_handle(uint32_t volume, uint64_t handle)
{
    int i;
    struct hwf_slot *s = begin(&i, HWF_CLOSE, volume);
    s->handle = handle;
    call(s);
    end(s);
}

static NTSTATUS open_common(FSP_FILE_SYSTEM *fs, uint32_t op, uint32_t flags, PWSTR name, PVOID *ctx,
                            FSP_FSCTL_FILE_INFO *fi)
{
    int i;
    struct hwf_slot *s = begin(&i, op, vol_of(fs));
    s->flags = flags;
    if (!put_path(s->path, name)) { end(s); return STATUS_OBJECT_NAME_INVALID; }
    int err = call(s);
    uint64_t handle = s->out_handle;
    struct hwf_info info = s->info;
    end(s);
    if (err) return status_of(err);
    struct file *f = new_file(name, handle, info.kind == HWF_KIND_DIR);
    if (!f) {
        close_handle(vol_of(fs), handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    to_info(&info, fi);
    *ctx = f;
    return STATUS_SUCCESS;
}

static NTSTATUS create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 options, UINT32 access, UINT32 attributes,
                       PSECURITY_DESCRIPTOR desc, UINT64 allocation, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)access; (void)attributes; (void)desc; (void)allocation;
    return open_common(fs, HWF_CREATE, options & FILE_DIRECTORY_FILE ? HWF_DIR : 0, name, ctx, fi);
}

static NTSTATUS open_file(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 options, UINT32 access, PVOID *ctx,
                          FSP_FSCTL_FILE_INFO *fi)
{
    (void)options; (void)access;
    return open_common(fs, HWF_OPEN, 0, name, ctx, fi);
}

/* One request on an open file that answers with its information. */
static NTSTATUS file_call(FSP_FILE_SYSTEM *fs, struct file *f, uint32_t op, uint64_t offset, int64_t a, int64_t b,
                          FSP_FSCTL_FILE_INFO *fi)
{
    int i;
    struct hwf_slot *s = begin(&i, op, vol_of(fs));
    s->handle = f->handle;
    s->offset = offset;
    s->a = a;
    s->b = b;
    int err = call(s);
    struct hwf_info info = s->info;
    end(s);
    if (err) return status_of(err);
    if (fi) to_info(&info, fi);
    return STATUS_SUCCESS;
}

static NTSTATUS overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attributes, BOOLEAN replace, UINT64 allocation,
                          FSP_FSCTL_FILE_INFO *fi)
{
    (void)attributes; (void)replace; (void)allocation;
    ahead_forget(ctx);
    return file_call(fs, ctx, HWF_RESIZE, 0, 0, 0, fi);
}

static VOID cleanup(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, ULONG flags)
{
    struct file *f = ctx;
    if (!(flags & FspCleanupDelete) || !name) return;
    int i;
    struct hwf_slot *s = begin(&i, HWF_REMOVE, vol_of(fs));
    s->flags = f->dir ? HWF_DIR : 0;
    if (put_path(s->path, name)) call(s);
    end(s);
}

static VOID close_file(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
    struct file *f = ctx;
    AcquireSRWLockExclusive(&f->lock);
    ahead_drop(f);
    ReleaseSRWLockExclusive(&f->lock);
    close_handle(vol_of(fs), f->handle);
    if (f->dir_buffer) fsp_delete_dir(&f->dir_buffer);
    free(f);
}

/* Reads or writes [offset, offset + length) in chunks of up to HWF_DATA, several at once. */
static NTSTATUS transfer(FSP_FILE_SYSTEM *fs, struct file *f, int write, uint8_t *buf, uint64_t offset,
                         ULONG length, ULONG *moved, struct hwf_info *last)
{
    ULONG total = 0;
    while (total < length) {
        int idx[MAX_PARALLEL];
        struct hwf_slot *slot[MAX_PARALLEL];
        ULONG want[MAX_PARALLEL];
        unsigned n = 0;
        ULONG at = total;
        while (n < MAX_PARALLEL && at < length) {
            int i;
            if (n == 0) {
                slot[0] = begin(&i, 0, 0);
            } else if ((i = try_claim()) >= 0) {
                slot[n] = hwf_slot(region, (uint32_t)i);   /* holding some already: never wait for more */
            } else {
                break;
            }
            struct hwf_slot *s = slot[n];
            ULONG len = length - at < HWF_DATA ? length - at : HWF_DATA;
            s->op = write ? HWF_WRITE : HWF_READ;
            s->volume = vol_of(fs);
            s->flags = 0;
            s->handle = f->handle;
            s->offset = offset + at;
            s->length = len;
            if (write) memcpy(hwf_data(region, (uint32_t)i), buf + at, len);
            submit(s);
            idx[n] = i;
            want[n] = len;
            at += len;
            n++;
        }
        int err = 0, short_io = 0;
        for (unsigned k = 0; k < n; k++) {
            int e = wait_done(slot[k]);
            if (!err && !short_io) {
                if (e) {
                    err = e;
                } else {
                    ULONG got = slot[k]->out_length < want[k] ? (ULONG)slot[k]->out_length : want[k];
                    if (!write) memcpy(buf + total, hwf_data(region, (uint32_t)idx[k]), got);
                    if (write && last && slot[k]->info.size >= last->size) *last = slot[k]->info;
                    total += got;
                    if (got < want[k]) short_io = 1;
                }
            }
            end(slot[k]);
        }
        if (err && !total) return status_of(err);
        if (err || short_io) break;
    }
    *moved = total;
    return STATUS_SUCCESS;
}

/* Drops the first read-ahead window. Call with f->lock held. */
static void ahead_pop(struct file *f)
{
    struct hwf_slot *s = hwf_slot(region, (uint32_t)f->ahead[0]);
    wait_done(s);
    end(s);
    for (unsigned k = 1; k < f->ahead_n; k++) f->ahead[k - 1] = f->ahead[k];
    f->ahead_n--;
    f->ahead_off += HWF_DATA;
    InterlockedDecrement(&ahead_total);
}

static void ahead_drop(struct file *f)
{
    while (f->ahead_n) ahead_pop(f);
}

/* Starts fetching windows up to AHEAD_WINDOWS, from where the last one ends. */
static void ahead_fill(FSP_FILE_SYSTEM *fs, struct file *f)
{
    if (!f->ahead_n) f->ahead_gen = write_gen;
    while (f->ahead_n < AHEAD_WINDOWS) {
        if (f->ahead_n) {
            struct hwf_slot *last = hwf_slot(region, (uint32_t)f->ahead[f->ahead_n - 1]);
            /* Not past the end: a window that came back short holds the end of the file. */
            if (__atomic_load_n(&last->state, __ATOMIC_ACQUIRE) == HWF_DONE && (last->err || last->out_length < HWF_DATA))
                return;
        }
        if (InterlockedIncrement(&ahead_total) > AHEAD_MAX) {
            InterlockedDecrement(&ahead_total);
            return;
        }
        int i = try_claim();
        if (i < 0) {
            InterlockedDecrement(&ahead_total);
            return;
        }
        struct hwf_slot *s = hwf_slot(region, (uint32_t)i);
        s->op = HWF_READ;
        s->volume = vol_of(fs);
        s->flags = 0;
        s->handle = f->handle;
        s->offset = f->ahead_off + (uint64_t)f->ahead_n * HWF_DATA;
        s->length = HWF_DATA;
        submit(s);
        f->ahead[f->ahead_n++] = i;
    }
}

/* Serves a read from the read-ahead windows when they hold all of it. 1 when done. */
static int ahead_read(struct file *f, uint8_t *buf, uint64_t offset, ULONG length, ULONG *moved)
{
    if (!f->ahead_n || f->ahead_gen != write_gen || offset < f->ahead_off || offset + length > f->ahead_off + (uint64_t)f->ahead_n * HWF_DATA)
        return 0;
    ULONG got = 0;
    while (got < length) {
        uint64_t at = offset + got;
        unsigned k = (unsigned)((at - f->ahead_off) / HWF_DATA);
        struct hwf_slot *s = hwf_slot(region, (uint32_t)f->ahead[k]);
        if (wait_done(s)) return 0;
        uint64_t start = f->ahead_off + (uint64_t)k * HWF_DATA, rel = at - start;
        if (rel >= s->out_length) break;   /* the end of the file */
        ULONG n = (ULONG)(s->out_length - rel < length - got ? s->out_length - rel : length - got);
        memcpy(buf + got, hwf_data(region, (uint32_t)f->ahead[k]) + rel, n);
        got += n;
        if (s->out_length < HWF_DATA && rel + n >= s->out_length) break;
    }
    *moved = got;
    /* Windows read past the first window: it is used up. */
    while (f->ahead_n && offset + got >= f->ahead_off + HWF_DATA) ahead_pop(f);
    return 1;
}

static NTSTATUS read_file(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 offset, ULONG length, PULONG moved)
{
    struct file *f = ctx;
    NTSTATUS st = STATUS_SUCCESS;
    AcquireSRWLockExclusive(&f->lock);
    if (!ahead_read(f, buf, offset, length, moved)) {
        ahead_drop(f);
        int sequential = offset == f->next && length < AHEAD_BELOW;
        st = transfer(fs, f, 0, buf, offset, length, moved, NULL);
        if (NT_SUCCESS(st) && sequential && *moved == length) f->ahead_off = offset + length;
        else sequential = 0;
        if (sequential) ahead_fill(fs, f);
    } else {
        ahead_fill(fs, f);
    }
    if (NT_SUCCESS(st)) f->next = offset + *moved;
    ReleaseSRWLockExclusive(&f->lock);
    if (NT_SUCCESS(st) && !*moved && length) return STATUS_END_OF_FILE;
    return st;
}

/* A change to the file makes what was read ahead stale. */
static void ahead_forget(struct file *f)
{
    InterlockedIncrement(&write_gen);
    AcquireSRWLockExclusive(&f->lock);
    ahead_drop(f);
    f->next = ~0ull;
    ReleaseSRWLockExclusive(&f->lock);
}

static NTSTATUS write_file(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 offset, ULONG length,
                           BOOLEAN to_end, BOOLEAN constrained, PULONG moved, FSP_FSCTL_FILE_INFO *fi)
{
    struct file *f = ctx;
    ahead_forget(f);
    if (to_end || constrained) {
        FSP_FSCTL_FILE_INFO now;
        NTSTATUS st = file_call(fs, f, HWF_FSTAT, 0, 0, 0, &now);
        if (!NT_SUCCESS(st)) return st;
        if (to_end) offset = now.FileSize;
        if (constrained) {
            if (offset >= now.FileSize) {
                *moved = 0;
                *fi = now;
                return STATUS_SUCCESS;
            }
            if (offset + length > now.FileSize) length = (ULONG)(now.FileSize - offset);
        }
    }
    struct hwf_info last = {0};
    NTSTATUS st = transfer(fs, f, 1, buf, offset, length, moved, &last);
    if (!NT_SUCCESS(st)) return st;
    if (!*moved) return file_call(fs, f, HWF_FSTAT, 0, 0, 0, fi);
    to_info(&last, fi);
    return STATUS_SUCCESS;
}

static NTSTATUS flush(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
    if (!ctx) return STATUS_SUCCESS;   /* the whole volume: the phone writes through */
    return file_call(fs, ctx, HWF_SYNC, 0, 0, 0, fi);
}

static NTSTATUS get_file_info(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
    return file_call(fs, ctx, HWF_FSTAT, 0, 0, 0, fi);
}

static NTSTATUS set_basic_info(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attributes, UINT64 created, UINT64 accessed,
                               UINT64 written, UINT64 changed, FSP_FSCTL_FILE_INFO *fi)
{
    (void)attributes; (void)created; (void)changed;
    return file_call(fs, ctx, HWF_TIMES, 0, unix_ns(accessed), unix_ns(written), fi);
}

static NTSTATUS set_file_size(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT64 size, BOOLEAN allocation,
                              FSP_FSCTL_FILE_INFO *fi)
{
    ahead_forget(ctx);
    if (allocation) {
        /* Only space to reserve, unless it is below the end of the file. */
        NTSTATUS st = file_call(fs, ctx, HWF_FSTAT, 0, 0, 0, fi);
        if (!NT_SUCCESS(st) || size >= fi->FileSize) return st;
    }
    return file_call(fs, ctx, HWF_RESIZE, size, 0, 0, fi);
}

static NTSTATUS can_delete(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name)
{
    struct file *f = ctx;
    if (!f->dir) return STATUS_SUCCESS;
    int i;
    struct hwf_slot *s = begin(&i, HWF_LIST, vol_of(fs));
    s->length = 1;   /* one entry is enough to know */
    if (!put_path(s->path, name)) { end(s); return STATUS_OBJECT_NAME_INVALID; }
    int err = call(s);
    uint64_t count = s->out_a;
    end(s);
    if (err) return status_of(err);
    return count ? STATUS_DIRECTORY_NOT_EMPTY : STATUS_SUCCESS;
}

static NTSTATUS rename_file(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PWSTR new_name, BOOLEAN replace)
{
    (void)ctx;
    int i;
    struct hwf_slot *s = begin(&i, HWF_RENAME, vol_of(fs));
    s->flags = replace ? HWF_REPLACE : 0;
    if (!put_path(s->path, name) || !put_path(s->path2, new_name)) { end(s); return STATUS_OBJECT_NAME_INVALID; }
    int err = call(s);
    end(s);
    return status_of(err);
}

static NTSTATUS get_security(FSP_FILE_SYSTEM *fs, PVOID ctx, PSECURITY_DESCRIPTOR dst, SIZE_T *size)
{
    (void)fs; (void)ctx;
    return give_sd(dst, size);
}

static NTSTATUS set_security(FSP_FILE_SYSTEM *fs, PVOID ctx, SECURITY_INFORMATION what, PSECURITY_DESCRIPTOR change)
{
    (void)fs; (void)ctx; (void)what; (void)change;
    return STATUS_SUCCESS;
}

static int add_entry(PVOID *buffer, const char *name, size_t name_len, const struct hwf_info *info, NTSTATUS *st)
{
    union {
        FSP_FSCTL_DIR_INFO d;
        UINT8 bytes[sizeof(FSP_FSCTL_DIR_INFO) + 512 * sizeof(WCHAR)];
    } u;
    memset(&u.d, 0, sizeof u.d);
    int n = MultiByteToWideChar(CP_UTF8, 0, name, (int)name_len, u.d.FileNameBuf, 512);
    if (n <= 0) return 1;   /* a name Windows cannot show: leave it out */
    u.d.Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + n * sizeof(WCHAR));
    to_info(info, &u.d.FileInfo);
    return fsp_fill_dir(buffer, &u.d, st);
}

static NTSTATUS read_directory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker, PVOID buf, ULONG length,
                               PULONG moved)
{
    (void)pattern;
    struct file *f = ctx;
    NTSTATUS st = STATUS_SUCCESS;
    if (fsp_acquire_dir(&f->dir_buffer, marker == NULL, &st)) {
        int i;
        uint64_t first = 0;
        int more = 1, ok = 1;
        if (wcscmp(f->path, L"\\")) {
            struct hwf_info self = {0};
            self.kind = HWF_KIND_DIR;
            struct hwf_slot *s = begin(&i, HWF_FSTAT, vol_of(fs));
            s->handle = f->handle;
            if (!call(s)) self = s->info;
            end(s);
            ok = add_entry(&f->dir_buffer, ".", 1, &self, &st) && add_entry(&f->dir_buffer, "..", 2, &self, &st);
        }
        while (ok && more) {
            struct hwf_slot *s = begin(&i, HWF_LIST, vol_of(fs));
            s->offset = first;
            if (!put_path(s->path, f->path)) { end(s); st = STATUS_OBJECT_NAME_INVALID; break; }
            int err = call(s);
            if (err) { end(s); st = status_of(err); break; }
            const uint8_t *p = hwf_data(region, (uint32_t)i), *stop = p + s->out_length;
            while (ok && p + sizeof(struct hwf_entry) <= stop) {
                const struct hwf_entry *e = (const struct hwf_entry *)p;
                ok = add_entry(&f->dir_buffer, (const char *)(e + 1), e->name_len, &e->info, &st);
                p += (sizeof *e + e->name_len + 7) & ~(size_t)7;
            }
            first += s->out_a;
            more = s->more && s->out_a;
            end(s);
        }
        fsp_release_dir(&f->dir_buffer);
    }
    if (!NT_SUCCESS(st)) return st;
    fsp_read_dir(&f->dir_buffer, marker, buf, length, moved);
    return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE fs_interface = {
    .GetVolumeInfo = get_volume_info,
    .SetVolumeLabel = set_volume_label,
    .GetSecurityByName = get_security_by_name,
    .Create = create,
    .Open = open_file,
    .Overwrite = overwrite,
    .Cleanup = cleanup,
    .Close = close_file,
    .Read = read_file,
    .Write = write_file,
    .Flush = flush,
    .GetFileInfo = get_file_info,
    .SetBasicInfo = set_basic_info,
    .SetFileSize = set_file_size,
    .CanDelete = can_delete,
    .Rename = rename_file,
    .GetSecurity = get_security,
    .SetSecurity = set_security,
    .ReadDirectory = read_directory,
};

/* ---- volumes ---- */

static WCHAR free_drive(uint32_t flags)
{
    DWORD used = GetLogicalDrives();
    for (uint32_t i = 0; i < HWF_VOLUMES; i++)
        if (volumes[i].fs) used |= 1u << (volumes[i].drive[0] - L'A');
    WCHAR first = flags & HWF_VOL_PRIMARY ? DRIVE_PREFERRED : DRIVE_PREFERRED + 1;
    for (WCHAR c = first; c <= L'Z'; c++)
        if (!(used & (1u << (c - L'A')))) return c;
    for (WCHAR c = L'D'; c < first; c++)
        if (!(used & (1u << (c - L'A')))) return c;
    return 0;
}

static void mount(struct volume *v, const struct hwf_volume *hv)
{
    FSP_FSCTL_VOLUME_PARAMS p;
    memset(&p, 0, sizeof p);
    p.Version = sizeof p;
    p.SectorSize = 4096;
    p.SectorsPerAllocationUnit = 1;
    p.MaxComponentLength = 255;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    p.VolumeCreationTime = ((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    p.VolumeSerialNumber = 0x48570000u | (hv->id & 0xffff);
    /* Windows may cache what it learned for a second; data stays cached while unchanged. */
    p.FileInfoTimeout = 1000;
    p.CaseSensitiveSearch = 0;
    p.CasePreservedNames = 1;
    p.UnicodeOnDisk = 1;
    p.ReadOnlyVolume = !!(hv->flags & HWF_VOL_READONLY);
    p.PostCleanupWhenModifiedOnly = 1;
    p.UmFileContextIsUserContext2 = 1;
    /* Some installers and launchers only accept NTFS drives. */
    wcscpy(p.FileSystemName, L"NTFS");
    WCHAR drive = free_drive(hv->flags);
    if (!drive) return;
    FSP_FILE_SYSTEM *fs = NULL;
    if (!NT_SUCCESS(fsp_create(L"" FSP_FSCTL_DISK_DEVICE_NAME, &p, &fs_interface, &fs))) return;
    fs->UserContext = v;
    v->id = hv->id;
    v->drive[0] = drive;
    v->drive[1] = L':';
    v->drive[2] = 0;
    if (!NT_SUCCESS(fsp_set_mount_point(fs, v->drive)) || !NT_SUCCESS(fsp_start(fs, 8))) {
        fsp_delete(fs);
        v->id = 0;
        return;
    }
    v->fs = fs;
}

static void unmount(struct volume *v)
{
    fsp_stop(v->fs);
    fsp_delete(v->fs);
    memset(v, 0, sizeof *v);
}

/* Matches the mounted drives to the phone's volume list. */
static void sync_volumes(void)
{
    struct hwf_hdr *h = hwf_hdr(region);
    struct hwf_volume list[HWF_VOLUMES];
    memcpy(list, (const void *)h->volume, sizeof list);
    for (uint32_t i = 0; i < HWF_VOLUMES; i++) {
        if (!volumes[i].fs) continue;
        int still = 0;
        for (uint32_t k = 0; k < HWF_VOLUMES; k++) still |= list[k].id == volumes[i].id;
        if (!still) unmount(&volumes[i]);
    }
    /* The phone's own storage first, so it gets its letter before the others. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t k = 0; k < HWF_VOLUMES; k++) {
            if (!list[k].id || !!(list[k].flags & HWF_VOL_PRIMARY) != !pass) continue;
            int have = 0;
            for (uint32_t i = 0; i < HWF_VOLUMES; i++) have |= volumes[i].fs && volumes[i].id == list[k].id;
            if (have) continue;
            for (uint32_t i = 0; i < HWF_VOLUMES; i++) {
                if (volumes[i].fs || volumes[i].id) continue;
                mount(&volumes[i], &list[k]);
                break;
            }
        }
    }
}

static void run(void)
{
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)",
                                                         SDDL_REVISION_1, &sd, &sd_size);
    while (!load_winfsp()) Sleep(5000);
    while (!map_region()) Sleep(5000);
    uint32_t seen = 0;
    int first = 1;
    for (;;) {
        if (hwf_valid(region)) {
            uint32_t gen = __atomic_load_n(&hwf_hdr(region)->volume_gen, __ATOMIC_ACQUIRE);
            if (first || gen != seen) {
                seen = gen;
                first = 0;
                sync_volumes();
            }
        }
        Sleep(500);
    }
}

/* ---- service ---- */

static DWORD WINAPI svc_ctrl(DWORD control, DWORD type, LPVOID data, LPVOID context)
{
    (void)type; (void)data; (void)context;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        /* At shutdown the drives go with Windows; unmounting could only hold it up. */
        if (control == SERVICE_CONTROL_STOP)
            for (uint32_t i = 0; i < HWF_VOLUMES; i++)
                if (volumes[i].fs) unmount(&volumes[i]);
        SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, SERVICE_STOPPED, 0, 0, 0, 0, 0};
        SetServiceStatus(svc_handle, &st);
        ExitProcess(0);
    }
    return NO_ERROR;
}

static VOID WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;
    SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN,
                         0, 0, 0, 0};
    svc_handle = RegisterServiceCtrlHandlerExW(L"HyperWinFiles", svc_ctrl, NULL);
    SetServiceStatus(svc_handle, &st);
    run();
}

int main(void)
{
    WCHAR name[] = L"HyperWinFiles";
    SERVICE_TABLE_ENTRYW table[] = {{name, svc_main}, {NULL, NULL}};
    if (!StartServiceCtrlDispatcherW(table)) run();   /* started by hand, not by the service manager */
    return 0;
}
