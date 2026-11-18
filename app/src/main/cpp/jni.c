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
static uint32_t present_bytes;
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
    if (bytes != present_bytes) {
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
