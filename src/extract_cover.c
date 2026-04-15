/*
 * extract_cover.c — Batch embedded cover art extractor
 *
 * Usage: extract_cover dir1 [dir2 ...]
 *
 * For each directory:
 *   - Skips if cover_embedded.jpg, cover.jpg or cover.png already exists
 *   - Finds the first audio file in the directory
 *   - Extracts embedded cover art to cover_embedded.jpg
 *
 * Parser selection (by extension):
 *   .m4b / .m4a  — custom MPEG-4 atom walker, O(1) memory regardless of
 *                  file size.  Avoids FFmpeg's MOVIndexEntry table (~120MB
 *                  for a 28-hour audiobook) that OOMs on armhf devices.
 *   .mp3         — custom ID3v2 APIC-frame parser
 *   .flac        — custom FLAC METADATA_BLOCK_PICTURE parser
 *   everything else — FFmpeg fallback (OGG/OPUS/AAC/WAV tend to be small)
 *
 * Spawned as a background child per directory by storyboy on armhf (SB_A30).
 * Each child processes one directory then exits, so peak RSS == one book.
 */

/* Large-file support: off_t / fseeko / ftello are 64-bit even on 32-bit ARM */
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <malloc.h>
#include <libavformat/avformat.h>

/* -------------------------------------------------------------------------
 * Shared helpers
 * ---------------------------------------------------------------------- */

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

/* Read big-endian u32 */
static int rb32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *v = (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3];
    return 1;
}

/* Read big-endian u64 */
static int rb64(FILE *f, uint64_t *v) {
    uint32_t hi, lo;
    if (!rb32(f, &hi) || !rb32(f, &lo)) return 0;
    *v = ((uint64_t)hi << 32) | lo;
    return 1;
}

/* Copy at most max_bytes from current position in src to dest file.
   Sanity-caps at 10 MB; deletes dest on error. */
static int write_image(FILE *src, int64_t nbytes, const char *dest) {
    if (nbytes <= 0 || nbytes > 10 * 1024 * 1024) return 0;
    FILE *out = fopen(dest, "wb");
    if (!out) return 0;
    char buf[65536];
    int64_t rem = nbytes;
    int ok = 1;
    while (rem > 0 && ok) {
        size_t want = (rem > (int64_t)sizeof(buf)) ? sizeof(buf) : (size_t)rem;
        size_t got  = fread(buf, 1, want, src);
        if (got == 0) { ok = 0; break; }
        if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
        rem -= (int64_t)got;
    }
    fclose(out);
    if (!ok) unlink(dest);
    return ok && rem == 0;
}

/* -------------------------------------------------------------------------
 * MPEG-4 atom parser (M4B / M4A)
 *
 * Scans directly through the atom hierarchy without building any index
 * tables — memory use is O(1) regardless of audio track length.
 * ---------------------------------------------------------------------- */

/*
 * Find the first child atom with the given 4-byte name within
 * [region_start, region_start + region_size).
 *
 * On success: positions f at the first byte AFTER the atom header (i.e. the
 *             atom's payload), sets *payload_size, returns 1.
 * On failure: returns 0.
 *
 * Handles standard 8-byte headers and extended 16-byte headers (size==1).
 * Atom with size==0 (extends to end) is treated as ending at region_start +
 * region_size.
 */
static int find_atom(FILE *f,
                     int64_t region_start, int64_t region_size,
                     const char name[4],
                     int64_t *payload_size) {
    int64_t pos = region_start;
    int64_t end = region_start + region_size;

    while (pos < end - 7) {
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0) return 0;

        uint32_t size32;
        char     type[4];
        if (!rb32(f, &size32)) return 0;
        if (fread(type, 1, 4, f) != 4) return 0;

        int64_t atom_total;   /* total bytes including header */
        int64_t header_bytes; /* bytes consumed by header */
        if (size32 == 1) {
            uint64_t size64;
            if (!rb64(f, &size64)) return 0;
            atom_total   = (int64_t)size64;
            header_bytes = 16;
        } else if (size32 == 0) {
            /* extends to end of region */
            atom_total   = end - pos;
            header_bytes = 8;
        } else {
            atom_total   = (int64_t)size32;
            header_bytes = 8;
        }

        if (atom_total < header_bytes) return 0; /* malformed */

        if (memcmp(type, name, 4) == 0) {
            *payload_size = atom_total - header_bytes;
            /* f is already positioned at payload start */
            return 1;
        }

        pos += atom_total;
    }
    return 0;
}

