// JNI entry for the app: dev.hyperwin.app.HwGpu.nativeStart(window, flags, width, height) and
// nativeStop(). window is the direct ByteBuffer the app already maps for the shared window.
#include <jni.h>

#include "hwgpu_host.h"

extern "C" JNIEXPORT jint JNICALL Java_dev_hyperwin_app_HwGpu_nativeStart(JNIEnv* env, jclass, jobject window,
                                                                         jint flags, jint width, jint height) {
    void* p = env->GetDirectBufferAddress(window);
    jlong n = env->GetDirectBufferCapacity(window);
    if (!p || n <= 0) return -22;
    return hwgpu_host_start(p, size_t(n), uint32_t(flags), uint32_t(width), uint32_t(height));
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_HwGpu_nativeStop(JNIEnv*, jclass) { hwgpu_host_stop(); }
