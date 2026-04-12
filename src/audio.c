#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifdef SB_A30
#include <unistd.h>
#include <spawn.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

/* -------------------------------------------------------------------------
 * Ring buffer
 * ---------------------------------------------------------------------- */

static int ring_init(AudioRingBuf *r) {
    r->buf      = malloc(AUDIO_RING_SIZE);
    if (!r->buf) return -1;
    r->capacity = AUDIO_RING_SIZE;
    r->filled   = 0;
    r->read_pos = r->write_pos = 0;
    r->mutex    = SDL_CreateMutex();
    r->not_full = SDL_CreateCond();
    return 0;
}

static void ring_destroy(AudioRingBuf *r) {
    free(r->buf);
    SDL_DestroyMutex(r->mutex);
    SDL_DestroyCond(r->not_full);
    memset(r, 0, sizeof(*r));
}

/* Write exactly `len` bytes from `src` to the ring buffer.
   Blocks if the ring is full. Returns -1 if aborted. */
static int ring_write(AudioRingBuf *r, const uint8_t *src, int len,
                      int *abort_flag) {
    int written = 0;
    while (written < len) {
        SDL_LockMutex(r->mutex);
        while (r->filled == r->capacity && !*abort_flag)
            SDL_CondWait(r->not_full, r->mutex);
        if (*abort_flag) { SDL_UnlockMutex(r->mutex); return -1; }

        int space = r->capacity - r->filled;
        int chunk = len - written;
        if (chunk > space) chunk = space;

        /* Handle wrap-around */
        int tail_space = r->capacity - r->write_pos;
        if (chunk > tail_space) {
            memcpy(r->buf + r->write_pos, src + written, (size_t)tail_space);
            memcpy(r->buf, src + written + tail_space, (size_t)(chunk - tail_space));
        } else {
            memcpy(r->buf + r->write_pos, src + written, (size_t)chunk);
        }
        r->write_pos = (r->write_pos + chunk) % r->capacity;
        r->filled   += chunk;
        written     += chunk;
        SDL_UnlockMutex(r->mutex);
    }
    return 0;
}

/* SDL audio callback — called from SDL's audio thread.
   Must never block. Reads up to `len` bytes; pads with silence if underrun. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    AudioCtx    *a = (AudioCtx *)userdata;

#ifdef SB_A30
    /* Measure actual callback rate to detect hardware rate mismatches.
       Logs after 20 callbacks (~1–2 s at expected rates). */
    {
        int n = (int)atomic_fetch_add(&a->cb_count, 1);
        if (n == 0)
            atomic_store(&a->cb_t0, (int)SDL_GetTicks());
        else if (n == 19) {
            Uint32 elapsed = (Uint32)SDL_GetTicks() - (Uint32)atomic_load(&a->cb_t0);
            if (elapsed > 0) {
                /* 19 intervals between callbacks 0…19 → 19 × AUDIO_SDL_SAMPLES samples */
                unsigned est = (unsigned)AUDIO_SDL_SAMPLES * 19u * 1000u / elapsed;
                fprintf(stderr, "audio_callback: 20 calls in %u ms → est_rate=%u Hz "
                        "(device=%d Hz)\n", elapsed, est, a->out_rate);
            }
        }
    }