/*
 * Try to extract cover art from the iTunes metadata path:
 *   moov / udta / meta[FullBox] / ilst / covr / data
 * and also the bare path (no udta wrapper, used by some encoders):
 *   moov / meta[FullBox] / ilst / covr / data
 *
 * The `meta` box is an ISO 14496-12 FullBox: its payload begins with
 * 4 bytes of version+flags before its child atoms.  Some broken encoders
 * omit those 4 bytes; we try with them first, then without.
 */
static int find_covr_data(FILE *f,
                          int64_t moov_payload_start,
                          int64_t moov_payload_size,
                          int64_t *img_start, int64_t *img_size) {
    /* --- Try moov/udta/meta/ilst/covr/data --- */
    int64_t udta_sz;
    if (find_atom(f, moov_payload_start, moov_payload_size, "udta", &udta_sz)) {
        int64_t udta_start = (int64_t)ftello(f);

        int64_t meta_sz;
        if (find_atom(f, udta_start, udta_sz, "meta", &meta_sz) && meta_sz > 4) {
            int64_t meta_payload = (int64_t)ftello(f);

            /* Try FullBox (skip 4 bytes version+flags) */
            int64_t ilst_sz;
            if (find_atom(f, meta_payload + 4, meta_sz - 4, "ilst", &ilst_sz)) {
                int64_t ilst_start = (int64_t)ftello(f);
                int64_t covr_sz;
                if (find_atom(f, ilst_start, ilst_sz, "covr", &covr_sz)) {
                    int64_t covr_start = (int64_t)ftello(f);
                    int64_t data_sz;
                    if (find_atom(f, covr_start, covr_sz, "data", &data_sz) && data_sz > 8) {
                        /* data payload: 4-byte type indicator + 4-byte locale + image */
                        *img_start = (int64_t)ftello(f) + 8;
                        *img_size  = data_sz - 8;
                        return 1;
                    }
                }
            }
            /* Try without FullBox header (broken encoders) */
            if (find_atom(f, meta_payload, meta_sz, "ilst", &ilst_sz)) {
                int64_t ilst_start = (int64_t)ftello(f);
                int64_t covr_sz;
                if (find_atom(f, ilst_start, ilst_sz, "covr", &covr_sz)) {
                    int64_t covr_start = (int64_t)ftello(f);
                    int64_t data_sz;
                    if (find_atom(f, covr_start, covr_sz, "data", &data_sz) && data_sz > 8) {
                        *img_start = (int64_t)ftello(f) + 8;
                        *img_size  = data_sz - 8;
                        return 1;
                    }
                }
            }
        }
    }

    /* --- Try moov/meta/ilst/covr/data (no udta wrapper) --- */
    int64_t meta_sz;
    if (find_atom(f, moov_payload_start, moov_payload_size, "meta", &meta_sz) && meta_sz > 4) {
        int64_t meta_payload = (int64_t)ftello(f);

        int64_t ilst_sz;
        if (find_atom(f, meta_payload + 4, meta_sz - 4, "ilst", &ilst_sz)) {
            int64_t ilst_start = (int64_t)ftello(f);
            int64_t covr_sz;
            if (find_atom(f, ilst_start, ilst_sz, "covr", &covr_sz)) {
                int64_t covr_start = (int64_t)ftello(f);
                int64_t data_sz;
                if (find_atom(f, covr_start, covr_sz, "data", &data_sz) && data_sz > 8) {
                    *img_start = (int64_t)ftello(f) + 8;
                    *img_size  = data_sz - 8;
                    return 1;
                }
            }
        }
        if (find_atom(f, meta_payload, meta_sz, "ilst", &ilst_sz)) {
            int64_t ilst_start = (int64_t)ftello(f);
            int64_t covr_sz;
            if (find_atom(f, ilst_start, ilst_sz, "covr", &covr_sz)) {
                int64_t covr_start = (int64_t)ftello(f);
                int64_t data_sz;
                if (find_atom(f, covr_start, covr_sz, "data", &data_sz) && data_sz > 8) {
                    *img_start = (int64_t)ftello(f) + 8;
                    *img_size  = data_sz - 8;
                    return 1;
                }
            }
        }
    }

    return 0;
}

