#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>

/* -------------------------------------------------------------------------
 * File probe info
 * ---------------------------------------------------------------------- */

typedef struct {
    double duration_sec;
    int    audio_stream_idx;
    int    sample_rate;
    int    channels;
    char   audio_codec[64];
    int    audio_track_count;
    int    num_chapters;        /* number of chapters embedded in the file */
} ProbeInfo;

int  decoder_probe(const char *path, ProbeInfo *info,
                   char *errbuf, int errbuf_size);
void format_duration(double sec, char *buf, int buf_size);

/* -------------------------------------------------------------------------
 * Packet queue
 * ---------------------------------------------------------------------- */

#ifdef SB_A30
#define PKT_QUEUE_MAX 16  /* conserve RAM on Mini/A30 (103MB, no swap) */
#else
#define PKT_QUEUE_MAX 64
#endif

typedef struct PacketNode {
    AVPacket        *pkt;   /* NULL = EOS or flush sentinel (check .flush) */
    int              flush; /* 1 = seek flush (codec flush + continue),
                               0 = EOS (exit decode thread)                 */
    int64_t          flush_pts; /* exact seek target in stream timebase units;
                                   AV_NOPTS_VALUE if unknown.  Carried to the
                                   audio decode thread so it can fast-discard
                                   frames that precede the true seek target
                                   (chunk-level index entries span ~2 min, so
                                   av_seek_frame lands on the chunk start, not
                                   the exact position). */
    struct PacketNode *next;
} PacketNode;

typedef struct {
    PacketNode  *head, *tail;
    int          count;
    int          abort;
    int          interrupt; /* set by demux_request_seek: causes pq_enqueue to
                               return -1 immediately so the demux thread exits
                               any blocked put and reaches the seek check fast */
    _Atomic int  flush_gen; /* incremented each time a flush sentinel is enqueued;
                               the audio decode thread checks this after each frame
                               and breaks the inner decode loop when it changes,
                               allowing seeks to be processed within ~185ms even
                               while the audio thread is blocked in ring_write */
    SDL_mutex   *mutex;
    SDL_cond    *not_empty;
    SDL_cond    *not_full;
} PacketQueue;

void packet_queue_init     (PacketQueue *q);
int  packet_queue_put      (PacketQueue *q, AVPacket *pkt);
/* Returns: 1 = packet, 0 = flush sentinel, -1 = abort */
int  packet_queue_get      (PacketQueue *q, AVPacket *out);
/* flush_pts: exact seek target in stream timebase (AV_NOPTS_VALUE if unknown) */
void packet_queue_put_flush(PacketQueue *q, int64_t flush_pts);
void packet_queue_flush    (PacketQueue *q);
void packet_queue_abort    (PacketQueue *q);
void packet_queue_destroy  (PacketQueue *q);

/* -------------------------------------------------------------------------
 * Demux context
 * ---------------------------------------------------------------------- */

typedef struct {
    AVFormatContext *fmt_ctx;
    int              audio_stream_idx;
    PacketQueue      audio_queue;
    SDL_Thread      *thread;
    int              abort;

    /* Seek request — set from main thread, handled by demux thread */
    SDL_mutex       *seek_mutex;
    SDL_cond        *seek_cond;   /* signaled when seek completes */
    volatile int     seek_pending;
    double           seek_target; /* seconds */

    /* All audio stream indices found in the file */
    int              audio_stream_indices[8];
    int              audio_stream_count;
} DemuxCtx;

int  demux_open        (DemuxCtx *d, const char *path, char *errbuf, int sz);
void demux_start       (DemuxCtx *d);
void demux_stop        (DemuxCtx *d);
void demux_close       (DemuxCtx *d);
void demux_request_seek(DemuxCtx *d, double target_sec);

/* Advance to the next audio stream. Flushes queued audio packets.
   Returns the new audio_stream_idx. No-op if only one audio stream. */
int  demux_cycle_audio (DemuxCtx *d);

/* Chapter helpers — operate on fmt_ctx->chapters[].
   Returns 0 if no chapters or index out of range. */
int    demux_chapter_count(const DemuxCtx *d);
double demux_chapter_start(const DemuxCtx *d, int idx); /* seconds */
double demux_chapter_end  (const DemuxCtx *d, int idx); /* seconds */

/* Find which chapter index contains pos_sec. Returns -1 if no chapters. */
int    demux_chapter_at   (const DemuxCtx *d, double pos_sec);
