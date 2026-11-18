/* Serves the phone's storage to Windows through the storage slots in the shared window
 * (include/hw_files.h; the Windows side is guest/files/hwfiles.c).
 *
 * One thread watches the slots and hands each request to that slot's own worker, so requests run
 * in parallel and file data goes straight between the storage and the window. FileShare.kt says
 * which volumes to share; with the all-files permission, plain paths under /storage reach them.
 */
#include <jni.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "hw_abi.h"
#include "hw_files.h"

#define MAX_HANDLES 8192u

struct share {
    uint32_t id;
    char root[512];
};

struct handle {
    int fd;
    uint8_t used, writable, dir;
    uint16_t gen;
    char *path;
};

static uint8_t *region;
static volatile int running;
static pthread_t watcher, workers[HWF_SLOTS];
static sem_t wake[HWF_SLOTS];
static uint32_t session;

static pthread_mutex_t share_lock = PTHREAD_MUTEX_INITIALIZER;
static struct share shares[HWF_VOLUMES];

static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
static struct handle handles[MAX_HANDLES];

/* ---- paths ---- */

/* The phone path for a Windows path on a volume, refusing anything that leaves the volume. */
static int resolve(uint32_t volume, const char *rel, char *out, size_t size)
{
    char root[512] = "";
    pthread_mutex_lock(&share_lock);
    for (uint32_t i = 0; i < HWF_VOLUMES; i++)
        if (shares[i].id && shares[i].id == volume) memcpy(root, shares[i].root, sizeof root);
    pthread_mutex_unlock(&share_lock);
    if (!root[0]) return ENODEV;
    if (rel[0] != '\\') return EINVAL;
    size_t n = strlen(root);
    if (n + 1 >= size) return ENAMETOOLONG;
    memcpy(out, root, n);
    const char *p = rel;
    while (*p == '\\') p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\\') e++;
        size_t len = (size_t)(e - p);
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.') || memchr(p, '/', len)) return EINVAL;
        if (len) {
            if (n + 1 + len + 1 > size) return ENAMETOOLONG;
            out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
        }
        p = *e ? e + 1 : e;
    }
    out[n] = 0;
    return 0;
}

static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

static void to_info(const struct stat *st, struct hwf_info *i)
{
    i->kind = S_ISDIR(st->st_mode) ? HWF_KIND_DIR : HWF_KIND_FILE;
    i->mode = st->st_mode & 07777;
    i->size = (uint64_t)st->st_size;
    i->blocks = (uint64_t)st->st_blocks * 512;
    i->ino = (uint64_t)st->st_ino;
    i->atime = ns(st->st_atim);
    i->mtime = ns(st->st_mtim);
    i->ctime = ns(st->st_ctim);
}

/* ---- handles ---- */

static uint64_t add_handle(int fd, const char *path, int writable, int dir)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        struct handle *h = &handles[i];
        if (h->used) continue;
        h->used = 1;
        h->fd = fd;
        h->writable = (uint8_t)writable;
        h->dir = (uint8_t)dir;
        h->path = strdup(path);
        uint64_t v = (uint64_t)session << 48 | (uint64_t)h->gen << 32 | i;
        pthread_mutex_unlock(&handle_lock);
        return v;
    }
    pthread_mutex_unlock(&handle_lock);
    return 0;
}

/* The fd behind a handle Windows sent, or -1. With writable, reopens it for writing first:
 * files open read-only until Windows writes, so reading never counts as a change. */
static int get_fd(uint64_t v, int writable, int *err)
{
    uint32_t i = (uint32_t)v;
    int fd = -1;
    *err = EBADF;
    pthread_mutex_lock(&handle_lock);
    if ((uint32_t)(v >> 48) == (session & 0xffff) && i < MAX_HANDLES && handles[i].used &&
        handles[i].gen == (uint16_t)(v >> 32)) {
        struct handle *h = &handles[i];
        fd = h->fd;
        *err = 0;
        if (writable && !h->writable && !h->dir) {
            int w = open(h->path, O_RDWR | O_CLOEXEC);
            if (w < 0) {
                *err = errno;
                fd = -1;
            } else {
                dup2(w, h->fd);
                close(w);
                h->writable = 1;
            }
        }
    }
    pthread_mutex_unlock(&handle_lock);
    return fd;
}