#endif

    /* After sleep/wake the A30 ALSA driver fires a storm of underruns while
       it reinitialises.  Each underrun causes the callback to consume ring
       data that ALSA never actually plays, producing drift and pops.
       Serve silence (without touching the ring) until the grace window expires
       so the storm exhausts itself against nothing. */
    Uint32 silence_until = atomic_load_explicit(&a->wake_silence_until,
                                                memory_order_relaxed);
    if (silence_until && SDL_GetTicks() < silence_until) {
        memset(stream, 0, (size_t)len);
        return;
    }

    AudioRingBuf *r = &a->ring;
    int read = 0;

    SDL_LockMutex(r->mutex);
    int avail = r->filled < len ? r->filled : len;
    if (avail > 0) {
        int tail = r->capacity - r->read_pos;
        if (avail > tail) {
            memcpy(stream,        r->buf + r->read_pos, (size_t)tail);
            memcpy(stream + tail, r->buf,               (size_t)(avail - tail));
        } else {
            memcpy(stream, r->buf + r->read_pos, (size_t)avail);
        }
        r->read_pos = (r->read_pos + avail) % r->capacity;
        r->filled  -= avail;
        read        = avail;
        SDL_CondSignal(r->not_full);
    }
    SDL_UnlockMutex(r->mutex);

    /* Silence for any underrun */
    if (read < len)
        memset(stream + read, 0, (size_t)(len - read));

    /* Apply app-level volume by scaling S16 samples in-place */
    float vol = atomic_load_explicit(&a->volume, memory_order_relaxed);
    if (vol < 0.999f) {
        int16_t *samples = (int16_t *)stream;
        int n = len / 2;
        for (int i = 0; i < n; i++)
            samples[i] = (int16_t)(samples[i] * vol);
    }
}

/* -------------------------------------------------------------------------
 * atempo filter graph — called only from the decode thread
 * ---------------------------------------------------------------------- */

/* Build (or rebuild) the abuffer → atempo → abuffersink filter graph.
   frm must be a valid decoded frame (used to derive input format/rate/layout).
   Returns 0 on success, negative on failure (caller falls back to passthrough). */
static int build_atempo_graph(AudioCtx *a, const AVFrame *frm, float speed) {
    avfilter_graph_free(&a->filter_graph);
    a->buffersrc_ctx  = NULL;
    a->buffersink_ctx = NULL;

    a->filter_graph = avfilter_graph_alloc();
    if (!a->filter_graph) return -1;

    /* Describe input channel layout as a string for the abuffer args */
    char ch_desc[64] = "stereo";
    av_channel_layout_describe(&frm->ch_layout, ch_desc, sizeof(ch_desc));

    char src_args[256];
    snprintf(src_args, sizeof(src_args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channels=%d:channel_layout=%s",
             frm->sample_rate,
             frm->sample_rate,
             av_get_sample_fmt_name((enum AVSampleFormat)frm->format),
             frm->ch_layout.nb_channels,
             ch_desc);

    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    const AVFilter *atempo_flt  = avfilter_get_by_name("atempo");

    if (!abuffersrc || !abuffersink || !atempo_flt) {
        fprintf(stderr, "build_atempo_graph: filter not found\n");
        avfilter_graph_free(&a->filter_graph);
        return -1;
    }

    int ret;
    ret = avfilter_graph_create_filter(&a->buffersrc_ctx, abuffersrc, "in",
                                        src_args, NULL, a->filter_graph);
    if (ret < 0) goto err;

    ret = avfilter_graph_create_filter(&a->buffersink_ctx, abuffersink, "out",
                                        NULL, NULL, a->filter_graph);
    if (ret < 0) goto err;

    /* Force sink to output FLTP so swr_convert always receives the format it
       was configured for.  atempo converts internally to S16; without this
       constraint the graph leaves the output as S16, whose data[1]==NULL
       causes a segfault when swr tries to read the second (right) channel. */
    {
        static const enum AVSampleFormat sink_fmts[] = {
            AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE
        };
        ret = av_opt_set_int_list(a->buffersink_ctx, "sample_fmts", sink_fmts,
                                   AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) goto err;
    }

    /* atempo accepts 0.5–2.0; our presets are all within range */
    char tempo_arg[32];
    snprintf(tempo_arg, sizeof(tempo_arg), "tempo=%.4f", (double)speed);
    AVFilterContext *atempo_ctx = NULL;
    ret = avfilter_graph_create_filter(&atempo_ctx, atempo_flt, "atempo",
                                        tempo_arg, NULL, a->filter_graph);
    if (ret < 0) goto err;

    ret = avfilter_link(a->buffersrc_ctx, 0, atempo_ctx, 0); if (ret < 0) goto err;
    ret = avfilter_link(atempo_ctx, 0, a->buffersink_ctx, 0); if (ret < 0) goto err;

    ret = avfilter_graph_config(a->filter_graph, NULL);
    if (ret < 0) goto err;

    a->filter_speed = speed;
    return 0;

err:
    {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "build_atempo_graph: %s\n", errbuf);
    }
    avfilter_graph_free(&a->filter_graph);
    a->buffersrc_ctx  = NULL;
    a->buffersink_ctx = NULL;
    return ret;
}

