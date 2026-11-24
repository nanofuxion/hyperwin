#include <jni.h>
#include <stdio.h>
#include <string.h>
#include "packfat.h"
#include "bcd_testsign.h"
#include "ring.h"

static struct hw_tpm tpm;
static int ready;

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeInit(JNIEnv *env, jclass cls, jobject buffer, jbyteArray seed)
{
    void *window = (*env)->GetDirectBufferAddress(env, buffer);
    jlong bytes = (*env)->GetDirectBufferCapacity(env, buffer);
    uint8_t raw[32];
    jsize n;
    (void)cls;
    if (!window || bytes < (jlong)HW_WINDOW_SIZE) return;
    hw_window_init(window, (size_t)bytes);
    memset(raw, 0xA5, sizeof raw);
    if (seed) {
        n = (*env)->GetArrayLength(env, seed);
        if (n >= 32) (*env)->GetByteArrayRegion(env, seed, 0, 32, (jbyte *)raw);
    }
    hw_tpm_init(&tpm, raw);
    __atomic_store_n(&((struct hw_header *)window)->tpm_status, 1, __ATOMIC_RELEASE);
    ready = 1;
}

static void put_text(JNIEnv *env, char *dst, size_t size, jstring text)
{
    const char *s = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    if (s) {
        snprintf(dst, size, "%s", s);
        (*env)->ReleaseStringUTFChars(env, text, s);
    }
}

/* Frame size for this boot and the names Windows shows for the computer and processor. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeConfigure(JNIEnv *env, jclass cls, jobject buffer,
    jint width, jint height, jint cpu_mhz, jstring system_maker, jstring system_model,
    jstring cpu_maker, jstring cpu_name)
{
    struct hw_header *h = (*env)->GetDirectBufferAddress(env, buffer);
    (void)cls;
    if (!ready || !h) return;
    hw_window_set_mode(h, (uint32_t)width, (uint32_t)height);
    h->cpu_mhz = (uint32_t)cpu_mhz;
    put_text(env, h->system_maker, sizeof h->system_maker, system_maker);
    put_text(env, h->system_model, sizeof h->system_model, system_model);
    put_text(env, h->cpu_maker, sizeof h->cpu_maker, cpu_maker);
    put_text(env, h->cpu_name, sizeof h->cpu_name, cpu_name);
}

/* The Windows screen size for the indirect display (hw_abi.h): the size chosen in the app and the phone's
 * own. ask != 0 means Windows is running and should switch to it now. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeSetScreen(JNIEnv *env, jclass cls, jobject buffer, jint width, jint height,
                                                   jint native_width, jint native_height, jboolean ask)
{
    struct hw_header *h = (*env)->GetDirectBufferAddress(env, buffer);
    (void)cls;
    if (!h) return;
    h->screen_native_width = (uint32_t)native_width;
    h->screen_native_height = (uint32_t)native_height;
    h->screen_want_width = (uint32_t)width;
    __atomic_store_n(&h->screen_want_height, (uint32_t)height, __ATOMIC_RELEASE);
    if (ask) __atomic_add_fetch(&h->screen_seq, 1, __ATOMIC_RELEASE);
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_WindowBridge_nativePoll(JNIEnv *env, jclass cls, jobject buffer)
{
    void *window = (*env)->GetDirectBufferAddress(env, buffer);
    (void)cls;
    if (!ready || !window) return 0;
    return hw_tpm_poll(window, &tpm);
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeCopyFrame(JNIEnv *env, jclass cls,
    jobject windowBuffer, jobject frameBuffer)
{
    void *window = (*env)->GetDirectBufferAddress(env, windowBuffer);
    void *frame = (*env)->GetDirectBufferAddress(env, frameBuffer);
    jlong capacity = (*env)->GetDirectBufferCapacity(env, frameBuffer);
    (void)cls;
    if (!ready || !window || !frame) return -1;
    struct hw_header *h = (struct hw_header *)window;
    uint8_t *pixels = (uint8_t *)frame;
    int sequence;
    uint32_t i, count = h->width * h->height;
    if (!hw_mode_ok(h) || capacity < (jlong)count * 4) return -1;
    if (__atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE) & HW_GUEST_GOP)
        sequence = hw_copy_gop_front(window, pixels, (uint32_t)capacity);
    else
        sequence = hw_copy_front(window, pixels, (uint32_t)capacity);
    if (sequence > 0 || sequence == -2) {
        /* Bitmap.copyPixelsFromBuffer expects packed AABBGGRR (RGBA bytes on LE).
         * Guest ring/GOP pixels are BGRA/BGRX. Convert only the app-side copy and
         * force opacity so legal GOP Reserved=0 cannot make desktop pixels vanish. */
        for (i = 0; i < count; ++i) {
            uint8_t blue = pixels[i * 4u];
            uint8_t green = pixels[i * 4u + 1u];
            uint8_t red = pixels[i * 4u + 2u];
            pixels[i * 4u] = red;
            pixels[i * 4u + 1u] = green;
            pixels[i * 4u + 2u] = blue;
            pixels[i * 4u + 3u] = 0xff;
        }
    }
    return sequence;
}

/* ---- presenting the guest frame ----
 * The guest's pixels go into the SurfaceView's buffer, which has the guest's size; the phone's
 * compositor scales it to the view. One pass finds the rows that changed and keeps a copy of
 * them; only those rows are locked and written, converting while they are copied. An unchanged
 * frame costs one compare and no drawing. GPU frames of fullscreen games skip all of this: they go
 * to a layer above this one (SGPU_CMD_SCANOUT, gpu/host/hwgpu_host.cpp). */

#include <pthread.h>
#include <stdlib.h>
#include <android/native_window_jni.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define FORMAT_BGRA 5   /* AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM: the guest's own byte order */

static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *present_window;
static uint8_t *present_last;        /* the frame on screen, as the guest wrote it */
static uint32_t present_bytes, present_width, present_height;
static int present_bgra;             /* the surface takes BGRA, so rows need no swap */

