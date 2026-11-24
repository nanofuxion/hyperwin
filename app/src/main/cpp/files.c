/* Serves the phone's storage to Windows through the storage slots in the shared window
 * (include/hw_files.h; the Windows side is guest/files/hwfiles.c).
 *
 * One thread watches the slots and hands each request to that slot's own worker, so requests run
 * in parallel and file data goes straight between the storage and the window. FileShare.kt says
 * which volumes to share; with the all-files permission, plain paths under /storage reach them.
 */
#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "hw_abi.h"
#include "hw_files.h"

#define MAX_HANDLES 8192u

struct share {
    uint32_t id;
    int case_insensitive;  /* 1 when the Android volume itself matches paths case-insensitively */
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
static int volume_root(uint32_t volume, char *root, size_t size)
{
    root[0] = 0;
    pthread_mutex_lock(&share_lock);
    for (uint32_t i = 0; i < HWF_VOLUMES; i++)
        if (shares[i].id && shares[i].id == volume) {
            memcpy(root, shares[i].root, size < sizeof shares[i].root ? size : sizeof shares[i].root);
            root[size - 1] = 0;
            break;
        }
    pthread_mutex_unlock(&share_lock);
    return root[0] ? 0 : ENODEV;
}

static int resolve(uint32_t volume, const char *rel, char *out, size_t size)
{
    char root[512] = "";
    if (volume_root(volume, root, sizeof root)) return ENODEV;
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

static void make_writable(const char *path, int dir);
static void parent_writable(const char *path);

/* The fd behind a handle Windows sent, or -1. With writable, reopens it for writing first:
 * files open read-only until Windows writes, so reading never counts as a change. */
static int get_fd(uint64_t v, int writable, int *err)
{
    uint32_t i = (uint32_t)v;
    uint16_t gen;
    int fd = -1, w = -1;
    char path[4096];
    *err = EBADF;
    pthread_mutex_lock(&handle_lock);
    if (!((uint32_t)(v >> 48) == (session & 0xffff) && i < MAX_HANDLES && handles[i].used &&
          handles[i].gen == (uint16_t)(v >> 32))) {
        pthread_mutex_unlock(&handle_lock);
        return -1;
    }
    gen = handles[i].gen;
    if (!(writable && !handles[i].writable && !handles[i].dir && handles[i].path)) {
        fd = handles[i].fd;
        *err = fd < 0 ? EBADF : 0;
        pthread_mutex_unlock(&handle_lock);
        return fd;
    }
    snprintf(path, sizeof path, "%s", handles[i].path);
    pthread_mutex_unlock(&handle_lock);
    make_writable(path, 0);
    parent_writable(path);
    w = open(path, O_RDWR | O_CLOEXEC);
    pthread_mutex_lock(&handle_lock);
    if (!(handles[i].used && handles[i].gen == gen && handles[i].path && !strcmp(handles[i].path, path))) {
        pthread_mutex_unlock(&handle_lock);
        if (w >= 0) close(w);
        return -1;
    }
    if (w < 0) {
        *err = errno ? errno : EIO;
    } else if (handles[i].fd < 0) {
        handles[i].fd = w;
        handles[i].writable = 1;
        fd = w;
        *err = 0;
    } else {
        dup2(w, handles[i].fd);
        close(w);
        handles[i].writable = 1;
        fd = handles[i].fd;
        *err = 0;
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
    if (!path && fd < 0) return EBADF;
    if (fd >= 0) close(fd);
    free(path);
    return 0;
}

/* Drop our own opens of this phone path so unlink and rename are not EBUSY. */
static void release_path(const char *path)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        struct handle *h = &handles[i];
        if (!h->used || !h->path || strcmp(h->path, path)) continue;
        if (h->fd >= 0) close(h->fd);
        h->fd = -1;
        h->writable = 0;
    }
    pthread_mutex_unlock(&handle_lock);
}

static void retarget_path(const char *from, const char *to)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        struct handle *h = &handles[i];
        char *next;
        if (!h->used || !h->path || strcmp(h->path, from)) continue;
        next = strdup(to);
        if (!next) continue;
        free(h->path);
        h->path = next;
    }
    pthread_mutex_unlock(&handle_lock);
}