/* Process one decoded frame through the atempo filter graph.
   Writes all output frames to the ring buffer and updates clock. */
static void filter_and_write(AudioCtx *a, AVFrame *frm, uint8_t **out_buf,
                              int *out_buf_size) {
    int ret = av_buffersrc_add_frame_flags(a->buffersrc_ctx, frm,
                                            AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) return;

    AVFrame *flt = av_frame_alloc();
    if (!flt) return;

    while (!a->abort &&
           av_buffersink_get_frame(a->buffersink_ctx, flt) >= 0) {

        int out_samples = swr_get_out_samples(a->swr, flt->nb_samples);
        int out_bytes   = out_samples * AUDIO_OUT_CHANNELS * 2;
        if (out_bytes > *out_buf_size) {
            *out_buf_size = out_bytes;
            *out_buf = realloc(*out_buf, (size_t)*out_buf_size);
        }
        uint8_t *out_ptr = *out_buf;
        int converted = swr_convert(a->swr,
                                    &out_ptr, out_samples,
                                    (const uint8_t **)flt->data, flt->nb_samples);
        if (converted > 0 &&
            !atomic_load_explicit(&a->clock_seek_active, memory_order_relaxed)) {
            int bytes = converted * AUDIO_OUT_CHANNELS * 2;
            ring_write(&a->ring, *out_buf, bytes, &a->abort);

            if (flt->pts != AV_NOPTS_VALUE) {
                double pts = (double)flt->pts *
                             av_q2d(a->codec_ctx->pkt_timebase) +
                             (double)converted / a->out_rate
                             - a->start_sec;
                SDL_LockMutex(a->clock_mutex);
                a->clock = pts;
                SDL_UnlockMutex(a->clock_mutex);
            }
        }
        av_frame_unref(flt);
    }
    av_frame_free(&flt);
}

/* -------------------------------------------------------------------------
 * Audio decode thread
 * ---------------------------------------------------------------------- */