/* surface null: stop drawing. Returns only when no draw is in flight, as surfaceDestroyed needs. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeSetSurface(JNIEnv *env, jclass cls, jobject surface)
{
    (void)cls;
    pthread_mutex_lock(&present_lock);
    if (present_window) ANativeWindow_release(present_window);
    present_window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    present_bytes = 0;               /* draw the next frame even if it did not change */
    pthread_mutex_unlock(&present_lock);
}

/* One row of guest BGRX into the surface: alpha forced opaque, red and blue swapped unless the
 * surface is BGRA itself. */
static void put_row(uint32_t *to, const uint32_t *in, uint32_t n, int bgra)
{
    uint32_t x = 0;
#if defined(__aarch64__)
    const uint8x16_t opaque = vdupq_n_u8(0xff);
    for (; x + 16 <= n; x += 16) {
        uint8x16x4_t p = vld4q_u8((const uint8_t *)(in + x));
        if (!bgra) {
            uint8x16_t b = p.val[0];
            p.val[0] = p.val[2];
            p.val[2] = b;
        }
        p.val[3] = opaque;
        vst4q_u8((uint8_t *)(to + x), p);
    }
#endif
    for (; x < n; x++)
        to[x] = bgra ? (in[x] | 0xff000000u)
                     : (in[x] & 0x0000ff00u) | ((in[x] >> 16) & 0xffu) | ((in[x] & 0xffu) << 16) | 0xff000000u;
}

/* 1 drew a frame, 0 nothing new, -1 no guest frame or no surface. */
/* The brightest colour channel among sampled pixels of the guest frame, 0..255; -1 when there is no frame.
 * DisplayWatchdog uses it to tell a black screen from a dark one. */
JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_WindowBridge_nativeFramePeak(JNIEnv *env, jclass cls, jobject windowBuffer)
{
    struct hw_header *h = (*env)->GetDirectBufferAddress(env, windowBuffer);
    const uint8_t *src;
    uint32_t front, x, y, peak = 0;
    (void)cls;
    if (!h || !hw_mode_ok(h)) return -1;
    front = __atomic_load_n(&h->front, __ATOMIC_ACQUIRE);
    if (front > 1) return -1;
    src = (const uint8_t *)h + hw_frame_off(h, front);
    for (y = h->height / 8; y < h->height - h->height / 16; y += 4)
        for (x = h->width / 16; x < h->width - h->width / 16; x += 4) {
            uint32_t px = *(const uint32_t *)(src + (size_t)y * h->stride + (size_t)x * 4);
            uint32_t m = px & 255;
            if (((px >> 8) & 255) > m) m = (px >> 8) & 255;
            if (((px >> 16) & 255) > m) m = (px >> 16) & 255;
            if (m > peak) peak = m;
        }
    return (jint)peak;
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_WindowBridge_nativePresent(JNIEnv *env, jclass cls, jobject windowBuffer)
{
    struct hw_header *h = (*env)->GetDirectBufferAddress(env, windowBuffer);
    ANativeWindow_Buffer out;
    const uint8_t *src;
    uint32_t bytes, front, y, row, first, last;
    int rc = -1;
    (void)cls;
    if (!ready || !h || !hw_mode_ok(h)) return -1;
    front = __atomic_load_n(&h->front, __ATOMIC_ACQUIRE);
    if (front > 1 || (!(__atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE) & HW_GUEST_GOP) &&
                      !__atomic_load_n(&h->seq, __ATOMIC_ACQUIRE))) return -1;
    bytes = hw_frame_bytes(h);
    row = h->width * 4;
    src = (const uint8_t *)h + hw_frame_off(h, front);
    pthread_mutex_lock(&present_lock);
    if (!present_window) goto out;
    if (bytes != present_bytes || h->width != present_width || h->height != present_height) {
        present_width = h->width;
        present_height = h->height;
        uint8_t *grown = realloc(present_last, bytes);
        if (!grown) goto out;
        present_last = grown;
        /* BGRA saves the swap. A surface that will not take it stays RGBX. */
        present_bgra = ANativeWindow_setBuffersGeometry(present_window, (int32_t)h->width, (int32_t)h->height,
                                                        FORMAT_BGRA) == 0;
        if (!present_bgra)
            ANativeWindow_setBuffersGeometry(present_window, (int32_t)h->width, (int32_t)h->height,
                                             WINDOW_FORMAT_RGBX_8888);
        memcpy(present_last, src, bytes);
        first = 0;
        last = h->height - 1;
    } else {
        /* The rows that changed, kept as they are now: the guest keeps drawing meanwhile. */
        first = UINT32_MAX;
        last = 0;
        for (y = 0; y < h->height; y++) {
            const uint8_t *a = src + (size_t)y * h->stride;
            uint8_t *b = present_last + (size_t)y * h->stride;
            if (!memcmp(a, b, row)) continue;
            memcpy(b, a, row);
            if (first == UINT32_MAX) first = y;
            last = y;
        }
        if (first == UINT32_MAX) { rc = 0; goto out; }
    }
    ARect dirty = {0, (int32_t)first, (int32_t)h->width, (int32_t)last + 1};
    if (ANativeWindow_lock(present_window, &out, &dirty) != 0) goto out;
    if ((uint32_t)out.width >= h->width && (uint32_t)out.height >= h->height) {
        /* The surface may hand back a bigger region than asked: fill all of it. */
        uint32_t top = dirty.top < 0 ? 0 : (uint32_t)dirty.top;
        uint32_t bottom = dirty.bottom > (int32_t)h->height ? h->height : (uint32_t)dirty.bottom;
        int bgra = out.format == FORMAT_BGRA;
        for (y = top; y < bottom; y++)
            put_row((uint32_t *)((uint8_t *)out.bits + (size_t)y * (size_t)out.stride * 4),
                    (const uint32_t *)(present_last + (size_t)y * h->stride), h->width, bgra);
        present_bytes = bytes;
        rc = 1;
    }
    ANativeWindow_unlockAndPost(present_window);
out:
    pthread_mutex_unlock(&present_lock);
    return rc;
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_WindowsInstall_nativePack(JNIEnv *env, jclass cls, jstring dir, jstring image)
{
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    const char *o = (*env)->GetStringUTFChars(env, image, NULL);
    int rc;
    (void)cls;
    rc = !d || !o ? -1 : hw_pack_tree(d, o);
    if (d) (*env)->ReleaseStringUTFChars(env, dir, d);
    if (o) (*env)->ReleaseStringUTFChars(env, image, o);
    return rc;
}

JNIEXPORT jstring JNICALL
Java_dev_hyperwin_app_WindowsInstall_nativeEnableTestSigning(JNIEnv *env, jclass cls, jstring disk)
{
    const char *path = (*env)->GetStringUTFChars(env, disk, NULL);
    char status[256];
    (void)cls;
    if (!path) return (*env)->NewStringUTF(env, "Could not update the BCD.");
    hw_bcd_enable_testsigning(path, status, sizeof status);
    (*env)->ReleaseStringUTFChars(env, disk, path);
    return (*env)->NewStringUTF(env, status);
}

/* ---- machine prep ---- */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <cJSON.h>
#include "machine_prep.h"
#include "fat32.h"

typedef struct {
    JNIEnv *env;
    jobject progress;
    jmethodID step;
} PrepJni;

static void prep_progress(void *ctx, int percent, const char *message)
{
    PrepJni *p = ctx;
    jstring msg = (*p->env)->NewStringUTF(p->env, message);
    (*p->env)->CallVoidMethod(p->env, p->progress, p->step, percent, msg);
    (*p->env)->DeleteLocalRef(p->env, msg);
    if ((*p->env)->ExceptionCheck(p->env)) (*p->env)->ExceptionClear(p->env);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n + 1))) {
        if (fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = 0;
        else { free(buf); buf = NULL; }
    }
    fclose(f);
    return buf;
}