/* Android storage often has no write bit even when this app is allowed to change the file. */
static void make_writable(const char *path, int dir)
{
    struct stat st;
    mode_t mode;
    if (stat(path, &st)) return;
    mode = st.st_mode | (dir ? 0777 : 0666);
    if ((st.st_mode & 0777) == (mode & 0777)) return;
    chmod(path, mode & 0777);
}

static void parent_writable(const char *path)
{
    char buf[4096];
    char *slash;
    snprintf(buf, sizeof buf, "%s", path);
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = 0;
    make_writable(buf, 1);
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

/* A stat or open that has not finished in time must not pin the request. The
 * thread keeps the job and frees it if this side gives up. */
struct io_job {
    int dirfd, close_fd, flags, mode, fd, rc, state; /* 0 running, 1 waiter owns it, 2 thread frees it */
    char path[4096];
    struct stat st;
};

static void *stat_job(void *arg)
{
    struct io_job *j = arg;
    if (fstatat(j->dirfd, j->path[0] ? j->path : ".", &j->st, j->flags)) j->rc = errno ? errno : EIO;
    else j->rc = 0;
    if (j->close_fd && j->dirfd >= 0) close(j->dirfd);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&j->state, &expected, 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) free(j);
    return NULL;
}

static void *open_job(void *arg)
{
    struct io_job *j = arg;
    int fd = open(j->path, j->flags, j->mode);
    if (fd < 0) {
        j->rc = errno ? errno : EIO;
        j->fd = -1;
    } else {
        j->rc = 0;
        j->fd = fd;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&j->state, &expected, 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        if (j->fd >= 0) close(j->fd);
        free(j);
    }
    return NULL;
}

/* 0 when the thread finished and the caller owns the object. -1 when the thread will free it. */
static int wait_state(int *state, pthread_t thread, int ms)
{
    struct timespec start, now;
    unsigned polls = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 1) {
            pthread_join(thread, NULL);
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= ms) {
            int expected = 0;
            if (!__atomic_compare_exchange_n(state, &expected, 2, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                pthread_join(thread, NULL);
                return 0;
            }
            pthread_detach(thread);
            return -1;
        }
        /* A stat or open answers in about 100 us, so look again soon. Sleeping a fixed 2 ms here cost
         * every open on the drive several milliseconds. Only a stalled storage falls back to 2 ms. */
        usleep(polls < 40 ? 25 : polls < 80 ? 200 : 2000);
        polls++;
    }
}

/* Storage that answers is asked directly: a helper thread per stat or open cost more than the call
 * itself. One answer slower than SLOW_MS sends the next minute of calls back through helper threads,
 * which can give up on a stalled storage and let Windows have an error. */
#define SLOW_MS 150
#define SLOW_HOLD_MS 60000
static long slow_until_ms;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* FUSE under memory pressure now and then answers a stat or open with a passing error. Ask again. */
static int transient(int err)
{
    return err == EIO || err == EAGAIN || err == EINTR || err == ENOTCONN || err == ESTALE || err == ETIMEDOUT || err == ENOMEM;
}

static int direct_ok(void)
{
    return now_ms() >= __atomic_load_n(&slow_until_ms, __ATOMIC_RELAXED);
}

static void note_time(long start)
{
    if (now_ms() - start > SLOW_MS) __atomic_store_n(&slow_until_ms, now_ms() + SLOW_HOLD_MS, __ATOMIC_RELAXED);
}

static int stat_flags(void)
{
    int flags = AT_SYMLINK_NOFOLLOW;
#ifdef AT_NO_AUTOMOUNT
    flags |= AT_NO_AUTOMOUNT;
#endif
    return flags;
}

