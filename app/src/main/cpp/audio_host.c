/* Windows' speakers and microphone on the phone (audio/README.md, include/hw_audio.h).
 *
 * hwaudio.sys gives Windows' audio engine buffers inside the shared window. Here an AAudio output
 * stream plays the speaker buffer straight from there, and an AAudio input stream records into the
 * mic buffer. The callbacks advance the frame cursors in the control page, and the driver reports
 * those cursors to Windows as the stream position: the phone's audio clock sets the pace.
 *
 * A watcher thread opens a stream when Windows starts one and closes it again once Windows has
 * stopped for a while, and reopens streams that a route change (headphones, Bluetooth) ended. */
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hw_abi.h"
#include "hw_audio.h"

#define TAG "HyperWinAudio"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define IDLE_CLOSE_NS 3000000000ll   /* close a stream Windows has not run for this long */

static uint8_t *region;
static volatile int running;
static pthread_t watcher;
static JavaVM *jvm;
static jclass audio_class;   /* dev.hyperwin.app.GuestAudio, for asking for the mic */

static AAudioStream *out, *in;
static volatile int out_lost, in_lost;   /* set by the error callback: reopen */
static volatile int mic_allowed;
static uint64_t pumped;      /* silence frames given while the mic is refused */
static float out_peak;       /* loudest sample played since the last log line */

static struct hwa_ctl *ctl(void) { return (struct hwa_ctl *)region; }
static uint8_t *buf(int capture) { return region + (capture ? HWA_CAPTURE_OFF : HWA_RENDER_OFF) - HWA_OFF; }

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

/* A new epoch means Windows started the stream again from position 0: 1 the first time it is seen. */
static int new_epoch(struct hwa_stream *h) {
    uint32_t epoch = __atomic_load_n(&h->epoch, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&h->seen_epoch, __ATOMIC_RELAXED) == epoch) return 0;
    __atomic_store_n(&h->frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&h->seen_epoch, epoch, __ATOMIC_RELEASE);
    LOG("%s epoch %u: format %u, %u bytes (%u ms), %u periods", h == &ctl()->render ? "speaker" : "mic", epoch,
        h->format, h->bytes, h->bytes / HWA_FRAME_BYTES(h->format) * 1000 / HWA_RATE, h->periods);
    return 1;
}

static void advance(struct hwa_stream *h, uint64_t frames) {
    __atomic_store_n(&h->frames, frames, __ATOMIC_RELEASE);
    __atomic_store_n(&h->time, now_ns(), __ATOMIC_RELAXED);
}

static int marked(const uint8_t *frame, uint32_t fmt) {
    const uint32_t *w = (const uint32_t *)frame;
    return fmt == HWA_FMT_F32 ? w[0] == HWA_MARK_F32 && w[1] == HWA_MARK_F32 : w[0] == HWA_MARK_S16;
}

static void mark(uint8_t *frame, uint32_t fmt) {
    uint32_t *w = (uint32_t *)frame;
    if (fmt == HWA_FMT_F32) w[0] = w[1] = HWA_MARK_F32;
    else w[0] = HWA_MARK_S16;
}

static uint32_t queued_min = UINT32_MAX, queued_max, calls, gap, gap_max, short_calls;

/* The speaker cushion. Windows' audio engine writes a set amount ahead of the position it is told,
 * and its thread sometimes runs late, more so when the VM is busy. So the phone reports a position
 * `lead` frames past what it has really played, and Windows writes that much further ahead. The
 * lead grows after an underrun and shrinks slowly while none come. */
#define LEAD_MIN (HWA_RATE / 100)          /* 10 ms */
#define LEAD_MAX (HWA_RATE * 30 / 1000)    /* 30 ms, and at most a quarter of the buffer */
static uint64_t played;      /* frames really played in this epoch */
static int64_t priming;      /* when this epoch began, until Windows has queued enough to start on */
static uint32_t lead = LEAD_MIN;
static int64_t lead_changed;