static int audio_decode_thread(void *userdata) {
    AudioCtx *a   = (AudioCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();

    /* Output buffer: enough for one frame resampled */
    int out_buf_size = 192000; /* 1s at 48kHz stereo S16 — more than enough */
    uint8_t *out_buf = malloc((size_t)out_buf_size);

    /* When chunk-level index is used (Lavf57 stsz-failure recovery), the MOV
       demuxer stamps each chunk packet with a single DTS covering thousands of
       AAC frames.  Only the first decoded frame per chunk gets a valid pts from
       the codec; subsequent frames within the chunk come out AV_NOPTS_VALUE,
       which would leave the clock frozen between chunk boundaries.
       Track the expected pts across frames and fill it in when missing. */
    int64_t inferred_pts    = AV_NOPTS_VALUE;
    int64_t discard_until   = AV_NOPTS_VALUE; /* fast-forward to seek target */
    int     log_first_frame = 0;              /* log first decoded frame after each flush */

    while (!a->abort) {
        /* Snapshot flush_gen before blocking in packet_queue_get.  If a new
           flush sentinel arrives while we're inside the inner decode loop
           (including while ring_write is blocking on the SDL callback), we
           detect the change and break out so the outer loop can process the
           new seek within one SDL callback cycle (~185ms). */
        int my_flush_gen = atomic_load_explicit(&a->pkt_queue->flush_gen,
                                                memory_order_relaxed);
        int ret = packet_queue_get(a->pkt_queue, pkt);
        if (ret < 0) break;  /* abort */
        if (ret == 0) {      /* seek-flush sentinel: reset codec + ring + filter */
            /* pkt->pts carries the exact seek target in stream timebase (set by
               demux_thread).  We will decode but discard frames whose pts is
               before that target so the audio resumes at the precise position
               even though av_seek_frame only lands on a coarse chunk boundary. */
            discard_until = pkt->pts; /* AV_NOPTS_VALUE = no discard needed */
            fprintf(stderr, "audio: flush sentinel pkt->pts=%lld → discard_until=%lld\n",
                    (long long)pkt->pts, (long long)discard_until);
            avcodec_flush_buffers(a->codec_ctx);
            /* Free filter graph — it will be rebuilt with correct format on next frame */
            avfilter_graph_free(&a->filter_graph);
            a->buffersrc_ctx  = NULL;
            a->buffersink_ctx = NULL;
            a->filter_speed   = 0.0f;
            /* Clear buffered audio so old samples don't play after seek */
            SDL_LockMutex(a->ring.mutex);
            a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
            SDL_CondSignal(a->ring.not_full);
            SDL_UnlockMutex(a->ring.mutex);
            /* Keep clock_seek_active=1 while discarding pre-target frames;
               clear immediately if no discard is needed (fine-grained index). */
            if (discard_until != AV_NOPTS_VALUE)
                atomic_store(&a->clock_seek_active, 1);
            else
                atomic_store(&a->clock_seek_active, 0);
            inferred_pts = AV_NOPTS_VALUE;
            log_first_frame = 1;
            continue;
        }
        if (!pkt->data) {    /* EOS */
            avcodec_flush_buffers(a->codec_ctx);
            av_packet_unref(pkt);
            a->eos = 1;
            break;
        }

        avcodec_send_packet(a->codec_ctx, pkt);
        av_packet_unref(pkt);

        while (!a->abort &&
               avcodec_receive_frame(a->codec_ctx, frm) == 0) {

            /* Fill in pts for frames that come out AV_NOPTS_VALUE (happens for
               frames 2..N within a chunk-level packet).  frm->nb_samples is in
               audio samples; pkt_timebase is 1/sample_rate for AAC/M4B, so
               adding nb_samples advances the pts by exactly one frame. */
            if (frm->pts == AV_NOPTS_VALUE && inferred_pts != AV_NOPTS_VALUE)
                frm->pts = inferred_pts;
            if (frm->pts != AV_NOPTS_VALUE)
                inferred_pts = frm->pts + frm->nb_samples;

            if (log_first_frame) {
                fprintf(stderr, "audio: first frame after flush frm->pts=%lld discard_until=%lld\n",
                        (long long)frm->pts, (long long)discard_until);
                log_first_frame = 0;
            }

            /* Once we have decoded past the seek target, stop discarding and
               let normal ring-write / clock-update logic resume.  For files
               with fine-grained indexes this triggers on the very first frame
               (discard_until == AV_NOPTS_VALUE after flush → no-op).
               After writing this frame, break out of the inner loop so the
               outer loop can pick up any new flush sentinel immediately — on
               chunk-level files (V2/V3) each chunk is ~128 s, and staying in
               the inner loop would prevent seeks from being processed for up
               to ~60 s of real-time playback. */
            if (discard_until != AV_NOPTS_VALUE &&
                frm->pts != AV_NOPTS_VALUE &&
                frm->pts >= discard_until) {
                fprintf(stderr, "audio: discard released frm->pts=%lld discard_until=%lld\n",
                        (long long)frm->pts, (long long)discard_until);
                discard_until = AV_NOPTS_VALUE;
                atomic_store(&a->clock_seek_active, 0);
            }

            float spd = atomic_load_explicit(&a->speed, memory_order_relaxed);

            if (spd > 1.001f) {
                /* Speed != 1.0 — route through atempo filter.
                   Rebuild graph if speed changed or graph was cleared by a seek. */
                if (!a->filter_graph ||
                    fabsf(spd - a->filter_speed) > 0.001f) {
                    if (build_atempo_graph(a, frm, spd) < 0) {
                        goto direct_path;
                    }
                }
                filter_and_write(a, frm, &out_buf, &out_buf_size);
                av_frame_unref(frm);
                /* Check for new seek before continuing (see flush_gen comment above) */
                if (atomic_load_explicit(&a->pkt_queue->flush_gen,
                                         memory_order_relaxed) != my_flush_gen)
                    break;
                continue;
            }

        direct_path:
            /* Normal 1x speed path: swr_convert directly */
            {
                int out_samples = swr_get_out_samples(a->swr, frm->nb_samples);
                int out_bytes   = out_samples * AUDIO_OUT_CHANNELS * 2;
                if (out_bytes > out_buf_size) {
                    out_buf_size = out_bytes;
                    out_buf = realloc(out_buf, (size_t)out_buf_size);
                }
                uint8_t *out_ptr = out_buf;
                int converted = swr_convert(a->swr,
                                            &out_ptr, out_samples,
                                            (const uint8_t **)frm->data,
                                            frm->nb_samples);
                if (converted > 0 &&
                    !atomic_load_explicit(&a->clock_seek_active,
                                          memory_order_relaxed)) {
                    int bytes = converted * AUDIO_OUT_CHANNELS * 2;
                    ring_write(&a->ring, out_buf, bytes, &a->abort);

                    if (frm->pts != AV_NOPTS_VALUE) {
                        double pts = (double)frm->pts *
                                     av_q2d(a->codec_ctx->pkt_timebase) +
                                     (double)converted / a->out_rate
                                     - a->start_sec;
                        SDL_LockMutex(a->clock_mutex);
                        a->clock = pts;
                        SDL_UnlockMutex(a->clock_mutex);
                    }
                }
            }
            av_frame_unref(frm);
            /* Break inner loop if a new flush sentinel has been enqueued while
               we were decoding / blocked in ring_write.  The outer loop will
               call packet_queue_get and receive the flush sentinel, ensuring
               seeks are processed within ~185ms (one SDL callback interval). */
            if (atomic_load_explicit(&a->pkt_queue->flush_gen,
                                     memory_order_relaxed) != my_flush_gen)
                break;
        }
    }

    free(out_buf);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int audio_open(AudioCtx *a, AVCodecParameters *codec_params,
               PacketQueue *pkt_queue, char *errbuf, int errbuf_sz) {
    memset(a, 0, sizeof(*a));
    a->pkt_queue  = pkt_queue;
    a->clock_mutex = SDL_CreateMutex();
    atomic_store(&a->volume, 1.0f);
    atomic_store(&a->speed,  1.0f);
    atomic_store(&a->clock_seek_active, 0);

    /* Find and open codec */
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "No decoder for codec id %d", codec_params->codec_id);
        return -1;
    }
    a->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a->codec_ctx, codec_params);