static int extract_m4b_cover(const char *path, const char *dest) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseeko(f, 0, SEEK_END);
    int64_t file_size = (int64_t)ftello(f);
    fseeko(f, 0, SEEK_SET);

    /* Find moov — may be at the end of a multi-GB file (non-faststart).
       find_atom only reads 8-byte headers and seeks over data, so RSS stays
       tiny even when skipping a 1.5GB mdat atom. */
    int64_t moov_sz;
    if (!find_atom(f, 0, file_size, "moov", &moov_sz)) {
        fclose(f); return 0;
    }
    int64_t moov_payload_start = (int64_t)ftello(f);

    int64_t img_start, img_size;
    if (!find_covr_data(f, moov_payload_start, moov_sz, &img_start, &img_size)) {
        fclose(f); return 0;
    }

    fseeko(f, (off_t)img_start, SEEK_SET);
    int ok = write_image(f, img_size, dest);
    fclose(f);
    return ok;
}

/* -------------------------------------------------------------------------
 * ID3v2 parser (MP3)
 * ---------------------------------------------------------------------- */

/* Read an ID3v2 synchsafe integer from 4 bytes (7 usable bits per byte) */
static uint32_t synchsafe32(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14)
         | ((uint32_t)b[2] <<  7) |  (uint32_t)b[3];
}

/* Skip a null-terminated string; enc==1 or 2 means UTF-16 (2-byte nulls) */
static int skip_id3_string(FILE *f, uint8_t enc) {
    if (enc == 1 || enc == 2) {
        /* UTF-16: look for 0x00 0x00 pair on a 2-byte boundary */
        int b1, b2;
        while (1) {
            b1 = fgetc(f); if (b1 == EOF) return 0;
            b2 = fgetc(f); if (b2 == EOF) return 0;
            if (b1 == 0 && b2 == 0) return 1;
        }
    } else {
        /* Latin-1 or UTF-8: single null byte */
        int c;
        while ((c = fgetc(f)) != EOF) if (c == 0) return 1;
        return 0;
    }
}