/* Speaker: play what Windows mixed at the cursor, mark it played, and move the cursor on. */
static aaudio_data_callback_result_t play(AAudioStream *s, void *user, void *data, int32_t n) {
    (void)s, (void)user;
    float *o = data;
    struct hwa_stream *h = &ctl()->render;
    if (new_epoch(h)) played = 0, priming = now_ns();
    uint32_t fmt = h->format, bytes = h->bytes, fb = HWA_FRAME_BYTES(fmt);
    if (__atomic_load_n(&h->state, __ATOMIC_ACQUIRE) != HWA_RUN || bytes < fb || bytes > HWA_BUF_BYTES) {
        memset(o, 0, (size_t)n * 2 * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint32_t frames = bytes / fb, pos = (uint32_t)(played % frames);
    uint8_t *b = buf(0);
    /* A new run starts once Windows has queued LEAD_MIN, or after 100 ms whatever it has. */
    if (priming) {
        uint32_t q = 0;
        while (q < LEAD_MIN && !marked(b + (size_t)((pos + q) % frames) * fb, fmt)) q++;
        if (q < LEAD_MIN && now_ns() - priming < 100000000ll) {
            memset(o, 0, (size_t)n * 2 * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        priming = 0;
    }
    int short_of = 0;
    for (int32_t i = 0; i < n; i++, o += 2) {
        uint8_t *f = b + (size_t)pos * fb;
        if (marked(f, fmt)) {
            o[0] = o[1] = 0;
            short_of = 1;
            if (++gap > gap_max) gap_max = gap;
        } else {
            gap = 0;
            if (fmt == HWA_FMT_F32) {
                memcpy(o, f, 8);
            } else {
                o[0] = ((const int16_t *)f)[0] * (1.0f / 32768.0f);
                o[1] = ((const int16_t *)f)[1] * (1.0f / 32768.0f);
            }
        }
        float m = fabsf(o[0]) > fabsf(o[1]) ? fabsf(o[0]) : fabsf(o[1]);
        if (m > out_peak) out_peak = m;
        mark(f, fmt);
        if (++pos == frames) pos = 0;
    }
    played += (uint64_t)n;
    /* Now and then, how much Windows has queued ahead of what is played. */
    if (++calls % 64 == 0) {
        uint32_t q = 0;
        while (q < frames && !marked(b + (size_t)((pos + q) % frames) * fb, fmt)) q++;
        if (q < queued_min) queued_min = q;
        if (q > queued_max) queued_max = q;
    }
    int64_t now = now_ns();
    uint32_t most = frames / 4 < LEAD_MAX ? frames / 4 : LEAD_MAX;
    if (short_of) {
        h->xruns++;
        short_calls++;
        if (lead < most && now - lead_changed > 100000000ll) lead += HWA_RATE / 200, lead_changed = now;
    } else if (lead > LEAD_MIN && now - lead_changed > 10000000000ll) {
        lead -= (uint32_t)n / 2 < HWA_RATE / 1000 ? (uint32_t)n / 2 : HWA_RATE / 1000;   /* never backwards */
        lead_changed = now;
    }
    if (lead > most) lead = most;
    advance(h, played + (played < lead ? played : lead));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* The microphone delivers 20 ms at a time (its echo cancelling works in such blocks), and Windows'
 * audio engine counts a cursor that stands still and then jumps as a glitch. So the callback writes
 * each block straight into the buffer, and the watcher moves the cursor Windows sees on over it
 * smoothly, a little faster than real time, never past what is written. */
static uint64_t mic_written;   /* frames written in mic_epoch: callback (or pump) stores, watcher loads */
static uint32_t mic_epoch;
static uint64_t mic_shown;     /* watcher: the cursor Windows has */
static int64_t mic_shown_at;

/* Microphone: write n frames of mono float (or silence when src is NULL) after the last ones. */
static void record(const float *src, int32_t n) {
    struct hwa_stream *h = &ctl()->capture;
    uint32_t epoch = __atomic_load_n(&h->epoch, __ATOMIC_ACQUIRE);
    if (epoch != __atomic_load_n(&mic_epoch, __ATOMIC_RELAXED)) {
        __atomic_store_n(&mic_written, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&mic_epoch, epoch, __ATOMIC_RELEASE);
    }
    uint64_t at = __atomic_load_n(&mic_written, __ATOMIC_RELAXED);
    uint32_t fmt = h->format, bytes = h->bytes, fb = HWA_FRAME_BYTES(fmt);
    if (__atomic_load_n(&h->state, __ATOMIC_ACQUIRE) != HWA_RUN || bytes < fb || bytes > HWA_BUF_BYTES) return;
    uint32_t frames = bytes / fb, pos = (uint32_t)(at % frames);
    uint8_t *b = buf(1);
    for (int32_t i = 0; i < n; i++) {
        float v = src ? src[i] : 0.0f;
        if (fmt == HWA_FMT_F32) {
            float *p = (float *)(b + (size_t)pos * 8);
            p[0] = p[1] = v;
        } else {
            float c = v * 32767.0f;
            int16_t x = (int16_t)(c > 32767.0f ? 32767 : c < -32768.0f ? -32768 : c);
            int16_t *p = (int16_t *)(b + (size_t)pos * 4);
            p[0] = p[1] = x;
        }
        if (++pos == frames) pos = 0;
    }
    __atomic_store_n(&mic_written, at + (uint64_t)n, __ATOMIC_RELEASE);
}

/* Watcher: move the cursor Windows sees towards what is written. */
static void show_mic(int64_t now) {
    struct hwa_stream *h = &ctl()->capture;
    uint32_t epoch = __atomic_load_n(&mic_epoch, __ATOMIC_ACQUIRE);
    uint64_t written = __atomic_load_n(&mic_written, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&h->seen_epoch, __ATOMIC_RELAXED) != epoch) {   /* a run the callback has begun */
        __atomic_store_n(&h->frames, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&h->seen_epoch, epoch, __ATOMIC_RELEASE);
        mic_shown = 0;
        mic_shown_at = now;
        LOG("mic epoch %u: format %u, %u bytes", epoch, h->format, h->bytes);
    }
    uint64_t step = (uint64_t)((now - mic_shown_at) * (HWA_RATE + HWA_RATE / 16) / 1000000000ll);
    mic_shown_at = now;
    if (written < mic_shown) return;
    mic_shown = written - mic_shown < step ? written : mic_shown + step;
    advance(h, mic_shown);
}

static aaudio_data_callback_result_t listen(AAudioStream *s, void *user, void *data, int32_t n) {
    (void)s, (void)user;
    record(data, n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void on_error(AAudioStream *s, void *user, aaudio_result_t err) {
    (void)user;
    LOG("%s stream ended: %s", s == out ? "speaker" : "mic", AAudio_convertResultToText(err));
    if (s == out) out_lost = 1;
    else in_lost = 1;
}

static AAudioStream *open_stream(int capture) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setDirection(b, capture ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, HWA_RATE);
    AAudioStreamBuilder_setChannelCount(b, capture ? 1 : HWA_CHANNELS);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (capture) {
        /* Android's echo cancelling and noise suppression: the phone's own speaker plays the game. */
        AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    } else {
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);   /* falls back to shared */
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    }
    AAudioStreamBuilder_setDataCallback(b, capture ? listen : play, NULL);
    AAudioStreamBuilder_setErrorCallback(b, on_error, NULL);
    AAudioStream *s = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOG("%s open failed: %s", capture ? "mic" : "speaker", AAudio_convertResultToText(r));
        return NULL;
    }
    int32_t burst = AAudioStream_getFramesPerBurst(s);
    if (!capture) AAudioStream_setBufferSizeInFrames(s, 2 * burst);
    LOG("%s open: %s, %d Hz, burst %d, buffer %d of %d frames", capture ? "mic" : "speaker",
        AAudioStream_getSharingMode(s) == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared",
        AAudioStream_getSampleRate(s), burst, AAudioStream_getBufferSizeInFrames(s),
        AAudioStream_getBufferCapacityInFrames(s));
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        return NULL;
    }
    return s;
}

static void close_stream(AAudioStream **s) {
    if (!*s) return;
    AAudioStream *x = *s;
    *s = NULL;
    AAudioStream_requestStop(x);
    AAudioStream_close(x);
}

/* The output latency AAudio reports: frames written but not yet out of the speaker. */
static void log_latency(void) {
    int64_t pos, t;
    if (!out || queued_min == UINT32_MAX || AAudioStream_getTimestamp(out, CLOCK_MONOTONIC, &pos, &t) != AAUDIO_OK)
        return;
    int64_t written = AAudioStream_getFramesWritten(out);
    int64_t ahead = written - pos - (now_ns() - t) * HWA_RATE / 1000000000ll;
    struct hwa_stream *h = &ctl()->render;
    LOG("speaker: Windows queued %u to %u ms with a %u ms lead; %u short callbacks, longest gap %u ms; "
        "AAudio holds %lld ms, %d xruns; peak %.3f", queued_min * 1000 / HWA_RATE, queued_max * 1000 / HWA_RATE,
        lead * 1000 / HWA_RATE, short_calls, gap_max * 1000 / HWA_RATE, (long long)(ahead * 1000 / HWA_RATE),
        AAudioStream_getXRunCount(out), out_peak);
    (void)h;
    out_peak = 0;
    short_calls = gap_max = 0;
    queued_min = UINT32_MAX;
    queued_max = 0;
}

static int attached;

static void ask_mic(void) {
    JNIEnv *env;
    if (!jvm || (*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) return;
    attached = 1;
    jmethodID m = (*env)->GetStaticMethodID(env, audio_class, "onMicWanted", "()V");
    if (m) (*env)->CallStaticVoidMethod(env, audio_class, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void *watch(void *arg) {
    (void)arg;
    int64_t out_idle = 0, in_idle = 0, pump_at = 0, last_log = 0, out_try = 0, in_try = 0;
    int asked = 0;
    while (running) {
        struct hwa_ctl *c = ctl();
        if (__atomic_load_n(&c->magic, __ATOMIC_ACQUIRE) != HWA_MAGIC) {
            close_stream(&out);
            close_stream(&in);
            usleep(20000);
            continue;
        }
        int64_t now = now_ns();
        int play_on = __atomic_load_n(&c->render.state, __ATOMIC_ACQUIRE) == HWA_RUN;
        int rec_on = __atomic_load_n(&c->capture.state, __ATOMIC_ACQUIRE) == HWA_RUN;

        if (out_lost) close_stream(&out), out_lost = 0;
        if (play_on) out_idle = now;
        if (play_on && !out && now - out_try > 1000000000ll) out_try = now, out = open_stream(0);
        if (!play_on && out && now - out_idle > IDLE_CLOSE_NS) close_stream(&out), LOG("speaker closed");

        c->capture.allowed = (uint32_t)mic_allowed;
        if (in_lost) close_stream(&in), in_lost = 0;
        if (rec_on) in_idle = now;
        if (rec_on && !mic_allowed && !asked) asked = 1, ask_mic();
        if (rec_on && mic_allowed && !in && now - in_try > 1000000000ll) in_try = now, in = open_stream(1);
        if ((!rec_on || !mic_allowed) && in && now - in_idle > (mic_allowed ? IDLE_CLOSE_NS : 0))
            close_stream(&in), LOG("mic closed");
        if (in) c->capture.xruns = (uint64_t)AAudioStream_getXRunCount(in);
        if (rec_on) show_mic(now);
        /* With the mic refused, Windows still gets a running microphone: silence, on time. */
        if (rec_on && !in) {
            if (!pump_at) pump_at = now, pumped = 0;
            uint64_t due = (uint64_t)((now - pump_at) * HWA_RATE / 1000000000ll);
            if (due > pumped) record(NULL, (int32_t)(due - pumped)), pumped = due;
        } else {
            pump_at = 0;
        }
        if (out && now - last_log > 10000000000ll) last_log = now, log_latency();
        usleep(out || in || rec_on ? 2000 : 10000);
    }
    close_stream(&out);
    close_stream(&in);
    if (attached) (*jvm)->DetachCurrentThread(jvm), attached = 0;
    return NULL;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_GuestAudio_nativeStart(JNIEnv *env, jclass cls, jobject buffer, jboolean mic)
{
    uint8_t *window = (*env)->GetDirectBufferAddress(env, buffer);
    if (running || !window || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)HW_WINDOW_SIZE) return;
    (*env)->GetJavaVM(env, &jvm);
    if (!audio_class) audio_class = (*env)->NewGlobalRef(env, cls);
    region = window + HWA_OFF;
    mic_allowed = mic;
    running = 1;
    pthread_create(&watcher, NULL, watch, NULL);
}

/* Before the window goes away. */
JNIEXPORT void JNICALL
Java_dev_hyperwin_app_GuestAudio_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env, (void)cls;
    if (!running) return;
    running = 0;
    pthread_join(watcher, NULL);
    region = NULL;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_GuestAudio_nativeSetMic(JNIEnv *env, jclass cls, jboolean allowed)
{
    (void)env, (void)cls;
    mic_allowed = allowed;
}