#ifdef SB_A30
    /* Single decode thread to conserve RAM on Mini/A30 (103MB, no swap).
       AAC/MP3 decoders don't meaningfully parallelise anyway, but setting
       this prevents FFmpeg from allocating per-thread context copies. */
    a->codec_ctx->thread_count = 1;
#endif

    int ret = avcodec_open2(a->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* Ring buffer */
    if (ring_init(&a->ring) < 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "ring buffer alloc failed");
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* SDL audio device — open first to discover the actual negotiated rate.
     * Allow frequency change so devices that only support 44100Hz (e.g.
     * Allwinner V3s on Miyoo Mini/Flip) don't fail with "Couldn't set
     * audio frequency". */
    SDL_AudioSpec want = {
        .freq     = AUDIO_OUT_RATE,
        .format   = AUDIO_SDL_FORMAT,
        .channels = AUDIO_OUT_CHANNELS,
        .samples  = AUDIO_SDL_SAMPLES,
        .callback = audio_callback,
        .userdata = a,
    };
    SDL_AudioSpec got;

#ifdef SB_A30
    /* MiyooMini V2/V3: the ALSA hw:0,0 driver always runs its DMA at HALF the
       configured sample rate — confirmed by callback-rate measurements across
       multiple attempts (requested 48000Hz→DMA 24000Hz, 44100Hz→22050Hz,
       22050Hz→11025Hz).  Fix: request 44100Hz with no ALLOW so the driver
       can't negotiate to 48000Hz, then override a->out_rate = got.freq/2
       (= 22050Hz).  swr outputs at 22050Hz which matches the true DMA rate
       → correct playback speed with a clean 2:1 integer downsample. */
    {
        const char *sb_plat = getenv("SB_PLATFORM");
        int is_mini = sb_plat && strcmp(sb_plat, "MiyooMini") == 0;

        if (is_mini) {
            a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
            if (!a->dev) {
                a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                             SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
                fprintf(stderr, "audio_open: MiyooMini ALLOW fallback got=%dHz\n",
                        got.freq);
            } else {
                fprintf(stderr, "audio_open: MiyooMini fixed-rate OK got=%dHz\n",
                        got.freq);
            }
        } else {
            a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                         SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        }
    }
#else
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
#endif

    if (a->dev == 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "SDL_OpenAudioDevice: %s", SDL_GetError());
        ring_destroy(&a->ring);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }
#ifdef SB_A30
    {
        const char *sb_plat = getenv("SB_PLATFORM");
        if (sb_plat && strcmp(sb_plat, "MiyooMini") == 0) {
            /* True DMA rate = got.freq / 2 — override so swr and clock
               are calibrated to the hardware's actual sample consumption rate */
            a->out_rate = got.freq / 2;
            fprintf(stderr, "audio_open: MiyooMini hw_rate=%dHz (got/2, got=%dHz)\n",
                    a->out_rate, got.freq);
        } else {
            a->out_rate = got.freq;
        }
    }
#else
    a->out_rate = got.freq;
#endif
    fprintf(stderr, "audio_open: want=%dHz got=%dHz fmt=%d ch=%d\n",
            AUDIO_OUT_RATE, got.freq, got.format, got.channels);

    /* Set up swresample: input = file's format, output = S16 stereo at got.freq.
       Use codec_ctx fields (populated by avcodec_open2) rather than codec_params
       directly: avcodec_open2 resolves AV_SAMPLE_FMT_NONE that limited probesize
       can leave in codec_params for large files (e.g. 20+ hour M4B on V2/V3). */
    enum AVSampleFormat in_fmt = a->codec_ctx->sample_fmt;
    if (in_fmt == AV_SAMPLE_FMT_NONE) {
        /* Should never happen after avcodec_open2, but guard just in case */
        fprintf(stderr, "audio_open: sample_fmt NONE after open2, defaulting FLTP\n");
        in_fmt = AV_SAMPLE_FMT_FLTP;
    }
    int in_rate = a->codec_ctx->sample_rate > 0 ? a->codec_ctx->sample_rate
                                                 : codec_params->sample_rate;
    /* ch_layout: prefer codec_ctx (set by open2), fall back to codec_params */
    AVChannelLayout *in_layout = (a->codec_ctx->ch_layout.nb_channels > 0)
                                 ? &a->codec_ctx->ch_layout
                                 : &codec_params->ch_layout;

    fprintf(stderr, "audio_open: swr in fmt=%d rate=%d ch=%d → out fmt=S16 rate=%d ch=%d\n",
            in_fmt, in_rate, in_layout->nb_channels, a->out_rate, AUDIO_OUT_CHANNELS);

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, AUDIO_OUT_CHANNELS);

    ret = swr_alloc_set_opts2(&a->swr,
                              &out_layout, AV_SAMPLE_FMT_S16, a->out_rate,
                              in_layout, in_fmt, in_rate,
                              0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr_init(a->swr) < 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "swr_init failed");
        fprintf(stderr, "audio_open: swr_init failed (ret=%d fmt=%d rate=%d ch=%d)\n",
                ret, in_fmt, in_rate, in_layout->nb_channels);
        SDL_CloseAudioDevice(a->dev); a->dev = 0;
        ring_destroy(&a->ring);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }
    return 0;
}