/* 0 and *st filled, a positive errno, or -1 if it did not finish in time. */
static int stat_at(int dirfd, const char *name, struct stat *st, int ms)
{
    if (direct_ok()) {
        long t0 = now_ms();
        int rc = 0, tries = 0;
        do {
            rc = fstatat(dirfd, name && name[0] ? name : ".", st, stat_flags()) ? (errno ? errno : EIO) : 0;
            if (rc && transient(rc)) {
                __android_log_print(ANDROID_LOG_INFO, "HyperWin", "stat %s: errno %d, again", name ? name : "", rc);
                usleep(20000);
            }
        } while (rc && transient(rc) && ++tries < 4);
        note_time(t0);
        return rc;
    }
    struct io_job *j = calloc(1, sizeof *j);
    pthread_t thread;
    int waited;
    if (!j) return ENOMEM;
    j->fd = -1;
    j->flags = stat_flags();
    if (dirfd == AT_FDCWD) j->dirfd = AT_FDCWD;
    else {
        j->dirfd = dup(dirfd);
        j->close_fd = 1;
        if (j->dirfd < 0) {
            free(j);
            return errno ? errno : EIO;
        }
    }
    snprintf(j->path, sizeof j->path, "%s", name ? name : "");
    if (pthread_create(&thread, NULL, stat_job, j)) {
        if (j->close_fd) close(j->dirfd);
        free(j);
        return EAGAIN;
    }
    waited = wait_state(&j->state, thread, ms);
    if (waited) return -1;
    if (!j->rc) *st = j->st;
    waited = j->rc;
    free(j);
    return waited;
}

static int open_timeout(const char *path, int flags, int mode, int ms, int *fd_out)
{
    if (direct_ok()) {
        long t0 = now_ms();
        int fd = -1, rc = 0, tries = 0;
        do {
            fd = open(path, flags, mode);
            rc = fd < 0 ? (errno ? errno : EIO) : 0;
            if (rc && transient(rc)) {
                __android_log_print(ANDROID_LOG_INFO, "HyperWin", "open %s: errno %d, again", path, rc);
                usleep(20000);
            }
        } while (rc && transient(rc) && ++tries < 4);
        note_time(t0);
        *fd_out = fd;
        return rc;
    }
    struct io_job *j = calloc(1, sizeof *j);
    pthread_t thread;
    int waited;
    if (!j) return ENOMEM;
    j->fd = -1;
    j->flags = flags;
    j->mode = mode;
    snprintf(j->path, sizeof j->path, "%s", path);
    if (pthread_create(&thread, NULL, open_job, j)) {
        free(j);
        return EAGAIN;
    }
    waited = wait_state(&j->state, thread, ms);
    if (waited) return -1;
    *fd_out = j->fd;
    waited = j->rc;
    free(j);
    return waited;
}

/* Looks in dir for a name that matches p in any case. 0 with the stored spelling in name, ENOENT only after
 * the whole folder was read, else the error. Storage under memory pressure now and then fails a folder read
 * halfway. That used to look like a missing file, so it is read again. */
static int find_ci(const char *dir, const char *p, size_t len, char *name, size_t cap)
{
    int tries;
    for (tries = 0; tries < 4; tries++) {
        DIR *d = opendir(dir);
        struct dirent *ent;
        int err;
        if (!d) {
            err = errno ? errno : EIO;
            if (!transient(err)) return err;
            usleep(20000);
            continue;
        }
        errno = 0;
        while ((ent = readdir(d))) {
            if (strlen(ent->d_name) == len && !strncasecmp(ent->d_name, p, len)) {
                if (len + 1 > cap) {
                    closedir(d);
                    return ENAMETOOLONG;
                }
                memcpy(name, ent->d_name, len);
                name[len] = 0;
                closedir(d);
                return 0;
            }
            errno = 0;
        }
        err = errno;
        closedir(d);
        if (!err) return ENOENT;
        __android_log_print(ANDROID_LOG_INFO, "HyperWin", "read of %s failed: errno %d, again", dir, err);
        usleep(20000);
    }
    return EIO;
}