static int extract_mp3_cover(const char *path, const char *dest) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* ID3v2 header: 3 magic + 1 major_version + 1 revision + 1 flags + 4 synchsafe */
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        fclose(f); return 0;
    }

    int      version   = hdr[3];
    uint8_t  flags     = hdr[5];
    uint32_t tag_size  = synchsafe32(hdr + 6);
    long     tag_end   = 10 + (long)tag_size;

    /* Skip extended header if present (v2.3+, flag bit 6) */
    if ((flags & 0x40) && version >= 3) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) { fclose(f); return 0; }
        uint32_t ext_size;
        if (version >= 4) {
            ext_size = synchsafe32(ext);
        } else {
            ext_size = (uint32_t)ext[0]<<24|(uint32_t)ext[1]<<16|(uint32_t)ext[2]<<8|ext[3];
            ext_size -= 4; /* v2.3 size includes these 4 bytes */
        }
        fseek(f, (long)ext_size, SEEK_CUR);
    }

    int  ok        = 0;
    long frame_pos = ftell(f);

    while (!ok && frame_pos < tag_end) {
        if (fseek(f, frame_pos, SEEK_SET) != 0) break;

        if (version == 2) {
            /* ID3v2.2: 3-char ID + 3-byte size (big-endian, not synchsafe) */
            uint8_t fh[6];
            if (fread(fh, 1, 6, f) != 6 || fh[0] == 0) break;
            uint32_t fsz = (uint32_t)fh[3]<<16 | (uint32_t)fh[4]<<8 | fh[5];
            long     next = frame_pos + 6 + (long)fsz;

            if (memcmp(fh, "PIC", 3) == 0 && fsz > 5) {
                uint8_t enc;
                if (fread(&enc, 1, 1, f) != 1) break;
                fseek(f, 3, SEEK_CUR); /* image format (3 chars, e.g. "JPG") */
                fseek(f, 1, SEEK_CUR); /* picture type */
                if (!skip_id3_string(f, enc)) break;
                long img_start = ftell(f);
                ok = write_image(f, (int64_t)(next - img_start), dest);
            }
            frame_pos = next;

        } else {
            /* ID3v2.3 / v2.4: 4-char ID + 4-byte size + 2-byte flags */
            uint8_t fh[10];
            if (fread(fh, 1, 10, f) != 10 || fh[0] == 0) break;

            uint32_t fsz;
            if (version >= 4) {
                fsz = synchsafe32(fh + 4);
            } else {
                fsz = (uint32_t)fh[4]<<24|(uint32_t)fh[5]<<16|(uint32_t)fh[6]<<8|fh[7];
            }
            long next = frame_pos + 10 + (long)fsz;
            if (next > tag_end || fsz == 0) { frame_pos = next; continue; }

            if (memcmp(fh, "APIC", 4) == 0 && fsz > 4) {
                uint8_t enc;
                if (fread(&enc, 1, 1, f) != 1) break;
                if (!skip_id3_string(f, 0))  break; /* MIME type: always Latin-1 */
                fseek(f, 1, SEEK_CUR);               /* picture type byte */
                if (!skip_id3_string(f, enc)) break; /* description */
                long img_start = ftell(f);
                ok = write_image(f, (int64_t)(next - img_start), dest);
            }
            frame_pos = next;
        }
    }

    fclose(f);
    return ok;
}

/* -------------------------------------------------------------------------
 * FLAC metadata block parser
 * ---------------------------------------------------------------------- */

static int extract_flac_cover(const char *path, const char *dest) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        fclose(f); return 0;
    }

    int ok = 0;
    while (!ok) {
        uint8_t blk[4];
        if (fread(blk, 1, 4, f) != 4) break;
        int      last_block = (blk[0] >> 7) & 1;
        int      block_type = blk[0] & 0x7F;
        uint32_t length     = (uint32_t)blk[1]<<16 | (uint32_t)blk[2]<<8 | blk[3];

        if (block_type == 6) { /* PICTURE */
            uint8_t tmp[4];
            /* picture type (4) */
            if (fread(tmp, 1, 4, f) != 4) break;
            /* MIME type length + string */
            if (fread(tmp, 1, 4, f) != 4) break;
            uint32_t mime_len = (uint32_t)tmp[0]<<24|(uint32_t)tmp[1]<<16|(uint32_t)tmp[2]<<8|tmp[3];
            if (fseek(f, (long)mime_len, SEEK_CUR) != 0) break;
            /* description length + string */
            if (fread(tmp, 1, 4, f) != 4) break;
            uint32_t desc_len = (uint32_t)tmp[0]<<24|(uint32_t)tmp[1]<<16|(uint32_t)tmp[2]<<8|tmp[3];
            /* skip description + width(4) + height(4) + depth(4) + indexed(4) */
            if (fseek(f, (long)desc_len + 16, SEEK_CUR) != 0) break;
            /* image data length */
            if (fread(tmp, 1, 4, f) != 4) break;
            uint32_t img_len = (uint32_t)tmp[0]<<24|(uint32_t)tmp[1]<<16|(uint32_t)tmp[2]<<8|tmp[3];
            ok = write_image(f, (int64_t)img_len, dest);
            break;
        }

        if (fseek(f, (long)length, SEEK_CUR) != 0) break;
        if (last_block) break;
    }

    fclose(f);
    return ok;
}