void audio_start(AudioCtx *a) {
    SDL_PauseAudioDevice(a->dev, 0); /* start playback */
    a->thread = SDL_CreateThread(audio_decode_thread, "audio", a);
}

void audio_pause(AudioCtx *a, int pause) {
    SDL_PauseAudioDevice(a->dev, pause);
}

void audio_stop(AudioCtx *a) {
    a->abort = 1;
    /* Wake the decode thread if it's blocked on ring_write */
    SDL_LockMutex(a->ring.mutex);
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
    /* Wake the decode thread if it's blocked in packet_queue_get waiting for
       packets — ring signal alone won't reach it in that case, causing
       SDL_WaitThread to hang until demux_stop aborts the queue (too late). */
    if (a->pkt_queue) packet_queue_abort(a->pkt_queue);
    if (a->thread) { SDL_WaitThread(a->thread, NULL); a->thread = NULL; }
    SDL_PauseAudioDevice(a->dev, 1);
}

void audio_close(AudioCtx *a) {
    avfilter_graph_free(&a->filter_graph);
    if (a->dev)       SDL_CloseAudioDevice(a->dev);
    if (a->swr)       swr_free(&a->swr);
    if (a->codec_ctx) avcodec_free_context(&a->codec_ctx);
    if (a->clock_mutex) SDL_DestroyMutex(a->clock_mutex);
    ring_destroy(&a->ring);
    memset(a, 0, sizeof(*a));
}

void audio_set_speed(AudioCtx *a, float speed) {
    atomic_store_explicit(&a->speed, speed, memory_order_relaxed);
    /* Flush ring so old-speed audio doesn't delay the transition.
       The decode thread will refill immediately at the new speed. */
    SDL_LockMutex(a->ring.mutex);
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
}

int audio_switch_stream(AudioCtx *a, PacketQueue *pkt_queue,
                        AVCodecParameters *cp, AVRational time_base,
                        char *errbuf, int errbuf_sz) {
    /* Stop decode thread */
    a->abort = 1;
    SDL_LockMutex(a->ring.mutex);
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
    packet_queue_abort(pkt_queue);
    if (a->thread) { SDL_WaitThread(a->thread, NULL); a->thread = NULL; }

    /* Free filter graph — will be rebuilt by the new decode thread */
    avfilter_graph_free(&a->filter_graph);
    a->buffersrc_ctx  = NULL;
    a->buffersink_ctx = NULL;
    a->filter_speed   = 0.0f;

    /* Flush queue and reset its abort flag so it can be reused */
    packet_queue_flush(pkt_queue);
    SDL_LockMutex(pkt_queue->mutex);
    pkt_queue->abort = 0;
    SDL_UnlockMutex(pkt_queue->mutex);

    /* Reinit codec */
    avcodec_free_context(&a->codec_ctx);
    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "No decoder for codec id %d", cp->codec_id);
        return -1;
    }
    a->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a->codec_ctx, cp);
    a->codec_ctx->pkt_timebase = time_base;
    int ret = avcodec_open2(a->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* Reinit swresample — same rationale as audio_open: use codec_ctx fields
       (post-avcodec_open2) rather than cp->format which may be NONE. */
    swr_free(&a->swr);
    {
        enum AVSampleFormat sw_fmt = a->codec_ctx->sample_fmt;
        if (sw_fmt == AV_SAMPLE_FMT_NONE) sw_fmt = AV_SAMPLE_FMT_FLTP;
        int sw_rate = a->codec_ctx->sample_rate > 0 ? a->codec_ctx->sample_rate
                                                     : cp->sample_rate;
        AVChannelLayout *sw_layout = (a->codec_ctx->ch_layout.nb_channels > 0)
                                     ? &a->codec_ctx->ch_layout
                                     : &cp->ch_layout;
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, AUDIO_OUT_CHANNELS);
        ret = swr_alloc_set_opts2(&a->swr,
                                  &out_layout, AV_SAMPLE_FMT_S16, a->out_rate,
                                  sw_layout, sw_fmt, sw_rate,
                                  0, NULL);
        av_channel_layout_uninit(&out_layout);
        if (ret < 0 || swr_init(a->swr) < 0) {
            if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "swr_init failed");
            avcodec_free_context(&a->codec_ctx);
            return -1;
        }
    }

    /* Clear ring buffer, but first subtract the ring's buffered delay from
       the clock so it reflects the actual playing position rather than the
       decoded-but-not-yet-played position. */
    SDL_LockMutex(a->ring.mutex);
    double bytes_per_sec = a->out_rate * AUDIO_OUT_CHANNELS * 2.0;
    double ring_delay    = a->ring.filled / bytes_per_sec;
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);

    SDL_LockMutex(a->clock_mutex);
    a->clock -= ring_delay;
    if (a->clock < 0.0) a->clock = 0.0;
    SDL_UnlockMutex(a->clock_mutex);

    /* Restart decode thread */
    a->abort = 0;
    a->eos   = 0;
    atomic_store(&a->clock_seek_active, 1);
    audio_start(a);
    return 0;
}