/* Windows treats the drive as case-insensitive. Android paths are not. */
static int resolve_ci(uint32_t volume, const char *rel, char *out, size_t size, int final_exact)
{
    char root[512], dir[4096], name[512];
    const char *p;
    size_t n;
    int err = volume_root(volume, root, sizeof root);
    if (err) return err;
    n = strlen(root);
    if (n + 1 > sizeof dir || n + 1 > size) return ENAMETOOLONG;
    memcpy(dir, root, n + 1);
    p = rel;
    while (*p == '\\') p++;
    if (!*p) {
        memcpy(out, dir, n + 1);
        return 0;
    }
    while (*p) {
        const char *e = p;
        size_t len, last = 0;
        int rc;
        while (*e && *e != '\\') e++;
        len = (size_t)(e - p);
        last = !*e || !e[1];
        while (*e == '\\') e++;
        if (!len) {
            p = e;
            continue;
        }
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.') || memchr(p, '/', len))
            return EINVAL;
        rc = find_ci(dir, p, len, name, sizeof name);
        if (final_exact && last) {
            /* An existing name in another case is the file Windows asked for. A new name keeps its own case. */
            const char *use = rc == 0 ? name : p;
            size_t ulen = rc == 0 ? strlen(name) : len;
            if (rc && rc != ENOENT && rc != ENOTDIR && rc != EACCES) return rc;
            if (n + 1 + ulen + 1 > size) return ENAMETOOLONG;
            memcpy(out, dir, n);
            out[n] = '/';
            memcpy(out + n + 1, use, ulen);
            out[n + 1 + ulen] = 0;
            return 0;
        }
        if (rc) return rc;
        if (n + 1 + strlen(name) + 1 > sizeof dir || n + 1 + strlen(name) + 1 > size) return ENAMETOOLONG;
        dir[n] = '/';
        memcpy(dir + n + 1, name, strlen(name));
        n += 1 + strlen(name);
        dir[n] = 0;
        p = e;
    }
    memcpy(out, dir, n + 1);
    return 0;
}

/* Probes whether a volume root matches names case-insensitively (Android /storage/emulated/0 FUSE
 * and FAT/exFAT SD/USB volumes do; ext4 does not). */
static int probe_case_insensitive(const char *root)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    int dfd = dirfd(d), ci = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char flipped[512];
        size_t len = strlen(e->d_name);
        if (!len || len >= sizeof flipped) continue;
        memcpy(flipped, e->d_name, len + 1);
        int changed = 0;
        for (size_t i = 0; i < len; i++) {
            if (flipped[i] >= 'a' && flipped[i] <= 'z') { flipped[i] -= 32; changed = 1; break; }
            if (flipped[i] >= 'A' && flipped[i] <= 'Z') { flipped[i] += 32; changed = 1; break; }
        }
        if (!changed) continue;
        struct stat s1, s2;
        if (!fstatat(dfd, e->d_name, &s1, AT_SYMLINK_NOFOLLOW) &&
            !fstatat(dfd, flipped, &s2, AT_SYMLINK_NOFOLLOW) &&
            s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino)
            ci = 1;
        break;
    }
    closedir(d);
    return ci;
}

static int volume_ci(uint32_t volume)
{
    int ci = 0;
    pthread_mutex_lock(&share_lock);
    for (uint32_t i = 0; i < HWF_VOLUMES; i++)
        if (shares[i].id && shares[i].id == volume) {
            ci = shares[i].case_insensitive;
            break;
        }
    pthread_mutex_unlock(&share_lock);
    return ci;
}

static int resolve_for(uint32_t volume, const char *rel, char *out, size_t size, int creating)
{
    struct stat st;
    int err = resolve(volume, rel, out, size), rc;
    if (err) return err;
    rc = stat_at(AT_FDCWD, out, &st, 1000);
    if (rc == 0) return 0;
    if (rc > 0 && rc != ENOENT) return rc;
    if (rc < 0) return ETIMEDOUT;
    if (volume_ci(volume)) {
        if (!creating) return ENOENT;
        /* Volume is already case-insensitive: if the parent directory exists, out is ready to create. */
        char *slash = strrchr(out, '/');
        if (slash && slash > out) {
            *slash = 0;
            int prc = stat_at(AT_FDCWD, out, &st, 1000);
            *slash = '/';
            if (prc == 0) return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
            if (prc < 0) return ETIMEDOUT;
            return prc;
        }
        return 0;
    }
    return resolve_ci(volume, rel, out, size, creating);
}

