#pragma once
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "decoder.h"

/* -------------------------------------------------------------------------
 * Output format (fixed — swresample converts to this)
 * ---------------------------------------------------------------------- */

#define AUDIO_OUT_RATE     44100        /* Requested rate; a->out_rate captures actual negotiated
                                          rate via SDL_AUDIO_ALLOW_FREQUENCY_CHANGE. 44100 avoids
                                          dmix rate conflicts on Flip/Mini family (PyUI locks dmix
                                          at 44100Hz; requesting 48000Hz causes EINVAL). */
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_SDL_FORMAT   AUDIO_S16SYS  /* signed 16-bit, native endian */
#define AUDIO_SDL_SAMPLES  4096          /* callback buffer size in frames */

/* -------------------------------------------------------------------------
 * Ring buffer — lock-free reads from SDL callback, locked writes from decoder
 * ---------------------------------------------------------------------- */

#ifdef SB_A30
#define AUDIO_RING_SIZE (1024 * 128)  /* 128 KB — conserve RAM on Mini/A30 (103MB, no swap) */
#else
#define AUDIO_RING_SIZE (1024 * 512)  /* 512 KB ≈ 2.7s at 48kHz stereo S16 */
#endif

typedef struct {
    uint8_t   *buf;
    int        capacity;
    int        filled;      /* bytes ready to read             */
    int        read_pos;
    int        write_pos;
    SDL_mutex *mutex;
    SDL_cond  *not_full;    /* signaled when space freed       */
} AudioRingBuf;

/* -------------------------------------------------------------------------
 * Audio context
 * ---------------------------------------------------------------------- */

typedef struct {
    AVCodecContext    *codec_ctx;
    struct SwrContext *swr;
    AudioRingBuf       ring;
    SDL_AudioDeviceID  dev;
    SDL_Thread        *thread;
    PacketQueue       *pkt_queue;   /* borrowed pointer — not owned */
    int                abort;
    int                eos;         /* set to 1 when decode thread reaches EOS */
    _Atomic float      volume;      /* 0.0 = mute, 1.0 = full; written from main thread,
                                       read from SDL audio callback — atomic for safety */
    _Atomic int        clock_seek_active; /* 1 while a seek is in flight after an audio track
                                             switch: decode thread discards frames (no ring
                                             writes, no clock updates) until the flush sentinel
                                             arrives, keeping the clock pinned at the seek
                                             target so video sync is never disturbed */
    _Atomic Uint32     wake_silence_until; /* SDL_GetTicks() deadline: callback outputs silence
                                              without consuming the ring until this expires, so
                                              the ALSA post-wake underrun storm drains against
                                              empty output rather than real audio data */
    double             clock;       /* playback position in seconds (relative to file start) */
    double             start_sec;   /* audio stream start_time converted to seconds; subtracted
                                       from raw PTS so clock is always file-relative (0 = BOF) */
    SDL_mutex         *clock_mutex;
    int                out_rate;    /* actual SDL device sample rate (may differ from
                                       AUDIO_OUT_RATE if device only supports 44100Hz) */

    /* Playback speed via atempo filter — 1.0 = passthrough (no filter graph).
       Written by main thread (audio_set_speed), read by decode thread. */
    _Atomic float      speed;       /* current target speed */
    AVFilterGraph     *filter_graph;
    AVFilterContext   *buffersrc_ctx;
    AVFilterContext   *buffersink_ctx;
    float              filter_speed; /* speed the current graph was built for */
} AudioCtx;

/* Open audio codec and SDL audio device.
   codec_params: from the audio stream's codecpar.
   pkt_queue: the demux context's audio_queue (borrowed). */
int  audio_open (AudioCtx *a, AVCodecParameters *codec_params,
                 PacketQueue *pkt_queue, char *errbuf, int errbuf_sz);
void audio_start(AudioCtx *a);
void audio_pause(AudioCtx *a, int pause); /* 1=pause, 0=resume */
void audio_stop (AudioCtx *a);            /* signals abort, joins thread */
void audio_close(AudioCtx *a);            /* frees all resources */

double audio_get_clock(const AudioCtx *a);

/* Set playback speed (1.0 = normal, 1.25/1.5/2.0 = fast, pitch-preserved).
   Takes effect on the next decoded frame. */
void audio_set_speed(AudioCtx *a, float speed);

/* Reinit SDL audio device after A30 sleep/wake.  Call from main thread only.
   Clears the ring buffer, reopens the device, and restores the DAC volume. */
void audio_wake(AudioCtx *a);

/* Switch to a new audio stream mid-playback.
   Stops the decode thread, reinitialises codec+swr, restarts.
   The SDL device stays open to avoid a gap.
   pkt_queue is the demux audio queue — used to wake the decode thread. */
int audio_switch_stream(AudioCtx *a, PacketQueue *pkt_queue,
                        AVCodecParameters *cp, AVRational time_base,
                        char *errbuf, int errbuf_sz);