/* Puts a new copy of one of our own boot files into a machine disk's boot partition. */
JNIEXPORT jboolean JNICALL
Java_dev_hyperwin_app_MachinePrep_nativeReplaceBootFile(JNIEnv *env, jobject self, jstring disk,
                                                        jstring path, jbyteArray data)
{
    const char *d = (*env)->GetStringUTFChars(env, disk, NULL);
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    int fd = d ? open(d, O_RDWR) : -1, rc = -1;
    (void)self;
    if (fd >= 0 && p && bytes) rc = hw_fat32_replace(fd, HW_PREP_ESP_OFFSET, p, bytes, (uint32_t)n);
    if (fd >= 0) close(fd);
    if (bytes) (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    if (d) (*env)->ReleaseStringUTFChars(env, disk, d);
    return rc == 0;
}

/* Applies hw_bcd_harden to the BCD store in a machine disk's boot partition. -1 error, 0 nothing to do, 1 fixed. */
JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_MachinePrep_nativeHardenBoot(JNIEnv *env, jobject self, jstring disk, jstring scratch)
{
    static const char *bcd = "EFI/Microsoft/Boot/BCD";
    const char *d = (*env)->GetStringUTFChars(env, disk, NULL);
    const char *t = (*env)->GetStringUTFChars(env, scratch, NULL);
    int fd = d ? open(d, O_RDWR) : -1, rc = -1;
    uint32_t len = 0;
    void *data = fd >= 0 ? hw_fat32_read(fd, HW_PREP_ESP_OFFSET, bcd, &len) : NULL;
    (void)self;
    if (data && t) {
        FILE *f = fopen(t, "wb");
        int ok = f && fwrite(data, 1, len, f) == len;
        if (f) fclose(f);
        free(data);
        data = NULL;
        rc = ok ? hw_bcd_harden(t) : -1;
        if (rc == 1) {
            FILE *r = fopen(t, "rb");
            long n = -1;
            if (r && !fseek(r, 0, SEEK_END) && (n = ftell(r)) > 0 && !fseek(r, 0, SEEK_SET) && (data = malloc((size_t)n)) &&
                fread(data, 1, (size_t)n, r) == (size_t)n)
                rc = hw_fat32_replace(fd, HW_PREP_ESP_OFFSET, bcd, data, (uint32_t)n) == 0 ? 1 : -1;
            else rc = -1;
            if (r) fclose(r);
        }
    }
    free(data);
    if (t) unlink(t);
    if (fd >= 0) close(fd);
    if (t) (*env)->ReleaseStringUTFChars(env, scratch, t);
    if (d) (*env)->ReleaseStringUTFChars(env, disk, d);
    return rc;
}

/* json: path of machine.json; system.img is written next to it. Returns "ok" or an error message. */
JNIEXPORT jstring JNICALL
Java_dev_hyperwin_app_MachinePrep_nativePrepare(JNIEnv *env, jobject self, jstring json, jint iso_fd,
                                                jstring assets, jobject progress)
{
    const char *jp = (*env)->GetStringUTFChars(env, json, NULL);
    const char *ap = (*env)->GetStringUTFChars(env, assets, NULL);
    char err[512] = "ok", dir[1024], disk[1100], work[1100], iso[64];
    char *text = jp ? slurp(jp) : NULL;
    cJSON *m = text ? cJSON_Parse(text) : NULL;
    PrepJni pj = {env, progress, NULL};
    HwPrepRequest r;
    const cJSON *v;
    char *slash;
    (void)self;
    memset(&r, 0, sizeof r);
    if (!m || !ap) {
        snprintf(err, sizeof err, "Could not read machine settings.");
        goto out;
    }
    snprintf(dir, sizeof dir, "%s", jp);
    slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    snprintf(disk, sizeof disk, "%s/system.img", dir);
    snprintf(work, sizeof work, "%s/prep", dir);
    snprintf(iso, sizeof iso, "/proc/self/fd/%d", (int)iso_fd);
    pj.step = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, progress), "step", "(ILjava/lang/String;)V");
    r.iso_path = iso;
    r.assets_dir = ap;
    r.disk_path = disk;
    r.work_dir = work;
    v = cJSON_GetObjectItemCaseSensitive(m, "diskSizeGb");
    r.disk_bytes = (uint64_t)(cJSON_IsNumber(v) ? v->valueint : 64) << 30;
    v = cJSON_GetObjectItemCaseSensitive(m, "imageIndex");
    r.image_index = cJSON_IsNumber(v) ? v->valueint : 1;
    v = cJSON_GetObjectItemCaseSensitive(m, "username");
    r.username = cJSON_IsString(v) ? v->valuestring : "User";
    v = cJSON_GetObjectItemCaseSensitive(m, "password");
    r.password = cJSON_IsString(v) ? v->valuestring : "";
    v = cJSON_GetObjectItemCaseSensitive(m, "computerName");
    r.computer_name = cJSON_IsString(v) ? v->valuestring : "HYPERWIN";
    r.progress = pj.step ? prep_progress : NULL;
    r.ctx = &pj;
    hw_prepare_machine(&r, err, sizeof err);
