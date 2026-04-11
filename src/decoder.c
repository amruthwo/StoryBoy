#include "decoder.h"
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>

/* -------------------------------------------------------------------------
 * Duration formatter
 * ---------------------------------------------------------------------- */

void format_duration(double sec, char *buf, int buf_size) {
    if (sec < 0) sec = 0;
    long long total = (long long)sec;
    int h = (int)(total / 3600);
    int m = (int)((total % 3600) / 60);
    int s = (int)(total % 60);
    if (h > 0)
        snprintf(buf, (size_t)buf_size, "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, (size_t)buf_size, "%d:%02d", m, s);
}

/* -------------------------------------------------------------------------
 * Probe
 * ---------------------------------------------------------------------- */

int decoder_probe(const char *path, ProbeInfo *info,
                  char *errbuf, int errbuf_size) {
    memset(info, 0, sizeof(*info));
    info->audio_stream_idx = -1;

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_size);
        return -1;
    }

    /* Cap probe I/O — default 5 MB / 5 s is far too slow on SD card. */
    fmt->probesize           = 1024 * 1024;  /* 1 MB */
    fmt->max_analyze_duration = 500000;      /* 0.5 s (µs) */

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_size);
        avformat_close_input(&fmt);
        return -1;
    }

    if (fmt->duration != AV_NOPTS_VALUE)
        info->duration_sec = (double)fmt->duration / AV_TIME_BASE;

    int best_audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (best_audio >= 0) {
        AVStream          *as  = fmt->streams[best_audio];
        AVCodecParameters *ap  = as->codecpar;
        info->audio_stream_idx = best_audio;
        info->sample_rate      = ap->sample_rate;
        info->channels         = ap->ch_layout.nb_channels;
        const AVCodecDescriptor *d = avcodec_descriptor_get(ap->codec_id);
        snprintf(info->audio_codec, sizeof(info->audio_codec),
                 "%s", d ? d->name : "unknown");
    }

    for (unsigned int i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            info->audio_track_count++;

    info->num_chapters = (int)fmt->nb_chapters;

    avformat_close_input(&fmt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Packet queue
 * ---------------------------------------------------------------------- */

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(*q));
    q->mutex     = SDL_CreateMutex();
    q->not_empty = SDL_CreateCond();
    q->not_full  = SDL_CreateCond();
}

static int pq_enqueue(PacketQueue *q, AVPacket *pkt, int flush) {
    PacketNode *node = av_malloc(sizeof(PacketNode));
    if (!node) return -1;
    node->next  = NULL;
    node->flush = flush;
    if (pkt) {
        node->pkt = av_packet_alloc();
        if (!node->pkt) { av_free(node); return -1; }
        av_packet_move_ref(node->pkt, pkt);
    } else {
        node->pkt = NULL;
    }

    SDL_LockMutex(q->mutex);
    while (q->count >= PKT_QUEUE_MAX && !q->abort && !q->interrupt)
        SDL_CondWait(q->not_full, q->mutex);
    if (q->abort || q->interrupt) {
        SDL_UnlockMutex(q->mutex);
        if (node->pkt) av_packet_free(&node->pkt);
        av_free(node);
        return -1;
    }
    if (q->tail) q->tail->next = node; else q->head = node;
    q->tail = node;
    q->count++;
    SDL_CondSignal(q->not_empty);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    return pq_enqueue(q, pkt, 0);
}

void packet_queue_put_flush(PacketQueue *q) {
    pq_enqueue(q, NULL, 1);
}

/* Returns: 1 = packet, 0 = seek-flush sentinel, -1 = abort */
int packet_queue_get(PacketQueue *q, AVPacket *out) {
    SDL_LockMutex(q->mutex);
    while (q->count == 0 && !q->abort)
        SDL_CondWait(q->not_empty, q->mutex);
    if (q->abort && q->count == 0) {
        SDL_UnlockMutex(q->mutex);
        return -1;
    }
    PacketNode *node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    SDL_CondSignal(q->not_full);
    SDL_UnlockMutex(q->mutex);

    int is_flush = node->flush;
    if (node->pkt) {
        av_packet_move_ref(out, node->pkt);
        av_packet_free(&node->pkt);
    } else {
        av_packet_unref(out);
    }
    av_free(node);
    return is_flush ? 0 : 1;
}

void packet_queue_flush(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    PacketNode *n = q->head;
    while (n) {
        PacketNode *next = n->next;
        if (n->pkt) av_packet_free(&n->pkt);
        av_free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    SDL_CondBroadcast(q->not_full);
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort = 1;
    SDL_CondBroadcast(q->not_empty);
    SDL_CondBroadcast(q->not_full);
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->not_empty);
    SDL_DestroyCond(q->not_full);
    memset(q, 0, sizeof(*q));
}

/* -------------------------------------------------------------------------
 * Demux thread
 * ---------------------------------------------------------------------- */

