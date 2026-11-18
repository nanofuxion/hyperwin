// Checks Windows' default speakers or microphone through WASAPI shared mode (audio/README.md).
//   audtest.exe play [seconds]     a 440 Hz tone; reports the period, latency and the clock's rate
//   audtest.exe record [seconds]   reports frames captured, the clock's rate and the loudest sample
// Writes the result to stdout and to audtest.txt beside the program.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* out_file;

static void report(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (out_file) {
        va_start(a, fmt);
        vfprintf(out_file, fmt, a);
        va_end(a);
        fflush(out_file);
    }
}

#define CHECK(x)                                                         \
    do {                                                                 \
        HRESULT hr_ = (x);                                               \
        if (FAILED(hr_)) {                                               \
            report("FAIL %s: 0x%08lx\n", #x, (unsigned long)hr_);        \
            return 1;                                                    \
        }                                                                \
    } while (0)

static double now_s() {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "audtest.txt");
    out_file = fopen(path, "a");
    bool capture = argc > 1 && !strcmp(argv[1], "record");
    double seconds = argc > 2 ? atof(argv[2]) : 5;

    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    IMMDeviceEnumerator* en = NULL;
    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en));
    IMMDevice* dev = NULL;
    CHECK(en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev));
    IAudioClient* ac = NULL;
    CHECK(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ac));
    WAVEFORMATEX* mix = NULL;
    CHECK(ac->GetMixFormat(&mix));
    REFERENCE_TIME def = 0, min = 0;
    CHECK(ac->GetDevicePeriod(&def, &min));
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL));
    CHECK(ac->SetEventHandle(ev));
    UINT32 frames = 0;
    CHECK(ac->GetBufferSize(&frames));
    REFERENCE_TIME lat = 0;
    CHECK(ac->GetStreamLatency(&lat));
    report("%s: mix %lu Hz %u ch %u bit, period %.1f ms (min %.1f), buffer %u frames, latency %.1f ms\n",
           capture ? "record" : "play", mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, def / 1e4,
           min / 1e4, frames, lat / 1e4);
    IAudioClock* clock = NULL;
    CHECK(ac->GetService(__uuidof(IAudioClock), (void**)&clock));
    UINT64 clock_freq = 0;
    clock->GetFrequency(&clock_freq);
    IAudioRenderClient* rc = NULL;
    IAudioCaptureClient* cc = NULL;
    if (capture) CHECK(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc));
    else CHECK(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc));

    unsigned ch = mix->nChannels, rate = mix->nSamplesPerSec;
    double phase = 0;
    UINT64 moved = 0, glitches = 0;
    float peak = 0;
    if (rc) {   // prefill with silence
        BYTE* d;
        CHECK(rc->GetBuffer(frames, &d));
        CHECK(rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT));
    }
    CHECK(ac->Start());
    double t0 = now_s(), t_first = 0;
    UINT64 p_first = 0;
    while (now_s() - t0 < seconds) {
        if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) {
            report("FAIL no event for 2 s after %llu frames\n", moved);
            break;
        }
        if (rc) {
            UINT32 pad = 0;
            CHECK(ac->GetCurrentPadding(&pad));
            UINT32 n = frames - pad;
            BYTE* d;
            if (!n) continue;
            CHECK(rc->GetBuffer(n, &d));
            float* f = (float*)d;   // the mix format is float
            for (UINT32 i = 0; i < n; i++) {
                float v = (float)(0.2 * sin(phase));
                phase += 2 * 3.14159265358979 * 440 / rate;
                for (unsigned c = 0; c < ch; c++) f[i * ch + c] = v;
            }
            CHECK(rc->ReleaseBuffer(n, 0));
            moved += n;
        } else {
            for (;;) {
                BYTE* d;
                UINT32 n = 0;
                DWORD flags = 0;
                if (cc->GetBuffer(&d, &n, &flags, NULL, NULL) != S_OK) break;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) glitches++;
                const float* f = (const float*)d;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    for (UINT32 i = 0; i < n * ch; i++) peak = fabsf(f[i]) > peak ? fabsf(f[i]) : peak;
                cc->ReleaseBuffer(n);
                moved += n;
            }
        }
        UINT64 pos = 0, qpc = 0;
        clock->GetPosition(&pos, &qpc);
        if (!t_first && pos) t_first = now_s(), p_first = pos;
    }
    UINT64 pos = 0, qpc = 0;
    clock->GetPosition(&pos, &qpc);
    double t1 = now_s();
    double rate_seen = t_first && t1 > t_first ? (double)(pos - p_first) / clock_freq * rate / (t1 - t_first) : 0;
    ac->Stop();
    if (capture)
        report("record: %llu frames in %.2f s, clock %.1f Hz, peak %.3f, %llu discontinuities\n", moved, t1 - t0,
               rate_seen, peak, glitches);
    else
        report("play: %llu frames in %.2f s, clock %.1f Hz, started after %.0f ms\n", moved, t1 - t0, rate_seen,
               (t_first - t0) * 1000);
    return 0;
}