static int close_handle(uint64_t v)
{
    uint32_t i = (uint32_t)v;
    int fd = -1;
    char *path = NULL;
    pthread_mutex_lock(&handle_lock);
    if ((uint32_t)(v >> 48) == (session & 0xffff) && i < MAX_HANDLES && handles[i].used &&
        handles[i].gen == (uint16_t)(v >> 32)) {
        fd = handles[i].fd;
        path = handles[i].path;
        handles[i].used = 0;
        handles[i].path = NULL;
        handles[i].gen++;
    }
    pthread_mutex_unlock(&handle_lock);
    if (fd < 0) return EBADF;
    close(fd);
    free(path);
    return 0;
}

static void close_all(void)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        if (!handles[i].used) continue;
        close(handles[i].fd);
        free(handles[i].path);
        handles[i].used = 0;
        handles[i].path = NULL;
        handles[i].gen++;
    }
    pthread_mutex_unlock(&handle_lock);
}

/* ---- requests ---- */

static int fstat_info(int fd, struct hwf_info *info)
{
    struct stat st;
    if (fstat(fd, &st)) return errno;
    to_info(&st, info);
    return 0;
}

static int do_open(struct hwf_slot *s, const char *path)
{
    struct stat st;
    int fd, writable = 0, dir;
    if (s->op == HWF_CREATE) {
        dir = !!(s->flags & HWF_DIR);
        if (dir) {
            if (mkdir(path, 0777)) return errno;
            fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        } else {
            fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            writable = 1;
        }
    } else {
        if (stat(path, &st)) return errno;
        dir = S_ISDIR(st.st_mode);
        fd = open(path, (dir ? O_DIRECTORY : 0) | O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) return errno;
    int err = fstat_info(fd, &s->info);
    if (!err && !(s->out_handle = add_handle(fd, path, writable, dir))) err = EMFILE;
    if (err) close(fd);
    return err;
}

static int do_read(struct hwf_slot *s, uint8_t *data)
{
    int err, fd = get_fd(s->handle, 0, &err);
    if (fd < 0) return err;
    size_t want = s->length < HWF_DATA ? (size_t)s->length : HWF_DATA, got = 0;
    while (got < want) {
        ssize_t n = pread(fd, data + got, want - got, (off_t)(s->offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (!got) return errno;
            break;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    s->out_length = got;
    return 0;
}

static int do_write(struct hwf_slot *s, const uint8_t *data)
{
    int err, fd = get_fd(s->handle, 1, &err);
    if (fd < 0) return err;
    size_t want = s->length < HWF_DATA ? (size_t)s->length : HWF_DATA, done = 0;
    while (done < want) {
        ssize_t n = pwrite(fd, data + done, want - done, (off_t)(s->offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!done) return n < 0 ? errno : EIO;
            break;
        }
        done += (size_t)n;
    }
    s->out_length = done;
    return fstat_info(fd, &s->info);
}

static int do_list(struct hwf_slot *s, const char *path, uint8_t *data)
{
    DIR *d = opendir(path);
    if (!d) return errno;
    uint64_t skip = s->offset, count = 0, limit = s->length;
    size_t used = 0;
    struct dirent *e;
    s->more = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (skip) {
            skip--;
            continue;
        }
        size_t len = strlen(e->d_name);
        size_t need = (sizeof(struct hwf_entry) + len + 7) & ~(size_t)7;
        if (used + need > HWF_DATA || (limit && count == limit)) {
            s->more = 1;
            break;
        }
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) && fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        struct hwf_entry *out = (struct hwf_entry *)(data + used);
        memset(out, 0, sizeof *out);
        to_info(&st, &out->info);
        out->name_len = (uint16_t)len;
        memcpy(out + 1, e->d_name, len);
        used += need;
        count++;
    }
    closedir(d);
    s->out_length = used;
    s->out_a = (int64_t)count;
    return 0;
}

static int do_rename(struct hwf_slot *s, const char *from, const char *to)
{
    struct stat a, b;
    if (!(s->flags & HWF_REPLACE) && !stat(to, &b)) {
        /* The same file under another case is a rename, not a clash. */
        if (stat(from, &a) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return EEXIST;
    }
    return rename(from, to) ? errno : 0;
}

static int serve(struct hwf_slot *s, uint8_t *data)
{
    char path[4096], path2[4096];
    int err, fd;
    s->path[HWF_PATH - 1] = s->path2[HWF_PATH - 1] = 0;
    s->more = 0;
    s->out_length = 0;
    switch (s->op) {
    case HWF_VOLINFO: {
        struct statvfs v;
        if ((err = resolve(s->volume, "\\", path, sizeof path))) return err;
        if (statvfs(path, &v)) return errno;
        s->out_a = (int64_t)((uint64_t)v.f_blocks * v.f_frsize);
        s->out_b = (int64_t)((uint64_t)v.f_bavail * v.f_frsize);
        return 0;
    }
    case HWF_STAT: {
        struct stat st;
        if ((err = resolve(s->volume, s->path, path, sizeof path))) return err;
        if (stat(path, &st)) return errno;
        to_info(&st, &s->info);
        return 0;
    }
    case HWF_OPEN:
    case HWF_CREATE:
        if ((err = resolve(s->volume, s->path, path, sizeof path))) return err;
        return do_open(s, path);
    case HWF_CLOSE:
        return close_handle(s->handle);
    case HWF_READ:
        return do_read(s, data);
    case HWF_WRITE:
        return do_write(s, data);
    case HWF_RESIZE:
        if ((fd = get_fd(s->handle, 1, &err)) < 0) return err;
        if (ftruncate(fd, (off_t)s->offset)) return errno;
        return fstat_info(fd, &s->info);
    case HWF_FSTAT:
        if ((fd = get_fd(s->handle, 0, &err)) < 0) return err;
        return fstat_info(fd, &s->info);
    case HWF_SYNC:
        if ((fd = get_fd(s->handle, 0, &err)) < 0) return err;
        fdatasync(fd);
        return fstat_info(fd, &s->info);
    case HWF_LIST:
        if ((err = resolve(s->volume, s->path, path, sizeof path))) return err;
        return do_list(s, path, data);
    case HWF_REMOVE:
        if ((err = resolve(s->volume, s->path, path, sizeof path))) return err;
        return (s->flags & HWF_DIR ? rmdir(path) : unlink(path)) ? errno : 0;
    case HWF_RENAME:
        if ((err = resolve(s->volume, s->path, path, sizeof path)) ||
            (err = resolve(s->volume, s->path2, path2, sizeof path2)))
            return err;
        return do_rename(s, path, path2);
    case HWF_TIMES: {
        if ((fd = get_fd(s->handle, 0, &err)) < 0) return err;
        struct timespec t[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
        if (s->a > 0) t[0] = (struct timespec){s->a / 1000000000, s->a % 1000000000};
        if (s->b > 0) t[1] = (struct timespec){s->b / 1000000000, s->b % 1000000000};
        futimens(fd, t);   /* storage that keeps its own times refuses; that is no failure for Windows */
        return fstat_info(fd, &s->info);
    }
    default:
        return ENOSYS;
    }
}

static void *worker(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    struct hwf_slot *s = hwf_slot(region, i);
    for (;;) {
        while (sem_wait(&wake[i]) && errno == EINTR) {}
        if (!running) break;
        s->err = serve(s, hwf_data(region, i));
        __atomic_store_n(&s->state, HWF_DONE, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* Spins while Windows is busy with files, then sleeps longer the longer it stays quiet. */
static void *watch(void *arg)
{
    (void)arg;
    struct hwf_hdr *h = hwf_hdr(region);
    unsigned idle = 0;
    while (running) {
        int found = 0;
        for (uint32_t i = 0; i < HWF_SLOTS; i++) {
            struct hwf_slot *s = hwf_slot(region, i);
            uint32_t expected = HWF_REQUEST;
            if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == HWF_REQUEST &&
                __atomic_compare_exchange_n(&s->state, &expected, HWF_BUSY, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                sem_post(&wake[i]);
                found = 1;
            }
        }
        __atomic_store_n(&h->host_tick, h->host_tick + 1, __ATOMIC_RELAXED);
        if (found) idle = 0;
        else if (idle < 100000) idle++;
        if (idle < 4000) sched_yield();
        else if (idle < 6000) usleep(50);
        else if (idle < 8000) usleep(200);
        else usleep(1000);
    }
    return NULL;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_FileShare_nativeStart(JNIEnv *env, jclass cls, jobject buffer)
{
    (void)cls;
    uint8_t *window = (*env)->GetDirectBufferAddress(env, buffer);
    if (running || !window || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)HW_WINDOW_SIZE) return;
    region = window + HWF_OFF;
    memset(region, 0, HWF_BYTES);
    struct hwf_hdr *h = hwf_hdr(region);
    h->version = HWF_VERSION;
    h->slots = HWF_SLOTS;
    h->data_bytes = HWF_DATA;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    session = ((uint32_t)now.tv_nsec ^ (uint32_t)getpid()) & 0xffff;
    __atomic_store_n(&h->magic, HWF_MAGIC, __ATOMIC_RELEASE);
    running = 1;
    for (uint32_t i = 0; i < HWF_SLOTS; i++) {
        sem_init(&wake[i], 0, 0);
        pthread_create(&workers[i], NULL, worker, (void *)(uintptr_t)i);
    }
    pthread_create(&watcher, NULL, watch, NULL);
}

/* Before the window goes away. Requests in flight finish first. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_FileShare_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (!running) return;
    running = 0;
    pthread_join(watcher, NULL);
    for (uint32_t i = 0; i < HWF_SLOTS; i++) {
        sem_post(&wake[i]);
        pthread_join(workers[i], NULL);
        sem_destroy(&wake[i]);
    }
    close_all();
    region = NULL;
}

/* The volumes Windows should show: id (stable, nonzero), flags, label, phone path of its root. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_FileShare_nativeSetVolumes(JNIEnv *env, jclass cls, jintArray ids, jintArray flags,
                                                 jobjectArray labels, jobjectArray roots)
{
    (void)cls;
    if (!running) return;
    jsize n = (*env)->GetArrayLength(env, ids);
    if (n > (jsize)HWF_VOLUMES) n = HWF_VOLUMES;
    jint id[HWF_VOLUMES], fl[HWF_VOLUMES];
    (*env)->GetIntArrayRegion(env, ids, 0, n, id);
    (*env)->GetIntArrayRegion(env, flags, 0, n, fl);
    struct hwf_volume list[HWF_VOLUMES];
    struct share next[HWF_VOLUMES];
    memset(list, 0, sizeof list);
    memset(next, 0, sizeof next);
    for (jsize i = 0; i < n; i++) {
        jstring label = (*env)->GetObjectArrayElement(env, labels, i);
        jstring root = (*env)->GetObjectArrayElement(env, roots, i);
        const char *l = (*env)->GetStringUTFChars(env, label, NULL);
        const char *r = (*env)->GetStringUTFChars(env, root, NULL);
        list[i].id = next[i].id = (uint32_t)id[i];
        list[i].flags = (uint32_t)fl[i];
        strncpy(list[i].label, l, sizeof list[i].label - 1);
        strncpy(next[i].root, r, sizeof next[i].root - 1);
        (*env)->ReleaseStringUTFChars(env, label, l);
        (*env)->ReleaseStringUTFChars(env, root, r);
    }
    pthread_mutex_lock(&share_lock);
    memcpy(shares, next, sizeof shares);
    pthread_mutex_unlock(&share_lock);
    struct hwf_hdr *h = hwf_hdr(region);
    memcpy(h->volume, list, sizeof list);
    __atomic_add_fetch(&h->volume_gen, 1, __ATOMIC_RELEASE);
}
