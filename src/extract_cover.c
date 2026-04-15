/*
 * extract_cover.c — Batch embedded cover art extractor
 *
 * Usage: extract_cover dir1 [dir2 ...]
 *
 * For each directory:
 *   - Skips if cover.jpg or cover.png already exists (preserves user art)
 *   - Finds the first audio file in the directory
 *   - Extracts embedded cover art (AV_DISPOSITION_ATTACHED_PIC) to cover.jpg
 *
 * Spawned as a background process by storyboy on SB_A30 (SpruceOS armhf)
 * after the library scan, so cover extraction never blocks the main process
 * or contributes to heap fragmentation before player_open.
 *
 * Only links libavformat — no SDL, no decoder needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <malloc.h>
#include <libavformat/avformat.h>

static const char * const AUDIO_EXTS[] = {
    ".mp3", ".m4b", ".m4a", ".flac", ".ogg", ".aac", ".opus", ".wav", NULL
};

static int is_audio_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; AUDIO_EXTS[i]; i++)
        if (strcasecmp(dot, AUDIO_EXTS[i]) == 0) return 1;
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int extract_to_file(const char *audio_path, const char *dest) {
    /* Limit how much data FFmpeg reads during open — we only need the
       container header to find the attached picture stream.  32KB covers
       most well-formed M4B/MP3 headers; the codec-id fallback reads only
       the first matching packet, keeping peak RSS well under 10MB/book. */
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return 0;
    fmt->probesize            = 32 * 1024;
    fmt->max_analyze_duration = 0;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) < 0) {
        avformat_free_context(fmt);
        return 0;
    }

    /* Fast path: avformat_open_input often populates nb_streams with the
       attached picture without needing find_stream_info (saves the slow
       moov-atom seek on large M4B files). Guard pkt->size > 0 so we don't
       write an empty file and mark extraction as "done" for a future run. */
    int ok = 0;
    for (unsigned int i = 0; i < fmt->nb_streams && !ok; i++) {
        AVStream *st = fmt->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket *pkt = &st->attached_pic;
            if (pkt->size > 0) {
                FILE *f = fopen(dest, "wb");
                if (f) { fwrite(pkt->data, 1, (size_t)pkt->size, f); fclose(f); ok = 1; }
            }
        }
    }

    /* Fallback: find image stream by codec_id and read its first packet.
       Avoids avformat_find_stream_info (full stream probe), which can OOM
       on low-memory armhf devices when processing large M4B files. */
    if (!ok) {
        for (unsigned int i = 0; i < fmt->nb_streams && !ok; i++) {
            enum AVCodecID cid = fmt->streams[i]->codecpar->codec_id;
            if (cid != AV_CODEC_ID_MJPEG && cid != AV_CODEC_ID_PNG) continue;
            AVPacket *pkt = av_packet_alloc();
            if (!pkt) break;
            int packets_read = 0;
            while (av_read_frame(fmt, pkt) >= 0 && !ok && packets_read++ < 8) {
                if ((unsigned)pkt->stream_index == i && pkt->size > 0) {
                    FILE *f = fopen(dest, "wb");
                    if (f) { fwrite(pkt->data, 1, (size_t)pkt->size, f); fclose(f); ok = 1; }
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    avformat_close_input(&fmt);
    malloc_trim(0);
    return ok;
}

static void process_dir(const char *dir) {
    /* Skip if any cover already exists — protects embedded art, OL-fetched
       covers, and user-placed images equally. No sentinel file needed. */
    char cover_embedded[1280], cover_jpg[1280], cover_png[1280];
    snprintf(cover_embedded, sizeof(cover_embedded), "%s/cover_embedded.jpg", dir);
    snprintf(cover_jpg,      sizeof(cover_jpg),      "%s/cover.jpg",          dir);
    snprintf(cover_png,      sizeof(cover_png),      "%s/cover.png",          dir);
    if (file_exists(cover_embedded) || file_exists(cover_jpg) || file_exists(cover_png))
        return;

    /* Find first audio file */
    DIR *d = opendir(dir);
    if (!d) return;
    char audio[1280] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (is_audio_file(e->d_name)) {
            snprintf(audio, sizeof(audio), "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);

    if (!audio[0]) return;

    /* Write to cover_embedded.jpg — this file is owned by extract_cover
       and takes priority over cover.jpg (Open Library) in the browser. */
    if (extract_to_file(audio, cover_embedded))
        fprintf(stderr, "extracted: %s\n", cover_embedded);

    /* Return glibc's cached free pages to the OS so the next book starts
       with a clean heap — prevents OOM accumulation across many books. */
    malloc_trim(0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        /* Fork a child for each directory so every book starts with a fresh
           heap.  FFmpeg's moov-atom allocations for large M4B files don't
           always return cleanly to glibc's arena; serial processing in one
           process accumulates memory across books and triggers OOM on armhf.
           Each child processes exactly one directory and exits; the parent
           waits before spawning the next, keeping peak RSS to one book. */
        pid_t pid = fork();
        if (pid == 0) {
            process_dir(argv[i]);
            exit(0);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
        } else {
            /* fork failed — fall back to in-process */
            process_dir(argv[i]);
        }
    }
    return 0;
}