out:
    cJSON_Delete(m);
    free(text);
    if (jp) (*env)->ReleaseStringUTFChars(env, json, jp);
    if (ap) (*env)->ReleaseStringUTFChars(env, assets, ap);
    return (*env)->NewStringUTF(env, err);
}

/* Offline repair: deletes files inside the Windows partition of a machine disk before the machine
 * starts, for when a driver stops Windows booting. Each path is like /Windows/System32/x.sys.
 * The partition is the biggest GPT entry. Returns how many were deleted, or -1. */
#include <sys/stat.h>
#include <time.h>
#define HAVE_SYS_STAT_H 1
#include <errno.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>

/* A directory component written as prefix* stands for every entry starting with prefix. */
struct hw_ls { const char *prefix; char names[16][160]; int n; };
static int hw_ls_fill(void *de, const ntfschar *name, const int len, const int type, const s64 pos, const MFT_REF mref, const unsigned dt)
{
    (void)type; (void)pos; (void)mref; (void)dt;
    struct hw_ls *l = de;
    char *mb = NULL;
    if (ntfs_ucstombs(name, len, &mb, 0) < 0 || !mb) return 0;
    if (!strncmp(mb, l->prefix, strlen(l->prefix)) && l->n < 16 && strlen(mb) < 160) strcpy(l->names[l->n++], mb);
    free(mb);
    return 0;
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_MachinePrep_nativeNtfsDelete(JNIEnv *env, jobject self, jstring disk, jobjectArray paths)
{
    (void)self;
    const char *d = (*env)->GetStringUTFChars(env, disk, NULL);
    int fd = open(d, O_RDONLY);
    int done = fd < 0 ? -2 : -3;
    if (fd >= 0) {
        unsigned char hdr[512], ent[128];
        uint64_t best = 0, best_first = 0;
        if (pread(fd, hdr, sizeof hdr, 512) == (ssize_t)sizeof hdr) {
            uint64_t tab = *(uint64_t *)(hdr + 72);
            uint32_t n = *(uint32_t *)(hdr + 80), sz = *(uint32_t *)(hdr + 84);
            for (uint32_t i = 0; i < n && sz == sizeof ent; i++) {
                if (pread(fd, ent, sizeof ent, (off_t)(tab * 512 + (uint64_t)i * sz)) != (ssize_t)sizeof ent) break;
                uint64_t first = *(uint64_t *)(ent + 32), last = *(uint64_t *)(ent + 40);
                if (last >= first && last - first + 1 > best) { best = last - first + 1; best_first = first; }
            }
        }
        close(fd);
        if (best) {
            char win[1300];
            snprintf(win, sizeof win, "hwwin:%llu:%llu:%s", (unsigned long long)(best_first * 512), (unsigned long long)(best * 512), d);
            errno = 0;
            struct ntfs_device *ndev = ntfs_device_alloc(win, 0, &ntfs_device_default_io_ops, NULL);
            ntfs_volume *vol = ndev ? ntfs_device_mount(ndev, NTFS_MNT_RDONLY | NTFS_MNT_IGNORE_HIBERFILE) : NULL;
            if (!vol) done = -100 - errno;
            if (vol) {
                done = 0;
                jsize count = (*env)->GetArrayLength(env, paths);
                for (jsize i = 0; i < count; i++) {
                    jstring js = (jstring)(*env)->GetObjectArrayElement(env, paths, i);
                    const char *path0 = (*env)->GetStringUTFChars(env, js, NULL);
                    char expanded[16][1024];
                    int ne = 0;
                    const char *star = strchr(path0, '*');
                    if (!star) {
                        snprintf(expanded[ne++], sizeof expanded[0], "%s", path0);
                    } else {
                        char parent[1024], prefix[160];
                        const char *cs = star;
                        while (cs > path0 && cs[-1] != '/') cs--;
                        snprintf(parent, sizeof parent, "%.*s", (int)(cs - path0) > 1 ? (int)(cs - path0) - 1 : 1, path0);
                        snprintf(prefix, sizeof prefix, "%.*s", (int)(star - cs), cs);
                        const char *rest = strchr(star, '/');
                        ntfs_inode *pi = ntfs_pathname_to_inode(vol, NULL, parent);
                        if (pi) {
                            struct hw_ls ls = { prefix, { { 0 } }, 0 };
                            s64 pos = 0;
                            ntfs_readdir(pi, &pos, &ls, hw_ls_fill);
                            ntfs_inode_close(pi);
                            for (int k = 0; k < ls.n && ne < 16; k++)
                                snprintf(expanded[ne++], sizeof expanded[0], "%s/%s%s", parent[1] ? parent : "", ls.names[k], rest ? rest : "");
                        }
                    }
                    for (int e = 0; e < ne; e++) {
                    const char *path = expanded[e];
                    char dir[1024];
                    snprintf(dir, sizeof dir, "%s", path);
                    char *slash = strrchr(dir, '/');
                    if (slash && slash != dir) {
                        *slash = 0;
                        /* The volume may hold Windows' cached metadata, which must not be changed. File data is
                         * not journaled, so the first cluster of the file is overwritten in place instead. */
                        ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
                        (void)dir;
                        if (ni) {
                            ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
                            if (na && NAttrNonResident(na) && ntfs_attr_map_whole_runlist(na) == 0) {
                                LCN lcn = ntfs_attr_vcn_to_lcn(na, 0);
                                int wfd = open(d, O_RDWR);
                                if (lcn >= 0 && wfd >= 0) {
                                    static const unsigned char zeros[4096];
                                    off_t at = (off_t)(best_first * 512 + (uint64_t)lcn * (uint64_t)vol->cluster_size);
                                    if (pwrite(wfd, zeros, sizeof zeros, at) == (ssize_t)sizeof zeros) done++;
                                }
                                if (wfd >= 0) close(wfd);
                            }
                            if (na) ntfs_attr_close(na);
                            ntfs_inode_close(ni);
                        }
                        ntfschar *uname = NULL;
                        (void)slash;
                        free(uname);
                    }
                    }
                    (*env)->ReleaseStringUTFChars(env, js, path0);
                }
                ntfs_umount(vol, 0);
            }
        }
    }
    (*env)->ReleaseStringUTFChars(env, disk, d);
    return done;
}

#include <android/log.h>
#include <dirent.h>
#include <strings.h>
#include <ntfs-3g/logging.h>
#include "sha256.h"

static int hw_ntfs_log(const char *function, const char *file, int line, u32 level,
                       void *data, const char *format, va_list args)
{
    (void)function; (void)file; (void)line; (void)level; (void)data;
    __android_log_vprint(ANDROID_LOG_WARN, "HyperWinNtfs", format, args);
    return 0;
}

typedef struct {
    char *host;
    char *ntfs;
} PatchItem;

typedef struct {
    PatchItem *items;
    int count;
    int cap;
} PatchList;

static int patch_list_add(PatchList *l, const char *host, const char *ntfs)
{
    if (l->count >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 64;
        PatchItem *ni = realloc(l->items, (size_t)ncap * sizeof *ni);
        if (!ni) return -1;
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->count].host = strdup(host);
    l->items[l->count].ntfs = strdup(ntfs);
    if (!l->items[l->count].host || !l->items[l->count].ntfs) return -1;
    l->count++;
    return 0;
}

static void patch_list_free(PatchList *l)
{
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].host);
        free(l->items[i].ntfs);
    }
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