static int do_open(struct hwf_slot *s, const char *path)
{
    struct stat st;
    int fd = -1, writable = 0, dir, rc;
    if (s->op == HWF_CREATE) {
        dir = !!(s->flags & HWF_DIR);
        parent_writable(path);
        if (dir) {
            if (mkdir(path, 0777)) {
                int mk = errno;
                if (mk == EACCES || mk == EPERM) {
                    parent_writable(path);
                    if (!mkdir(path, 0777)) mk = 0;
                    else mk = errno;
                }
                if (mk) return mk;
            }
            rc = open_timeout(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 2000, &fd);
        } else {
            rc = open_timeout(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666, 2000, &fd);
            if (rc == EACCES || rc == EPERM) {
                parent_writable(path);
                rc = open_timeout(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666, 2000, &fd);
            }
            writable = 1;
        }
    } else {
        rc = stat_at(AT_FDCWD, path, &st, 1000);
        if (rc < 0) return ETIMEDOUT;
        if (rc) return rc;
        dir = S_ISDIR(st.st_mode);
        if (!dir && (s->flags & HWF_OPEN_WRITE)) {
            make_writable(path, 0);
            rc = open_timeout(path, O_RDWR | O_CLOEXEC, 0, 2000, &fd);
            writable = rc == 0;
            if (rc) rc = open_timeout(path, O_RDONLY | O_CLOEXEC, 0, 2000, &fd);
        } else {
            rc = open_timeout(path, (dir ? O_DIRECTORY : 0) | O_RDONLY | O_CLOEXEC, 0, 2000, &fd);
        }
    }
    if (rc < 0) return ETIMEDOUT;
    if (rc) return rc;
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

#define LIST_NAMES 65536u
#define LIST_BUDGET_MS 800u
#define LIST_PARALLEL 8u
#define LIST_STAT_MS 200
#define LIST_CACHE_MS 3000u

struct list_name {
    char *name;
    unsigned char d_type;
    struct stat st;
    int stated;
};

#define LIST_CACHE_SLOTS 16u

struct list_cache_entry {
    char path[4096];
    uint64_t until;
    uint64_t used_at;
    uint8_t *data;
    size_t len;
    uint64_t count;
};

static struct {
    pthread_mutex_t mu;
    struct list_cache_entry entries[LIST_CACHE_SLOTS];
} list_cache = { .mu = PTHREAD_MUTEX_INITIALIZER };

static int list_busy;

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static size_t entry_need(size_t name_len)
{
    return (sizeof(struct hwf_entry) + name_len + 7u) & ~(size_t)7u;
}

static void cache_drop(void)
{
    pthread_mutex_lock(&list_cache.mu);
    for (uint32_t i = 0; i < LIST_CACHE_SLOTS; i++) {
        struct list_cache_entry *c = &list_cache.entries[i];
        free(c->data);
        c->data = NULL;
        c->len = 0;
        c->count = 0;
        c->path[0] = 0;
        c->until = 0;
        c->used_at = 0;
    }
    pthread_mutex_unlock(&list_cache.mu);
}

/* 1 when this page came from a recent listing of the same directory. */
static int cache_slice(const char *path, uint64_t skip, uint8_t *data, struct hwf_slot *s)
{
    const uint8_t *p, *end;
    uint64_t index = 0, count = 0, now = mono_ms();
    size_t used = 0;
    int hit = 0;
    pthread_mutex_lock(&list_cache.mu);
    for (uint32_t i = 0; i < LIST_CACHE_SLOTS; i++) {
        struct list_cache_entry *c = &list_cache.entries[i];
        if (c->until <= now || strcmp(c->path, path)) continue;
        c->used_at = now;
        hit = 1;
        p = c->data;
        end = p + c->len;
        s->more = 0;
        while (p && p + sizeof(struct hwf_entry) <= end) {
            const struct hwf_entry *e = (const struct hwf_entry *)p;
            size_t need = entry_need(e->name_len);
            if (need < sizeof *e || p + need > end) break;
            if (index >= skip) {
                if (used + need > HWF_DATA) {
                    s->more = 1;
                    break;
                }
                memcpy(data + used, p, need);
                used += need;
                count++;
            }
            index++;
            p += need;
        }
        if (index < c->count) s->more = 1;
        s->out_length = used;
        s->out_a = (int64_t)count;
        break;
    }
    pthread_mutex_unlock(&list_cache.mu);
    return hit;
}

static void cache_store(const char *path, uint8_t *data, size_t len, uint64_t count)
{
    uint64_t now = mono_ms();
    struct list_cache_entry *victim = &list_cache.entries[0];
    pthread_mutex_lock(&list_cache.mu);
    for (uint32_t i = 0; i < LIST_CACHE_SLOTS; i++) {
        struct list_cache_entry *c = &list_cache.entries[i];
        if (!strcmp(c->path, path)) {
            victim = c;
            break;
        }
        if (c->until <= now) {
            victim = c;
        } else if (victim->until > now && c->used_at < victim->used_at) {
            victim = c;
        }
    }
    free(victim->data);
    snprintf(victim->path, sizeof victim->path, "%s", path);
    victim->data = data;
    victim->len = len;
    victim->count = count;
    victim->until = now + LIST_CACHE_MS;
    victim->used_at = now;
    pthread_mutex_unlock(&list_cache.mu);
}

struct listed {
    char path[4096];
    uint8_t *data;
    size_t len;
    uint64_t count;
    int rc, state;
};

static void pack_name(uint8_t **buf, size_t *len, size_t *cap, struct list_name *nm)
{
    size_t nlen = strlen(nm->name), need = entry_need(nlen);
    struct hwf_entry *e;
    uint8_t *grown;
    if (nlen > 65535) nlen = 65535;
    need = entry_need(nlen);
    if (*len + need > *cap) {
        size_t ncap = *cap ? *cap * 2 : 65536;
        while (ncap < *len + need) ncap *= 2;
        grown = realloc(*buf, ncap);
        if (!grown) return;
        *buf = grown;
        *cap = ncap;
    }
    e = (struct hwf_entry *)(*buf + *len);
    memset(e, 0, need);
    if (nm->stated) to_info(&nm->st, &e->info);
    else e->info.kind = nm->d_type == DT_DIR ? HWF_KIND_DIR : HWF_KIND_FILE;
    e->name_len = (uint16_t)nlen;
    memcpy(e + 1, nm->name, nlen);
    *len += need;
}

static void *list_thread(void *arg)
{
    struct listed *L = arg;
    DIR *d;
    struct list_name *names = NULL;
    size_t n = 0, capn = 0, blob_cap = 0, i = 0;
    uint64_t start;
    int dfd = -1, tries = 0, rderr;
again:
    d = opendir(L->path);
    if (!d) {
        L->rc = errno ? errno : EIO;
        if (transient(L->rc) && ++tries < 4) {
            __android_log_print(ANDROID_LOG_INFO, "HyperWin", "list %s: open errno %d, again", L->path, L->rc);
            usleep(20000);
            goto again;
        }
        goto finish;
    }
    dfd = dirfd(d);
    errno = 0;
    while (n < LIST_NAMES) {
        struct dirent *e = readdir(d);
        size_t ncap;
        struct list_name *grown;
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == capn) {
            ncap = capn ? capn * 2 : 256;
            grown = realloc(names, ncap * sizeof *names);
            if (!grown) break;
            names = grown;
            capn = ncap;
        }
        names[n].name = strdup(e->d_name);
        if (!names[n].name) break;
        names[n].d_type = e->d_type;
        names[n].stated = 0;
        memset(&names[n].st, 0, sizeof names[n].st);
        n++;
        errno = 0;
    }
    rderr = errno;
    if (rderr && transient(rderr) && tries + 1 < 4) {
        /* The read stopped on an error, so the list is short. Start it again. */
        size_t k;
        __android_log_print(ANDROID_LOG_INFO, "HyperWin", "list %s: read errno %d after %zu names, again", L->path, rderr, n);
        for (k = 0; k < n; k++) free(names[k].name);
        free(names);
        names = NULL;
        n = capn = 0;
        closedir(d);
        dfd = -1;
        tries++;
        usleep(20000);
        goto again;
    }
    start = mono_ms();
    i = 0;
    while (i < n && mono_ms() - start < LIST_BUDGET_MS) {
        struct io_job *job[LIST_PARALLEL] = {0};
        pthread_t thread[LIST_PARALLEL];
        size_t at[LIST_PARALLEL];
        unsigned k = 0, t;
        while (k < LIST_PARALLEL && i < n && mono_ms() - start < LIST_BUDGET_MS) {
            char child[4096];
            int wrote;
            if (names[i].d_type == DT_DIR) {
                i++;
                continue;
            }
            wrote = snprintf(child, sizeof child, "%s/%s", L->path, names[i].name);
            if (wrote < 0 || wrote >= (int)sizeof child) {
                i++;
                continue;
            }
            job[k] = calloc(1, sizeof *job[k]);
            if (!job[k]) {
                i++;
                break;
            }
            job[k]->dirfd = AT_FDCWD;
            job[k]->flags = stat_flags();
            job[k]->fd = -1;
            snprintf(job[k]->path, sizeof job[k]->path, "%s", child);
            if (pthread_create(&thread[k], NULL, stat_job, job[k])) {
                free(job[k]);
                job[k] = NULL;
                i++;
                break;
            }
            at[k] = i;
            k++;
            i++;
        }
        for (t = 0; t < k; t++) {
            uint64_t elapsed = mono_ms() - start;
            int budget = elapsed >= LIST_BUDGET_MS ? 1 : (int)(LIST_BUDGET_MS - elapsed);
            if (budget > LIST_STAT_MS) budget = LIST_STAT_MS;
            if (wait_state(&job[t]->state, thread[t], budget)) continue;
            if (!job[t]->rc) {
                names[at[t]].st = job[t]->st;
                names[at[t]].stated = 1;
                free(job[t]);
            } else {
                free(job[t]);
            }
        }
    }
    closedir(d);
    dfd = -1;
    for (i = 0; i < n; i++) {
        pack_name(&L->data, &L->len, &blob_cap, &names[i]);
        L->count++;
        free(names[i].name);
    }
    free(names);
    names = NULL;
    __android_log_print(ANDROID_LOG_INFO, "HyperWin", "list %s names=%zu stated_budget=%ums", L->path, n,
                        LIST_BUDGET_MS);
    cache_store(L->path, L->data, L->len, L->count);
    L->data = NULL;
finish:
    if (dfd >= 0 && d) closedir(d);
    for (i = 0; names && i < n; i++) free(names[i].name);
    free(names);
    __atomic_store_n(&list_busy, 0, __ATOMIC_RELEASE);
    {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&L->state, &expected, 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) free(L);
    }
    return NULL;
}

