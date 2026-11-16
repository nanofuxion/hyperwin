#include <jni.h>
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
    if (!ready || !window || !frame || capacity < (jlong)(HW_WIDTH * HW_HEIGHT * 4u)) return -1;
    struct hw_header *h = (struct hw_header *)window;
    uint8_t *pixels = (uint8_t *)frame;
    int sequence;
    uint32_t i;
    if (__atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE) & HW_GUEST_GOP)
        sequence = hw_copy_gop_front(window, pixels, (uint32_t)capacity);
    else
        sequence = hw_copy_front(window, pixels, (uint32_t)capacity);
    if (sequence > 0 || sequence == -2) {
        /* Bitmap.copyPixelsFromBuffer expects packed AABBGGRR (RGBA bytes on LE).
         * Guest ring/GOP pixels are BGRA/BGRX. Convert only the app-side copy and
         * force opacity so legal GOP Reserved=0 cannot make desktop pixels vanish. */
        for (i = 0; i < HW_WIDTH * HW_HEIGHT; ++i) {
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

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_WindowBridge_nativePad(JNIEnv *env, jclass cls, jobject buffer,
    jint connected, jint packet, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry)
{
    void *window = (*env)->GetDirectBufferAddress(env, buffer);
    (void)env;
    (void)cls;
    if (!window) return;
    hw_set_pad(window, (uint32_t)connected, (uint32_t)packet, (uint16_t)buttons,
               (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry);
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