/* -------------------------------------------------------------------------
 * audio_wake — reinit SDL audio device + restore DAC after A30 sleep/wake
 * ---------------------------------------------------------------------- */

void audio_wake(AudioCtx *a) {
    if (!a->dev) return;

    double bps = a->out_rate * AUDIO_OUT_CHANNELS * 2.0;

    SDL_LockMutex(a->ring.mutex);
    fprintf(stderr, "audio_wake: (clock=%.3f ring_delay=%.3fs)\n",
            a->clock, a->ring.filled / bps);
    SDL_UnlockMutex(a->ring.mutex);

    SDL_CloseAudioDevice(a->dev);
    a->dev = 0;

    SDL_LockMutex(a->ring.mutex);
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);

    packet_queue_flush(a->pkt_queue);
    fprintf(stderr, "audio_wake: ring + queue cleared\n");

    SDL_AudioSpec want = {
        .freq     = a->out_rate,
        .format   = AUDIO_SDL_FORMAT,
        .channels = AUDIO_OUT_CHANNELS,
        .samples  = AUDIO_SDL_SAMPLES,
        .callback = audio_callback,
        .userdata = a,
    };
    SDL_AudioSpec got;
    /* Don't allow frequency change — a different rate causes pitch shift.
       On A30 the OSS device may not be immediately ready after wake; retry
       a few times with brief delays before giving up. */
    int attempts = 5;
    while (attempts-- > 0) {
        a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (a->dev) break;
        fprintf(stderr, "audio_wake: SDL_OpenAudioDevice failed (%d left): %s\n",
                attempts, SDL_GetError());
        SDL_Delay(200);
    }
    if (!a->dev)
        fprintf(stderr, "audio_wake: giving up — audio will be silent\n");
}

double audio_get_clock(const AudioCtx *a) {
    SDL_LockMutex(a->clock_mutex);
    double c = a->clock;
    SDL_UnlockMutex(a->clock_mutex);

    /* `clock` is the PTS of the end of the last *decoded* chunk.
       Subtract the audio that has been decoded but not yet played:
         - bytes sitting in the ring buffer
         - samples in SDL's internal device buffer                    */
    SDL_LockMutex(a->ring.mutex);
    int buffered_bytes = a->ring.filled;
    SDL_UnlockMutex(a->ring.mutex);

    double bytes_per_sec = a->out_rate * AUDIO_OUT_CHANNELS * 2.0; /* S16 */
    double ring_delay    = buffered_bytes / bytes_per_sec;
    double sdl_delay     = (double)AUDIO_SDL_SAMPLES / a->out_rate;

    double actual = c - ring_delay - sdl_delay;
    return actual > 0.0 ? actual : 0.0;
}
