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

// ---- the desktop and games processes (hwgpu_host.h) ----
// The service calls back into Kotlin from the channel threads: GpuService.handoff and GpuService.hole.

namespace {
JavaVM* g_vm = nullptr;
jobject g_service = nullptr;
jmethodID g_handoff = nullptr, g_hole = nullptr;

JNIEnv* attached() {
    JNIEnv* env = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    // A channel thread: attached for its life, and let go when it ends.
    static thread_local struct Detach {
        ~Detach() { if (g_vm) g_vm->DetachCurrentThread(); }
    } detach;
    return g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}

int on_handoff(uint32_t slot, uint32_t generation, const char* exe, const char* conf) {
    JNIEnv* env = attached();
    if (!env || !g_service) return 0;
    jstring e = env->NewStringUTF(exe), f = env->NewStringUTF(conf);
    jboolean ok = env->CallBooleanMethod(g_service, g_handoff, jint(slot), jint(generation), e, f);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = JNI_FALSE; }
    env->DeleteLocalRef(e);
    env->DeleteLocalRef(f);
    return ok ? 1 : 0;
}

void on_hole(int on, int32_t l, int32_t t, int32_t r, int32_t b) {
    JNIEnv* env = attached();
    if (!env || !g_service) return;
    env->CallVoidMethod(g_service, g_hole, jboolean(on != 0), jint(l), jint(t), jint(r), jint(b));
    if (env->ExceptionCheck()) env->ExceptionClear();
}
}  // namespace

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetCallbacks(JNIEnv* env, jobject self) {
    env->GetJavaVM(&g_vm);
    if (g_service) env->DeleteGlobalRef(g_service);
    g_service = env->NewGlobalRef(self);
    jclass k = env->GetObjectClass(self);
    g_handoff = env->GetMethodID(k, "handoff", "(IILjava/lang/String;Ljava/lang/String;)Z");
    g_hole = env->GetMethodID(k, "hole", "(ZIIII)V");
    hwgpu_host_set_callbacks(on_handoff, on_hole);
}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_hyperwin_app_GpuService_nativeAdopt(JNIEnv* env, jobject, jint slot,
                                                                                  jint generation, jstring exe,
                                                                                  jstring conf) {
    const char* e = env->GetStringUTFChars(exe, nullptr);
    const char* f = env->GetStringUTFChars(conf, nullptr);
    int ok = hwgpu_host_adopt(uint32_t(slot), uint32_t(generation), e, f);
    env->ReleaseStringUTFChars(exe, e);
    env->ReleaseStringUTFChars(conf, f);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeGamesDied(JNIEnv*, jobject) {
    hwgpu_host_games_died();
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetHole(JNIEnv*, jobject, jboolean on, jint l,
                                                                                jint t, jint r, jint b) {
    hwgpu_host_set_hole(on ? 1 : 0, l, t, r, b);
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetUnderLayer(JNIEnv* env, jobject,
                                                                                      jobject layer) {
    hwgpu_host_set_under_layer(layer ? ASurfaceControl_fromJava(env, layer) : nullptr);
}

extern "C" JNIEXPORT void JNICALL Java_dev_hyperwin_app_GpuService_nativeSetPointerLayer(JNIEnv* env, jobject,
                                                                                        jobject layer) {
    hwgpu_host_set_pointer_layer(layer ? ASurfaceControl_fromJava(env, layer) : nullptr);
}