/* -------------------------------------------------------------------------
 * FFmpeg fallback (OGG, OPUS, AAC, WAV, etc.)
 * ---------------------------------------------------------------------- */

static int extract_ffmpeg_cover(const char *audio_path, const char *dest) {
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return 0;
    fmt->probesize            = 32 * 1024;
    fmt->max_analyze_duration = 0;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) < 0) {
        avformat_free_context(fmt);
        return 0;
    }

    int ok = 0;
    for (unsigned int i = 0; i < fmt->nb_streams && !ok; i++) {
        AVStream *st = fmt->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket *pkt = &st->attached_pic;
            if (pkt->size > 0) {
                FILE *out = fopen(dest, "wb");
                if (out) {
                    fwrite(pkt->data, 1, (size_t)pkt->size, out);
                    fclose(out);
                    ok = 1;
                }
            }
        }
    }

    if (!ok) {
        for (unsigned int i = 0; i < fmt->nb_streams && !ok; i++) {
            enum AVCodecID cid = fmt->streams[i]->codecpar->codec_id;
            if (cid != AV_CODEC_ID_MJPEG && cid != AV_CODEC_ID_PNG) continue;
            AVPacket *pkt = av_packet_alloc();
            if (!pkt) break;
            int packets_read = 0;
            while (av_read_frame(fmt, pkt) >= 0 && !ok && packets_read++ < 8) {
                if ((unsigned)pkt->stream_index == i && pkt->size > 0) {
                    FILE *out = fopen(dest, "wb");
                    if (out) {
                        fwrite(pkt->data, 1, (size_t)pkt->size, out);
                        fclose(out);
                        ok = 1;
                    }
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

/* -------------------------------------------------------------------------
 * Dispatcher
 * ---------------------------------------------------------------------- */

static int extract_to_file(const char *audio_path, const char *dest) {
    const char *dot = strrchr(audio_path, '.');
    if (dot) {
        if (strcasecmp(dot, ".m4b") == 0 || strcasecmp(dot, ".m4a") == 0)
            return extract_m4b_cover(audio_path, dest);
        if (strcasecmp(dot, ".mp3") == 0)
            return extract_mp3_cover(audio_path, dest);
        if (strcasecmp(dot, ".flac") == 0)
            return extract_flac_cover(audio_path, dest);
    }
    return extract_ffmpeg_cover(audio_path, dest);
}

/* -------------------------------------------------------------------------
 * Per-directory processing and main
 * ---------------------------------------------------------------------- */

static void process_dir(const char *dir) {
    char cover_embedded[1280], cover_jpg[1280], cover_png[1280];
    snprintf(cover_embedded, sizeof(cover_embedded), "%s/cover_embedded.jpg", dir);
    snprintf(cover_jpg,      sizeof(cover_jpg),      "%s/cover.jpg",          dir);
    snprintf(cover_png,      sizeof(cover_png),      "%s/cover.png",          dir);
    if (file_exists(cover_embedded) || file_exists(cover_jpg) || file_exists(cover_png))
        return;

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

    if (extract_to_file(audio, cover_embedded))
        fprintf(stderr, "extracted: %s\n", cover_embedded);

    malloc_trim(0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        /* Fork a child per directory: each book starts with a fresh heap.
           FFmpeg moov allocations for large M4B files don't always return
           cleanly to glibc's arena; forking keeps peak RSS to one book. */
        pid_t pid = fork();
        if (pid == 0) {
            process_dir(argv[i]);
            exit(0);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
        } else {
            process_dir(argv[i]); /* fork failed — in-process fallback */
        }
    }
    return 0;
}