/* One listing runs at a time and its result is kept briefly for the folder it was made for. A request
 * for another folder must wait its turn and then list for itself. It used to wait for the other
 * folder's result and give up after 8 s, which Windows showed as an I/O error. */
static int do_list(struct hwf_slot *s, const char *path, uint8_t *data)
{
    uint64_t start = mono_ms();
    while (mono_ms() - start < 8000) {
        struct listed *L;
        pthread_t thread;
        int busy = 0, waited, rc;
        if (cache_slice(path, s->offset, data, s)) return 0;
        if (!__atomic_compare_exchange_n(&list_busy, &busy, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            usleep(2000);
            continue;
        }
        L = calloc(1, sizeof *L);
        if (!L) {
            __atomic_store_n(&list_busy, 0, __ATOMIC_RELEASE);
            return ENOMEM;
        }
        snprintf(L->path, sizeof L->path, "%s", path);
        if (pthread_create(&thread, NULL, list_thread, L)) {
            free(L);
            __atomic_store_n(&list_busy, 0, __ATOMIC_RELEASE);
            return EAGAIN;
        }
        waited = wait_state(&L->state, thread, 8000);
        if (waited) {
            if (cache_slice(path, s->offset, data, s)) return 0;
            __android_log_print(ANDROID_LOG_INFO, "HyperWin", "list %s timed out", path);
            return ETIMEDOUT;
        }
        rc = L->rc;
        free(L);
        if (rc) return rc;
        /* The result is in the cache now. If another folder's listing replaced it already, go round. */
    }
    __android_log_print(ANDROID_LOG_INFO, "HyperWin", "list %s gave up waiting", path);
    return ETIMEDOUT;
}

static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) || stat(b, &sb)) return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int do_rename(struct hwf_slot *s, const char *from, const char *to)
{
    struct stat dest;
    int replacing = !stat(to, &dest);
    if (replacing && !(s->flags & HWF_REPLACE) && !same_file(from, to)) return EEXIST;
    if (replacing && (s->flags & HWF_REPLACE) && !same_file(from, to) && !S_ISDIR(dest.st_mode)) {
        release_path(to);
        make_writable(to, 0);
        unlink(to);
    }
    if (!rename(from, to)) {
        retarget_path(from, to);
        return 0;
    }
    if (errno == EBUSY || errno == EACCES || errno == EPERM) {
        release_path(from);
        release_path(to);
        parent_writable(to);
        make_writable(from, 0);
        if (!rename(from, to)) {
            retarget_path(from, to);
            return 0;
        }
    }
    return errno ? errno : EIO;
}

