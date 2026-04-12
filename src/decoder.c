#include "decoder.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
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

static int pq_enqueue(PacketQueue *q, AVPacket *pkt, int flush, int64_t flush_pts) {
    PacketNode *node = av_malloc(sizeof(PacketNode));
    if (!node) return -1;
    node->next      = NULL;
    node->flush     = flush;
    node->flush_pts = flush_pts;
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
    return pq_enqueue(q, pkt, 0, AV_NOPTS_VALUE);
}

void packet_queue_put_flush(PacketQueue *q, int64_t flush_pts) {
    if (pq_enqueue(q, NULL, 1, flush_pts) == 0)
        atomic_fetch_add(&q->flush_gen, 1); /* signal audio thread to break inner loop */
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

    int is_flush     = node->flush;
    int64_t fpts     = node->flush_pts;
    if (node->pkt) {
        av_packet_move_ref(out, node->pkt);
        av_packet_free(&node->pkt);
    } else {
        av_packet_unref(out);
        /* Carry flush_pts to the audio thread via out->pts so it knows the
           exact seek target and can fast-discard pre-target frames. */
        if (is_flush) out->pts = fpts;
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
 * MOV index rebuild — for Lavf57 stsz allocation failure
 *
 * When the stsz box is too large to allocate (e.g. 11.9 MB on V2/V3 with
 * ~7 MB free), Lavf57 silently sets sc->sample_count = 0, which leaves the
 * stream with zero index entries and causes av_read_frame to return EOF
 * immediately.  We recover by parsing the small stco / stsc / stts boxes
 * (all under 10 KB for the affected file) and calling av_add_index_entry
 * for each of the 569 chunks.  The AAC bitstream parser handles frame
 * boundaries within each chunk.
 * ---------------------------------------------------------------------- */

/* Scan boxes in [from, from+limit).
 * Returns the body start offset and sets *body_sz_out to the body size.
 * Returns -1 if not found. */
static int64_t box_find(AVIOContext *pb, int64_t from, int64_t limit,
                         uint32_t want_type, int64_t *body_sz_out)
{
    int64_t pos = from;
    int64_t end = from + limit;
    while (pos + 8 <= end) {
        if (avio_seek(pb, pos, SEEK_SET) < 0) return -1;
        uint32_t sz  = avio_rb32(pb);
        uint32_t typ = avio_rb32(pb);
        int64_t  body_start, box_end;
        if (sz == 1) {
            /* 64-bit extended size */
            uint64_t hi = (uint64_t)avio_rb32(pb) << 32;
            uint64_t lo = (uint64_t)avio_rb32(pb);
            uint64_t esz = hi | lo;
            body_start = pos + 16;
            box_end    = pos + (int64_t)esz;
        } else if (sz == 0) {
            body_start = pos + 8;
            box_end    = end; /* extends to container boundary */
        } else {
            body_start = pos + 8;
            box_end    = pos + (int64_t)sz;
        }
        if (box_end <= pos) break; /* corrupt / zero-size */
        if (typ == want_type) {
            if (body_sz_out) *body_sz_out = box_end - body_start;
            return body_start;
        }
        pos = box_end;
    }
    return -1;
}

/* Build a chunk-level index for stream_idx from stco + stsc + stts.
 * Returns number of entries added, or -1 on error. */
static int demux_rebuild_index(AVFormatContext *fmt, int stream_idx)
{
    AVIOContext *pb = fmt->pb;
    if (!pb) return -1;
    AVStream *st = fmt->streams[stream_idx];

    int64_t file_size = avio_size(pb);
    if (file_size <= 0) return -1;

    /* moov */
    int64_t moov_sz = 0;
    int64_t moov_body = box_find(pb, 0, file_size,
                                  MKBETAG('m','o','o','v'), &moov_sz);
    if (moov_body < 0) { fprintf(stderr, "rebuild_index: moov not found\n"); return -1; }

    /* The N-th trak box (0-based), where N = stream_idx */
    int64_t trak_search = moov_body;
    int64_t trak_body = -1, trak_sz = 0;
    for (int i = 0; i <= stream_idx; i++) {
        int64_t bsz = 0;
        int64_t body = box_find(pb, trak_search,
                                moov_body + moov_sz - trak_search,
                                MKBETAG('t','r','a','k'), &bsz);
        if (body < 0) {
            fprintf(stderr, "rebuild_index: trak[%d] not found\n", i);
            return -1;
        }
        if (i == stream_idx) { trak_body = body; trak_sz = bsz; }
        else                  trak_search = body + bsz;
    }

    /* trak → mdia → minf → stbl */
    int64_t bsz = 0;
    int64_t mdia = box_find(pb, trak_body, trak_sz, MKBETAG('m','d','i','a'), &bsz);
    if (mdia < 0) { fprintf(stderr, "rebuild_index: mdia not found\n"); return -1; }
    int64_t mdia_sz = bsz;

    int64_t minf = box_find(pb, mdia, mdia_sz, MKBETAG('m','i','n','f'), &bsz);
    if (minf < 0) { fprintf(stderr, "rebuild_index: minf not found\n"); return -1; }
    int64_t minf_sz = bsz;

    int64_t stbl = box_find(pb, minf, minf_sz, MKBETAG('s','t','b','l'), &bsz);
    if (stbl < 0) { fprintf(stderr, "rebuild_index: stbl not found\n"); return -1; }
    int64_t stbl_sz = bsz;

    /* ---- stco: chunk offsets ---- */
    int64_t stco_body = box_find(pb, stbl, stbl_sz, MKBETAG('s','t','c','o'), &bsz);
    if (stco_body < 0) { fprintf(stderr, "rebuild_index: stco not found\n"); return -1; }
    avio_seek(pb, stco_body, SEEK_SET);
    avio_rb32(pb); /* version+flags */
    uint32_t chunk_count = avio_rb32(pb);
    if (chunk_count == 0 || chunk_count > 1000000) {
        fprintf(stderr, "rebuild_index: bad chunk_count=%u\n", chunk_count);
        return -1;
    }
    int64_t *chunk_offsets = av_malloc(chunk_count * sizeof(int64_t));
    if (!chunk_offsets) return -1;
    for (uint32_t i = 0; i < chunk_count; i++)
        chunk_offsets[i] = (int64_t)avio_rb32(pb);

    /* ---- stsc: sample-to-chunk ---- */
    int64_t stsc_body = box_find(pb, stbl, stbl_sz, MKBETAG('s','t','s','c'), &bsz);
    if (stsc_body < 0) {
        av_free(chunk_offsets);
        fprintf(stderr, "rebuild_index: stsc not found\n"); return -1;
    }
    avio_seek(pb, stsc_body, SEEK_SET);
    avio_rb32(pb); /* version+flags */
    uint32_t stsc_count = avio_rb32(pb);
    typedef struct { uint32_t first_chunk, spc, sdi; } StscEntry;
    StscEntry *stsc_entries = NULL;
    if (stsc_count > 0) {
        stsc_entries = av_malloc(stsc_count * sizeof(StscEntry));
        if (!stsc_entries) { av_free(chunk_offsets); return -1; }
        for (uint32_t i = 0; i < stsc_count; i++) {
            stsc_entries[i].first_chunk = avio_rb32(pb);
            stsc_entries[i].spc         = avio_rb32(pb);
            stsc_entries[i].sdi         = avio_rb32(pb);
        }
    }

    /* ---- stts: time-to-sample ---- */
    int64_t stts_body = box_find(pb, stbl, stbl_sz, MKBETAG('s','t','t','s'), &bsz);
    if (stts_body < 0) {
        av_free(stsc_entries); av_free(chunk_offsets);
        fprintf(stderr, "rebuild_index: stts not found\n"); return -1;
    }
    avio_seek(pb, stts_body, SEEK_SET);
    avio_rb32(pb); /* version+flags */
    uint32_t stts_count = avio_rb32(pb);
    typedef struct { uint32_t count, delta; } SttsEntry;
    SttsEntry *stts_entries = NULL;
    if (stts_count > 0) {
        stts_entries = av_malloc(stts_count * sizeof(SttsEntry));
        if (!stts_entries) { av_free(stsc_entries); av_free(chunk_offsets); return -1; }
        for (uint32_t i = 0; i < stts_count; i++) {
            stts_entries[i].count = avio_rb32(pb);
            stts_entries[i].delta = avio_rb32(pb);
        }
    }

    fprintf(stderr, "rebuild_index: chunk_count=%u stsc=%u stts=%u\n",
            chunk_count, stsc_count, stts_count);

    /* Build one index entry per chunk */
    int64_t  dts        = 0;
    uint32_t stts_ei    = 0;
    uint32_t stts_rem   = (stts_count > 0) ? stts_entries[0].count : 0;

    for (uint32_t c = 0; c < chunk_count; c++) {
        /* samples-per-chunk: find last stsc entry with first_chunk <= c+1 */
        uint32_t spc = 1;
        for (int32_t s = (int32_t)stsc_count - 1; s >= 0; s--) {
            if (stsc_entries[s].first_chunk <= c + 1) {
                spc = stsc_entries[s].spc;
                break;
            }
        }

        /* Chunk size: distance to next chunk offset; last chunk → EOF.
           The MOV demuxer uses AVIndexEntry.size to read bytes from the file —
           passing 0 yields empty packets even if the index entry is found. */
        int64_t chunk_size = (c + 1 < chunk_count)
                             ? chunk_offsets[c + 1] - chunk_offsets[c]
                             : file_size - chunk_offsets[c];
        if (chunk_size < 0) chunk_size = 0;

        av_add_index_entry(st, chunk_offsets[c], dts, (int)chunk_size, 0, AVINDEX_KEYFRAME);

        /* Advance DTS by spc samples */
        uint32_t left = spc;
        while (left > 0 && stts_ei < stts_count) {
            uint32_t take = left < stts_rem ? left : stts_rem;
            dts      += (int64_t)take * stts_entries[stts_ei].delta;
            stts_rem -= take;
            left     -= take;
            if (stts_rem == 0) {
                stts_ei++;
                stts_rem = (stts_ei < stts_count) ? stts_entries[stts_ei].count : 0;
            }
        }
    }

    av_free(stts_entries);
    av_free(stsc_entries);
    av_free(chunk_offsets);

    fprintf(stderr, "rebuild_index: added %u entries, final dts=%lld\n",
            chunk_count, (long long)dts);
    return (int)chunk_count;
}

/* -------------------------------------------------------------------------
 * Demux thread
 * ---------------------------------------------------------------------- */

static int demux_thread(void *userdata) {
    DemuxCtx *d = (DemuxCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();
    int log_next_pkt = 0; /* set after each seek to log the first post-seek packet */

    while (!d->abort) {
        /* Handle pending seek request */
        SDL_LockMutex(d->seek_mutex);
        if (d->seek_pending) {
            int64_t ts = (int64_t)(d->seek_target * AV_TIME_BASE);
            int seek_ret = av_seek_frame(d->fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
            fprintf(stderr, "demux_thread: seek to %.2fs ts=%lld ret=%d\n",
                    d->seek_target, (long long)ts, seek_ret);
            /* Flush queue, clear interrupt flag, send flush sentinel.
               Compute the exact seek target in stream timebase units so the
               audio decode thread can discard frames that precede the target
               (needed when the index has coarse chunk-level entries). */
            int64_t flush_pts = AV_NOPTS_VALUE;
            if (d->audio_stream_idx >= 0) {
                AVRational tb = d->fmt_ctx->streams[d->audio_stream_idx]->time_base;
                if (tb.num > 0 && tb.den > 0)
                    flush_pts = (int64_t)(d->seek_target / av_q2d(tb) + 0.5);
            }
            fprintf(stderr, "demux_thread: flush_pts=%lld (seek_target=%.2fs)\n",
                    (long long)flush_pts, d->seek_target);
            log_next_pkt = 1;
            packet_queue_flush(&d->audio_queue);
            SDL_LockMutex(d->audio_queue.mutex);
            d->audio_queue.interrupt = 0;
            SDL_UnlockMutex(d->audio_queue.mutex);
            packet_queue_put_flush(&d->audio_queue, flush_pts);
            d->seek_pending = 0;
            SDL_CondSignal(d->seek_cond);
        }
        SDL_UnlockMutex(d->seek_mutex);

        int ret = av_read_frame(d->fmt_ctx, pkt);
        if (ret < 0) {
            char av_ebuf[64] = {0};
            av_strerror(ret, av_ebuf, sizeof(av_ebuf));
            fprintf(stderr, "demux_thread: av_read_frame → %d (%s), sending EOS\n",
                    ret, av_ebuf);
            packet_queue_put(&d->audio_queue, NULL); /* EOS */
            break;
        }

        if (pkt->stream_index == d->audio_stream_idx) {
            if (log_next_pkt) {
                fprintf(stderr, "demux_thread: first pkt after seek pts=%lld dts=%lld\n",
                        (long long)pkt->pts, (long long)pkt->dts);
                log_next_pkt = 0;
            }
            packet_queue_put(&d->audio_queue, pkt);
        }

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

    if (d->audio_stream_idx >= 0) {
        AVStream *das = d->fmt_ctx->streams[d->audio_stream_idx];
        if (avformat_index_get_entries_count(das) == 0 && das->nb_frames > 0) {
            fprintf(stderr, "demux_open: nb_index=0 but nb_frames=%lld — "
                    "rebuilding index from stco/stsc/stts\n",
                    (long long)das->nb_frames);
            demux_rebuild_index(d->fmt_ctx, d->audio_stream_idx);
            fprintf(stderr, "demux_open: after rebuild nb_index=%d\n",
                    avformat_index_get_entries_count(das));
        }
    }

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

/* -------------------------------------------------------------------------
 * Background pre-open — start demux_open in a thread so it's ready by
 * the time the user presses play.  demux_preopen_claim() transfers
 * ownership to the caller (returns 1) or signals not ready / wrong path (0).
 * Call demux_preopen_cancel() at app exit if unused.
 * ---------------------------------------------------------------------- */

static pthread_t     s_preopen_thread;
static DemuxCtx      s_preopen_ctx;
static char          s_preopen_path[1024];
static atomic_int    s_preopen_state; /* 0=idle, 1=running, 2=done, -1=failed */

static void *preopen_worker(void *arg) {
    (void)arg;
    char errbuf[256];
    int r = demux_open(&s_preopen_ctx, s_preopen_path, errbuf, sizeof(errbuf));
    atomic_store(&s_preopen_state, r == 0 ? 2 : -1);
    if (r != 0)
        fprintf(stderr, "demux_preopen: failed: %s\n", errbuf);
    else
        fprintf(stderr, "demux_preopen: ready — %s\n", s_preopen_path);
    return NULL;
}

void demux_preopen_start(const char *path) {
    if (atomic_load(&s_preopen_state) != 0) return; /* already running */
    snprintf(s_preopen_path, sizeof(s_preopen_path), "%s", path);
    atomic_store(&s_preopen_state, 1);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&s_preopen_thread, &attr, preopen_worker, NULL);
    pthread_attr_destroy(&attr);
}

int demux_preopen_claim(const char *path, DemuxCtx *out) {
    if (atomic_load(&s_preopen_state) != 2) return 0;
    if (strcmp(path, s_preopen_path) != 0)  return 0;
    pthread_join(s_preopen_thread, NULL);
    *out = s_preopen_ctx;
    memset(&s_preopen_ctx, 0, sizeof(s_preopen_ctx));
    atomic_store(&s_preopen_state, 0);
    s_preopen_path[0] = '\0';
    return 1;
}

int demux_preopen_is_done(void) {
    int st = atomic_load(&s_preopen_state);
    return st == 2 || st == -1;
}

void demux_preopen_cancel(void) {
    int st = atomic_load(&s_preopen_state);
    if (st == 0) return;
    if (st == 1 || st == 2) {
        pthread_join(s_preopen_thread, NULL);
        if (atomic_load(&s_preopen_state) == 2)
            demux_close(&s_preopen_ctx);
    }
    atomic_store(&s_preopen_state, 0);
    s_preopen_path[0] = '\0';
}