static int patch_collect_tree(const char *host_dir, const char *ntfs_dir, PatchList *list)
{
    DIR *d = opendir(host_dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char hp[1200], np[1200];
        snprintf(hp, sizeof hp, "%s/%s", host_dir, de->d_name);
        snprintf(np, sizeof np, "%s/%s", ntfs_dir, de->d_name);
        struct stat st;
        if (stat(hp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (patch_collect_tree(hp, np, list) != 0) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            if (patch_list_add(list, hp, np) != 0) { closedir(d); return -1; }
        }
    }
    closedir(d);
    return 0;
}

static int hw_ntfs_exists(ntfs_volume *vol, const char *path)
{
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
    if (!ni) return 0;
    ntfs_inode_close(ni);
    return 1;
}

static int hw_ntfs_mkdir_p(ntfs_volume *vol, const char *path)
{
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", path);
    for (char *p = cur + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        if (!hw_ntfs_exists(vol, cur)) {
            char parent[1024];
            char *slash = strrchr(cur, '/');
            const char *name = slash ? slash + 1 : cur;
            if (slash && slash != cur) snprintf(parent, sizeof parent, "%.*s", (int)(slash - cur), cur);
            else snprintf(parent, sizeof parent, "/");
            ntfs_inode *pni = ntfs_pathname_to_inode(vol, NULL, parent);
            if (!pni) return -1;
            ntfschar *uname = NULL;
            int ulen = ntfs_mbstoucs(name, &uname);
            if (ulen <= 0 || !uname) { ntfs_inode_close(pni); free(uname); return -1; }
            ntfs_inode *cni = ntfs_create(pni, 0, uname, (u8)ulen, S_IFDIR);
            free(uname);
            ntfs_inode_close(pni);
            if (!cni) return -1;
            ntfs_inode_close(cni);
        }
        *p = saved;
        if (!saved) break;
    }
    return 0;
}

static ntfs_inode *hw_ntfs_open_or_create(ntfs_volume *vol, const char *ntfs_path)
{
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, ntfs_path);
    if (ni) return ni;
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", ntfs_path);
    char *slash = strrchr(parent, '/');
    if (!slash) return NULL;
    const char *name = slash + 1;
    if (slash == parent) parent[1] = '\0';
    else {
        *slash = '\0';
        if (hw_ntfs_mkdir_p(vol, parent) != 0) return NULL;
    }
    ntfs_inode *pni = ntfs_pathname_to_inode(vol, NULL, parent);
    if (!pni) return NULL;
    ntfschar *uname = NULL;
    int ulen = ntfs_mbstoucs(name, &uname);
    if (ulen <= 0 || !uname) { ntfs_inode_close(pni); free(uname); return NULL; }
    ni = ntfs_create(pni, 0, uname, (u8)ulen, S_IFREG);
    free(uname);
    ntfs_inode_close(pni);
    return ni;
}