static int demux_thread(void *userdata) {
    DemuxCtx *d = (DemuxCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();

    while (!d->abort) {
        /* Handle pending seek request */
        SDL_LockMutex(d->seek_mutex);
        if (d->seek_pending) {
            int64_t ts = (int64_t)(d->seek_target * AV_TIME_BASE);
            av_seek_frame(d->fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
            /* Flush queue, clear interrupt flag, send flush sentinel */
            packet_queue_flush(&d->audio_queue);
            SDL_LockMutex(d->audio_queue.mutex);
            d->audio_queue.interrupt = 0;
            SDL_UnlockMutex(d->audio_queue.mutex);
            packet_queue_put_flush(&d->audio_queue);
            d->seek_pending = 0;
            SDL_CondSignal(d->seek_cond);
        }
        SDL_UnlockMutex(d->seek_mutex);

        int ret = av_read_frame(d->fmt_ctx, pkt);
        if (ret < 0) {
            packet_queue_put(&d->audio_queue, NULL); /* EOS */
            break;
        }

        if (pkt->stream_index == d->audio_stream_idx)
            packet_queue_put(&d->audio_queue, pkt);

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Demux public API
 * ---------------------------------------------------------------------- */

int demux_open(DemuxCtx *d, const char *path, char *errbuf, int errbuf_sz) {
    memset(d, 0, sizeof(*d));
    d->audio_stream_idx = -1;

    int ret = avformat_open_input(&d->fmt_ctx, path, NULL, NULL);
    if (ret < 0) { if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz); return -1; }

    d->fmt_ctx->probesize            = 1024 * 1024;
    d->fmt_ctx->max_analyze_duration = 500000;

    ret = avformat_find_stream_info(d->fmt_ctx, NULL);
    if (ret < 0) { if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
                   avformat_close_input(&d->fmt_ctx); return -1; }

    d->audio_stream_idx = av_find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                               -1, -1, NULL, 0);

    /* Collect all audio stream indices */
    for (unsigned int i = 0; i < d->fmt_ctx->nb_streams; i++) {
        if (d->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            d->audio_stream_count < 8)
            d->audio_stream_indices[d->audio_stream_count++] = (int)i;
    }

    packet_queue_init(&d->audio_queue);
    d->seek_mutex = SDL_CreateMutex();
    d->seek_cond  = SDL_CreateCond();
    return 0;
}

void demux_start(DemuxCtx *d) {
    d->thread = SDL_CreateThread(demux_thread, "demux", d);
}

void demux_stop(DemuxCtx *d) {
    d->abort = 1;
    SDL_LockMutex(d->seek_mutex);
    SDL_CondSignal(d->seek_cond);
    SDL_UnlockMutex(d->seek_mutex);
    packet_queue_abort(&d->audio_queue);
    if (d->thread) { SDL_WaitThread(d->thread, NULL); d->thread = NULL; }
}

void demux_close(DemuxCtx *d) {
    packet_queue_destroy(&d->audio_queue);
    if (d->fmt_ctx) avformat_close_input(&d->fmt_ctx);
    if (d->seek_mutex) SDL_DestroyMutex(d->seek_mutex);
    if (d->seek_cond)  SDL_DestroyCond(d->seek_cond);
    memset(d, 0, sizeof(*d));
}

int demux_cycle_audio(DemuxCtx *d) {
    if (d->audio_stream_count <= 1) return d->audio_stream_idx;

    int pos = 0;
    for (int i = 0; i < d->audio_stream_count; i++) {
        if (d->audio_stream_indices[i] == d->audio_stream_idx) { pos = i; break; }
    }
    pos = (pos + 1) % d->audio_stream_count;
    d->audio_stream_idx = d->audio_stream_indices[pos];
    packet_queue_flush(&d->audio_queue);
    return d->audio_stream_idx;
}

void demux_request_seek(DemuxCtx *d, double target_sec) {
    SDL_LockMutex(d->seek_mutex);
    d->seek_target  = target_sec;
    d->seek_pending = 1;
    SDL_UnlockMutex(d->seek_mutex);
    /* Wake demux thread if blocked in pq_enqueue */
    SDL_LockMutex(d->audio_queue.mutex);
    d->audio_queue.interrupt = 1;
    SDL_CondBroadcast(d->audio_queue.not_full);
    SDL_UnlockMutex(d->audio_queue.mutex);
}

/* -------------------------------------------------------------------------
 * Chapter helpers
 * ---------------------------------------------------------------------- */

int demux_chapter_count(const DemuxCtx *d) {
    if (!d->fmt_ctx) return 0;
    return (int)d->fmt_ctx->nb_chapters;
}

double demux_chapter_start(const DemuxCtx *d, int idx) {
    if (!d->fmt_ctx || idx < 0 || idx >= (int)d->fmt_ctx->nb_chapters) return 0.0;
    AVChapter *ch = d->fmt_ctx->chapters[idx];
    return (double)ch->start * av_q2d(ch->time_base);
}

double demux_chapter_end(const DemuxCtx *d, int idx) {
    if (!d->fmt_ctx || idx < 0 || idx >= (int)d->fmt_ctx->nb_chapters) return 0.0;
    AVChapter *ch = d->fmt_ctx->chapters[idx];
    return (double)ch->end * av_q2d(ch->time_base);
}

int demux_chapter_at(const DemuxCtx *d, double pos_sec) {
    if (!d->fmt_ctx || d->fmt_ctx->nb_chapters == 0) return -1;
    for (int i = 0; i < (int)d->fmt_ctx->nb_chapters; i++) {
        double start = demux_chapter_start(d, i);
        double end   = demux_chapter_end(d, i);
        if (pos_sec >= start && pos_sec < end) return i;
    }
    /* Past last chapter end — return last chapter */
    return (int)d->fmt_ctx->nb_chapters - 1;
}
