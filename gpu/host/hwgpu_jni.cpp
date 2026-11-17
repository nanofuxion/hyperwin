// JNI entry for the app: dev.hyperwin.app.GpuService.nativeStart(window, heap, flags, width, height) and
// nativeStop(). window is the direct ByteBuffer the app already maps for the shared window.
#include <jni.h>

#include "hwgpu_host.h"

extern "C" JNIEXPORT jint JNICALL Java_dev_hyperwin_app_GpuService_nativeStart(JNIEnv* env, jobject, jobject window,
                                                                              jobject heap, jint flags, jint width,
                                                                              jint height) {
    void* p = env->GetDirectBufferAddress(window);
    jlong n = env->GetDirectBufferCapacity(window);
    if (!p || n <= 0) return -22;
    void* h = heap ? env->GetDirectBufferAddress(heap) : nullptr;
    jlong hn = h ? env->GetDirectBufferCapacity(heap) : 0;
    return hwgpu_host_start(p, size_t(n), h, size_t(hn > 0 ? hn : 0), uint32_t(flags), uint32_t(width),
                            uint32_t(height));
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeStop(JNIEnv*, jobject) { hwgpu_host_stop(); }