static int do_remove(const char *path, int dir)
{
    int err;
    if (dir) {
        if (!rmdir(path)) return 0;
        err = errno;
        if (err == EACCES || err == EPERM || err == EBUSY) {
            release_path(path);
            make_writable(path, 1);
            parent_writable(path);
            if (!rmdir(path)) return 0;
            err = errno;
        }
        return err ? err : EIO;
    }
    if (!unlink(path)) return 0;
    err = errno;
    if (err == EBUSY || err == EACCES || err == EPERM) {
        release_path(path);
        make_writable(path, 0);
        parent_writable(path);
        if (!unlink(path)) return 0;
        err = errno;
    }
    return err ? err : EIO;
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
        int rc;
        if ((err = resolve_for(s->volume, s->path, path, sizeof path, 0))) return err;
        rc = stat_at(AT_FDCWD, path, &st, 1000);
        if (rc < 0) return ETIMEDOUT;
        if (rc) return rc;
        to_info(&st, &s->info);
        return 0;
    }
    case HWF_OPEN:
    case HWF_CREATE:
        if (s->op == HWF_CREATE) cache_drop();
        if ((err = resolve_for(s->volume, s->path, path, sizeof path, s->op == HWF_CREATE))) return err;
        return do_open(s, path);
    case HWF_CLOSE:
        return close_handle(s->handle);
    case HWF_READ:
        return do_read(s, data);
    case HWF_WRITE:
        cache_drop();
        return do_write(s, data);
    case HWF_RESIZE:
        cache_drop();
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
        if ((err = resolve_for(s->volume, s->path, path, sizeof path, 0))) return err;
        return do_list(s, path, data);
    case HWF_REMOVE:
        cache_drop();
        if ((err = resolve_for(s->volume, s->path, path, sizeof path, 0))) return err;
        return do_remove(path, s->flags & HWF_DIR);
    case HWF_RENAME:
        cache_drop();
        if ((err = resolve_for(s->volume, s->path, path, sizeof path, 0)) ||
            (err = resolve_for(s->volume, s->path2, path2, sizeof path2, 1)))
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
        next[i].case_insensitive = probe_case_insensitive(next[i].root);
        __android_log_print(ANDROID_LOG_INFO, "HyperWin", "volume %u %s ci=%d", next[i].id, next[i].root,
                            next[i].case_insensitive);
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
