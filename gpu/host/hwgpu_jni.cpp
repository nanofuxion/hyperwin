// JNI entry for the app: dev.hyperwin.app.GpuService.nativeStart(window, heap, flags, width, height) and
// nativeStop(), nativeSetLayer(surfaceControl, width, height). window is the direct ByteBuffer the
// app already maps for the shared window.
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/surface_control_jni.h>

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

// The layer the app process made for scanout, lent to this process, or null to stop using it.
extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetLayer(JNIEnv* env, jobject, jobject layer,
                                                                                 jint width, jint height) {
    hwgpu_host_set_layer(layer ? ASurfaceControl_fromJava(env, layer) : nullptr, int(width), int(height));
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetLayerSize(JNIEnv*, jobject, jint width,
                                                                                     jint height) {
    hwgpu_host_set_layer_size(int(width), int(height));
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetVsync(JNIEnv*, jobject, jboolean on) {
    hwgpu_host_set_vsync(on ? 1 : 0);
}

// HwD3D's layer and a Surface on it, or nulls.
extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetD3dLayer(JNIEnv* env, jobject, jobject layer,
                                                                                    jobject surface) {
    hwgpu_host_set_d3d_layer(layer ? ASurfaceControl_fromJava(env, layer) : nullptr,
                             surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}