static int hw_ntfs_write_bytes(ntfs_volume *vol, const char *ntfs_path, const void *data, size_t len)
{
    ntfs_inode *ni = hw_ntfs_open_or_create(vol, ntfs_path);
    if (!ni) return -1;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) { ntfs_inode_close(ni); return -1; }
    int rc = 0;
    if (ntfs_attr_truncate(na, 0) != 0) rc = -1;
    else if (len > 0 && ntfs_attr_pwrite(na, 0, (s64)len, data) != (s64)len) rc = -1;
    ntfs_attr_close(na);
    ntfs_inode_mark_dirty(ni);
    ntfs_inode_close(ni);
    return rc;
}

static int hw_ntfs_copy_file(ntfs_volume *vol, const char *ntfs_path, const char *host_path, int *updated)
{
    static unsigned char hbuf[65536], nbuf[65536];
    int fd = open(host_path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    s64 size = (s64)st.st_size;

    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, ntfs_path);
    if (ni) {
        ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (na && na->data_size == size) {
            int same = 1;
            s64 pos = 0;
            while (pos < size) {
                s64 chunk = size - pos < (s64)sizeof hbuf ? size - pos : (s64)sizeof hbuf;
                if (pread(fd, hbuf, (size_t)chunk, (off_t)pos) != (ssize_t)chunk ||
                    ntfs_attr_pread(na, pos, chunk, nbuf) != chunk ||
                    memcmp(hbuf, nbuf, (size_t)chunk) != 0) {
                    same = 0;
                    break;
                }
                pos += chunk;
            }
            if (same) {
                ntfs_attr_close(na);
                ntfs_inode_close(ni);
                close(fd);
                *updated = 0;
                return 0;
            }
        }
        if (na) ntfs_attr_close(na);
    } else {
        ni = hw_ntfs_open_or_create(vol, ntfs_path);
        if (!ni) { close(fd); return -1; }
    }

    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) { ntfs_inode_close(ni); close(fd); return -1; }
    int rc = 0;
    if (ntfs_attr_truncate(na, 0) != 0) {
        rc = -1;
    } else {
        s64 pos = 0;
        while (pos < size) {
            s64 chunk = size - pos < (s64)sizeof hbuf ? size - pos : (s64)sizeof hbuf;
            if (pread(fd, hbuf, (size_t)chunk, (off_t)pos) != (ssize_t)chunk ||
                ntfs_attr_pwrite(na, pos, chunk, hbuf) != chunk) {
                rc = -1;
                break;
            }
            pos += chunk;
        }
    }
    ntfs_attr_close(na);
    ntfs_inode_mark_dirty(ni);
    ntfs_inode_close(ni);
    close(fd);
    if (rc == 0) *updated = 1;
    return rc;
}

static int hw_ntfs_file_equals(ntfs_volume *vol, const char *ntfs_path, const char *host_path)
{
    static unsigned char hbuf[65536], nbuf[65536];
    int fd = open(host_path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    s64 size = (s64)st.st_size;
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, ntfs_path);
    if (!ni) { close(fd); return 0; }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    int same = 0;
    if (na && na->data_size == size) {
        same = 1;
        s64 pos = 0;
        while (pos < size) {
            s64 chunk = size - pos < (s64)sizeof hbuf ? size - pos : (s64)sizeof hbuf;
            if (pread(fd, hbuf, (size_t)chunk, (off_t)pos) != (ssize_t)chunk ||
                ntfs_attr_pread(na, pos, chunk, nbuf) != chunk ||
                memcmp(hbuf, nbuf, (size_t)chunk) != 0) {
                same = 0;
                break;
            }
            pos += chunk;
        }
    }
    if (na) ntfs_attr_close(na);
    ntfs_inode_close(ni);
    close(fd);
    return same;
}

static int hw_ls_ci_fill(void *de, const ntfschar *name, const int len, const int type, const s64 pos, const MFT_REF mref, const unsigned dt)
{
    (void)type; (void)pos; (void)mref; (void)dt;
    struct hw_ls *l = de;
    char *mb = NULL;
    if (ntfs_ucstombs(name, len, &mb, 0) < 0 || !mb) return 0;
    if (!strncasecmp(mb, l->prefix, strlen(l->prefix)) && l->n < 16 && strlen(mb) < 160) strcpy(l->names[l->n++], mb);
    free(mb);
    return 0;
}

static int cmp_str_ci(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static int hw_ends_with_ci(const char *s, const char *suf)
{
    size_t sl = strlen(s), fl = strlen(suf);
    return sl >= fl && strcasecmp(s + sl - fl, suf) == 0;
}

static int hw_pkg_sha256(const char *pkg_dir, char hex_out[65])
{
    DIR *d = opendir(pkg_dir);
    if (!d) return -1;
    char *names[64];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < 64) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcasecmp(de->d_name, "installed.sha") || !strcasecmp(de->d_name, "install.log") || hw_ends_with_ci(de->d_name, ".cat")) continue;
        char hp[1200];
        snprintf(hp, sizeof hp, "%s/%s", pkg_dir, de->d_name);
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISREG(st.st_mode)) names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_str_ci);

    size_t cap = 4096, len = 0;
    char *joined = malloc(cap);
    if (!joined) { for (int i = 0; i < n; i++) free(names[i]); return -1; }
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        char hp[1200];
        snprintf(hp, sizeof hp, "%s/%s", pkg_dir, names[i]);
        int fd = open(hp, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); ok = 0; break; }
        uint8_t *buf = malloc((size_t)(st.st_size > 0 ? st.st_size : 1));
        if (!buf || (st.st_size > 0 && read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size)) {
            free(buf); close(fd); ok = 0; break;
        }
        close(fd);
        uint8_t digest[32];
        hw_sha256(buf, (size_t)st.st_size, digest);
        free(buf);
        size_t need = len + strlen(names[i]) + 1 + 64 + 2;
        if (need > cap) {
            cap = need * 2;
            char *nj = realloc(joined, cap);
            if (!nj) { ok = 0; break; }
            joined = nj;
        }
        if (i > 0) joined[len++] = ';';
        len += (size_t)snprintf(joined + len, cap - len, "%s:", names[i]);
        for (int b = 0; b < 32; b++) len += (size_t)snprintf(joined + len, cap - len, "%02X", digest[b]);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    if (!ok) { free(joined); return -1; }

    uint8_t final_digest[32];
    hw_sha256((const uint8_t *)joined, len, final_digest);
    free(joined);
    for (int b = 0; b < 32; b++) snprintf(hex_out + b * 2, 3, "%02X", final_digest[b]);
    hex_out[64] = '\0';
    return 0;
}

