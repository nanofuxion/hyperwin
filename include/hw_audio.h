/* Windows sound and microphone through the shared window (audio/README.md).
 *
 * hwaudio.sys, a PortCls WaveRT driver, hands Windows' audio engine buffers that lie here, so the
 * engine mixes straight into shared RAM and reads the microphone straight out of it. The phone's
 * AAudio callbacks play and record them (app/src/main/cpp/audio_host.c), and their cursors are the
 * position the driver reports to Windows: the phone's audio clock sets the pace and nothing drifts.
 *
 * Cursors are free-running 64-bit frame counts; each side takes them modulo the buffer's frames.
 * Stores release, loads acquire. All numbers are little endian.
 */
#ifndef HYPERWIN_HW_AUDIO_H
#define HYPERWIN_HW_AUDIO_H

#include <stdint.h>

/* Inside the window: HW_AUDIO, 1 MiB at 0x800000 (include/hw_abi.h). */
#define HWA_OFF           0x800000u  /* HW_AUDIO */
#define HWA_MAGIC         0x314f494455415748ull /* "HWAUDIO1" little endian */
#define HWA_VERSION       1u
#define HWA_CTL_BYTES     0x1000u
#define HWA_BUF_BYTES     (256u << 10)
#define HWA_RENDER_OFF    (HWA_OFF + HWA_CTL_BYTES)
#define HWA_CAPTURE_OFF   (HWA_RENDER_OFF + HWA_BUF_BYTES)
#define HWA_END           (HWA_CAPTURE_OFF + HWA_BUF_BYTES)

#define HWA_RATE          48000u
#define HWA_CHANNELS      2u

/* hwa_stream.format: the WAVE_FORMAT tag of the samples, interleaved stereo */
#define HWA_FMT_S16       1u   /* WAVE_FORMAT_PCM, 16 bit: 4 bytes a frame */
#define HWA_FMT_F32       3u   /* WAVE_FORMAT_IEEE_FLOAT, 32 bit: 8 bytes a frame */
#define HWA_FRAME_BYTES(fmt) ((fmt) == HWA_FMT_F32 ? 8u : 4u)

/* "Not written yet": the driver fills a new speaker buffer with this, and the phone stamps each
 * frame again once it has played it. A frame still marked when its turn comes means Windows fell
 * behind: the phone plays silence there instead of old sound, and counts an underrun. How far
 * ahead of the cursor the marks begin is how much sound Windows has queued. */
#define HWA_MARK_F32      0x7fa5a5a5u   /* each sample: a NaN, which the audio engine never writes */
#define HWA_MARK_S16      0xa5a55a5au   /* each frame */

/* hwa_stream.state */
#define HWA_STOP          0u
#define HWA_PAUSE         1u
#define HWA_RUN           2u

struct hwa_stream {
    uint32_t state;        /* driver: the phone plays or records only while HWA_RUN */
    uint32_t bytes;        /* driver: the part of the buffer the audio engine uses, whole frames */
    uint32_t epoch;        /* driver: bumped each time the stream starts again from position 0 */
    uint32_t format;       /* driver: HWA_FMT_* */
    uint32_t periods;      /* driver: notifications per buffer the audio engine asked for, 0 for none */
    uint32_t pad;
    uint64_t frames;       /* phone: frames played (render) or recorded (capture) in seen_epoch */
    int64_t time;          /* phone: CLOCK_MONOTONIC ns when frames was current */
    uint64_t xruns;        /* phone: speaker callbacks that found frames Windows had not written */
    uint32_t seen_epoch;   /* phone: the epoch frames counts for, stored after frames */
    uint32_t allowed;      /* phone, capture only: 0 when Android refused the mic (Windows gets silence) */
};

struct hwa_ctl {
    uint64_t magic;        /* driver: HWA_MAGIC once the rest is valid */
    uint32_t version;
    uint32_t rate, channels;
    uint32_t pad;
    struct hwa_stream render, capture;
};

_Static_assert(sizeof(struct hwa_stream) == 56, "hwa_stream layout");
_Static_assert(sizeof(struct hwa_ctl) == 136, "hwa_ctl layout");
_Static_assert(HWA_END <= HWA_OFF + (1024u << 10), "audio fits HW_AUDIO, below HW_FILES");

#endif
