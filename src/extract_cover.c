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
#include <dirent.h>
#include <sys/stat.h>
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
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) < 0)
        return 0;
    avformat_find_stream_info(fmt, NULL);
    int ok = 0;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket *pkt = &st->attached_pic;
            FILE *f = fopen(dest, "wb");
            if (f) {
                fwrite(pkt->data, 1, (size_t)pkt->size, f);
                fclose(f);
                ok = 1;
            }
            break;
        }
    }
    avformat_close_input(&fmt);
    return ok;
}

static void process_dir(const char *dir) {
    /* Skip if the user has explicitly chosen cover art for this dir.
       .sb_cover_locked is written by StoryBoy when the user fetches art
       via the "A — fetch art" menu action; it prevents embedded art from
       overriding the user's choice across restarts. */
    char locked[1280];
    snprintf(locked, sizeof(locked), "%s/.sb_cover_locked", dir);
    if (file_exists(locked)) return;

    /* Skip if we've already extracted embedded art or user placed cover.png */
    char cover_embedded[1280], cover_png[1280];
    snprintf(cover_embedded, sizeof(cover_embedded), "%s/cover_embedded.jpg", dir);
    snprintf(cover_png, sizeof(cover_png), "%s/cover.png", dir);
    if (file_exists(cover_embedded) || file_exists(cover_png))
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
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        process_dir(argv[i]);
    return 0;
}