/* Patches an already-installed PnP driver package directly inside DriverStore\FileRepository.
 * Returns 1 if patched offline (no PnP reinstall needed), 0 if gpu-install.cmd is needed, -1 on error. */
static int hw_patch_driverstore_pkg(ntfs_volume *vol, const char *assets_dir, const char *pkg, int *written)
{
    char pkg_dir[1100], inf_host[1200], prefix[160];
    snprintf(pkg_dir, sizeof pkg_dir, "%s/guest/gpu/%s", assets_dir, pkg);
    snprintf(inf_host, sizeof inf_host, "%s/%s.inf", pkg_dir, pkg);
    if (access(inf_host, R_OK) != 0) return 1;

    ntfs_inode *repo = ntfs_pathname_to_inode(vol, NULL, "/Windows/System32/DriverStore/FileRepository");
    if (!repo) return 0;
    snprintf(prefix, sizeof prefix, "%s.inf_", pkg);
    struct hw_ls ls = { prefix, { { 0 } }, 0 };
    s64 pos = 0;
    ntfs_readdir(repo, &pos, &ls, hw_ls_ci_fill);
    ntfs_inode_close(repo);
    if (ls.n == 0) return 0;

    for (int r = 0; r < ls.n; r++) {
        char inf_ntfs[1200];
        snprintf(inf_ntfs, sizeof inf_ntfs, "/Windows/System32/DriverStore/FileRepository/%s/%s.inf", ls.names[r], pkg);
        if (!hw_ntfs_file_equals(vol, inf_ntfs, inf_host)) return 0;
    }

    DIR *d = opendir(pkg_dir);
    if (!d) return 0;
    char *files[64];
    int nf = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && nf < 64) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (hw_ends_with_ci(de->d_name, ".inf") || hw_ends_with_ci(de->d_name, ".cat") ||
            !strcasecmp(de->d_name, "installed.sha") || !strcasecmp(de->d_name, "install.log")) continue;
        char hp[1200];
        snprintf(hp, sizeof hp, "%s/%s", pkg_dir, de->d_name);
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISREG(st.st_mode)) files[nf++] = strdup(de->d_name);
    }
    closedir(d);

    for (int r = 0; r < ls.n; r++) {
        for (int i = 0; i < nf; i++) {
            char np[1200];
            snprintf(np, sizeof np, "/Windows/System32/DriverStore/FileRepository/%s/%s", ls.names[r], files[i]);
            if (!hw_ntfs_exists(vol, np)) {
                for (int k = 0; k < nf; k++) free(files[k]);
                return 0;
            }
        }
    }

    for (int i = 0; i < nf; i++) {
        char hp[1200];
        snprintf(hp, sizeof hp, "%s/%s", pkg_dir, files[i]);
        for (int r = 0; r < ls.n; r++) {
            char np[1200];
            int did = 0;
            snprintf(np, sizeof np, "/Windows/System32/DriverStore/FileRepository/%s/%s", ls.names[r], files[i]);
            if (hw_ntfs_copy_file(vol, np, hp, &did) != 0) {
                for (int k = 0; k < nf; k++) free(files[k]);
                return -1;
            }
            if (did) (*written)++;
        }
        if (hw_ends_with_ci(files[i], ".sys")) {
            char dp[1200];
            snprintf(dp, sizeof dp, "/Windows/System32/drivers/%s", files[i]);
            if (hw_ntfs_exists(vol, dp)) {
                int did = 0;
                if (hw_ntfs_copy_file(vol, dp, hp, &did) != 0) {
                    for (int k = 0; k < nf; k++) free(files[k]);
                    return -1;
                }
                if (did) (*written)++;
            }
        } else if (hw_ends_with_ci(files[i], ".dll")) {
            char up[1200];
            snprintf(up, sizeof up, "/Windows/System32/drivers/UMDF/%s", files[i]);
            if (hw_ntfs_exists(vol, up)) {
                int did = 0;
                if (hw_ntfs_copy_file(vol, up, hp, &did) != 0) {
                    for (int k = 0; k < nf; k++) free(files[k]);
                    return -1;
                }
                if (did) (*written)++;
            }
        }
    }
    for (int k = 0; k < nf; k++) free(files[k]);

    char hex[65], sha_ntfs[1200];
    if (hw_pkg_sha256(pkg_dir, hex) == 0) {
        snprintf(sha_ntfs, sizeof sha_ntfs, "/HyperWin/gpu/%s/installed.sha", pkg);
        hw_ntfs_write_bytes(vol, sha_ntfs, hex, 64);
    }
    return 1;
}

JNIEXPORT jstring JNICALL
Java_dev_hyperwin_app_MachinePrep_nativePatchGuest(JNIEnv *env, jobject self, jstring disk,
                                                   jstring assets, jobject progress)
{
    (void)self;
    const char *d = (*env)->GetStringUTFChars(env, disk, NULL);
    const char *a = (*env)->GetStringUTFChars(env, assets, NULL);
    char err[512] = "ok";
    PrepJni pj = {env, progress, NULL};
    PatchList list = {0};
    ntfs_volume *vol = NULL;
    if (progress) {
        pj.step = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, progress), "step", "(ILjava/lang/String;)V");
    }

    int fd = d ? open(d, O_RDONLY) : -1;
    uint64_t best = 0, best_first = 0;
    if (fd >= 0) {
        unsigned char hdr[512], ent[128];
        if (pread(fd, hdr, sizeof hdr, 512) == (ssize_t)sizeof hdr) {
            uint64_t tab = *(uint64_t *)(hdr + 72);
            uint32_t n = *(uint32_t *)(hdr + 80), sz = *(uint32_t *)(hdr + 84);
            for (uint32_t i = 0; i < n && sz == sizeof ent; i++) {
                if (pread(fd, ent, sizeof ent, (off_t)(tab * 512 + (uint64_t)i * sz)) != (ssize_t)sizeof ent) break;
                uint64_t first = *(uint64_t *)(ent + 32), last = *(uint64_t *)(ent + 40);
                if (last >= first && last - first + 1 > best) { best = last - first + 1; best_first = first; }
            }
        }
        close(fd);
    }
    if (!best || !a) {
        snprintf(err, sizeof err, "Could not find the Windows partition on disk.");
        goto out;
    }

    char win[1300];
    snprintf(win, sizeof win, "hwwin:%llu:%llu:%s", (unsigned long long)(best_first * 512),
             (unsigned long long)(best * 512), d);
    ntfs_log_set_handler(hw_ntfs_log);
    ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_WARNING);
    errno = 0;
    struct ntfs_device *ndev = ntfs_device_alloc(win, 0, &ntfs_device_default_io_ops, NULL);
    vol = ndev ? ntfs_device_mount(ndev, NTFS_MNT_RECOVER | NTFS_MNT_IGNORE_HIBERFILE) : NULL;
    if (!vol) {
        snprintf(err, sizeof err,
                 "Could not open the Windows partition (error %d). Start Windows and shut it down normally, then try again.",
                 errno);
        goto out;
    }

    char guest_dir[1100], drv_dir[1100], hp[1200];
    snprintf(guest_dir, sizeof guest_dir, "%s/guest", a);
    snprintf(drv_dir, sizeof drv_dir, "%s/drivers", a);
    if (patch_collect_tree(guest_dir, "/HyperWin", &list) != 0) {
        snprintf(err, sizeof err, "Could not read app guest files.");
        goto out;
    }
    if (patch_collect_tree(drv_dir, "/Windows/System32/drivers", &list) != 0) {
        snprintf(err, sizeof err, "Could not read app driver files.");
        goto out;
    }
    snprintf(hp, sizeof hp, "%s/guest/gpu/hwgpu.sys", a);
    if (access(hp, R_OK) == 0) patch_list_add(&list, hp, "/Windows/System32/drivers/hwgpu.sys");
    snprintf(hp, sizeof hp, "%s/guest/gpu/hwfiles.sys", a);
    if (access(hp, R_OK) == 0) patch_list_add(&list, hp, "/Windows/System32/drivers/hwfiles.sys");
    snprintf(hp, sizeof hp, "%s/guest/gpu/hwfiles.exe", a);
    if (access(hp, R_OK) == 0 && hw_ntfs_exists(vol, "/HyperWin/files")) {
        patch_list_add(&list, hp, "/HyperWin/files/hwfiles.exe");
    }
    snprintf(hp, sizeof hp, "%s/guest/gpu/vulkan-1.dll", a);
    if (access(hp, R_OK) == 0 && hw_ntfs_exists(vol, "/HyperWin/gpu/vulkan-1.ours")) {
        patch_list_add(&list, hp, "/Windows/System32/vulkan-1.dll");
    }
    snprintf(hp, sizeof hp, "%s/guest/gpu/vulkan-1_x86.dll", a);
    if (access(hp, R_OK) == 0 && hw_ntfs_exists(vol, "/HyperWin/gpu/vulkan-1-x86.ours")) {
        patch_list_add(&list, hp, "/Windows/SysWOW64/vulkan-1.dll");
    }

    char cmd_host[1200];
    snprintf(cmd_host, sizeof cmd_host, "%s/guest/gpu-install.cmd", a);
    int need_pnp = !hw_ntfs_file_equals(vol, "/HyperWin/gpu-install.cmd", cmd_host);

    int written = 0;
    for (int i = 0; i < list.count; i++) {
        if (pj.step) {
            int pct = (i * 90) / (list.count > 0 ? list.count : 1);
            prep_progress(&pj, pct, "Updating Windows files…");
        }
        int did = 0;
        if (hw_ntfs_copy_file(vol, list.items[i].ntfs, list.items[i].host, &did) != 0) {
            snprintf(err, sizeof err, "Could not write %s into Windows.", list.items[i].ntfs);
            goto out;
        }
        if (did) written++;
    }

    static const char *const pnp_pkgs[] = { "hwgpuw", "hwidd", "hwaudio", "hwhid" };
    for (int k = 0; k < 4; k++) {
        if (pj.step) prep_progress(&pj, 90 + k * 2, "Updating DriverStore…");
        int r = hw_patch_driverstore_pkg(vol, a, pnp_pkgs[k], &written);
        if (r < 0) {
            snprintf(err, sizeof err, "Could not update DriverStore package %s.", pnp_pkgs[k]);
            goto out;
        }
        if (r == 0) need_pnp = 1;
    }

    if (need_pnp) {
        if (hw_ntfs_write_bytes(vol, "/HyperWin/gpu/pending.install", "1\r\n", 3) != 0) {
            snprintf(err, sizeof err, "Could not write update marker into Windows.");
            goto out;
        }
    }
    if (pj.step) prep_progress(&pj, 100, "Done");
    __android_log_print(ANDROID_LOG_INFO, "HyperWin", "offline guest patch: updated %d files (need_pnp=%d)", written, need_pnp);

out:
    if (strcmp(err, "ok") != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "HyperWin", "offline guest patch failed: %s (errno=%d)", err, errno);
    }
    if (vol) ntfs_umount(vol, 0);
    patch_list_free(&list);
    if (d) (*env)->ReleaseStringUTFChars(env, disk, d);
    if (a) (*env)->ReleaseStringUTFChars(env, assets, a);
    return (*env)->NewStringUTF(env, err);
}
