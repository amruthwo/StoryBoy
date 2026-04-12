#include "filebrowser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
/* Forward declarations — avoids pulling SDL/curl headers into this file */
extern int  cover_extract_to_file(const char *audio_path, const char *dest_path);
extern void cover_fetch_async(const char *title, const char *author, const char *book_dir);

/* -------------------------------------------------------------------------
 * Supported audio extensions
 * ---------------------------------------------------------------------- */

const char *AUDIO_EXTENSIONS[] = { ".mp3", ".m4b", ".m4a", ".flac", ".ogg", ".opus" };
const int   AUDIO_EXT_COUNT    = 6;

/* -------------------------------------------------------------------------
 * Scan root path
 * ---------------------------------------------------------------------- */

#ifdef SB_TEST_ROOTS
static const char *SCAN_ROOT = "/tmp/storyboy_test/Media/Audiobooks";
#else
static const char *SCAN_ROOT = "/mnt/SDCARD/Media/Audiobooks";
#endif

/* -------------------------------------------------------------------------
 * Natural sort comparator
 * ---------------------------------------------------------------------- */

static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *end_a, *end_b;
            long na = strtol(a, &end_a, 10);
            long nb = strtol(b, &end_b, 10);
            if (na != nb) return (na > nb) ? 1 : -1;
            a = end_a;
            b = end_b;
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int audiofile_cmp(const void *x, const void *y) {
    return natural_cmp(((AudioFile *)x)->name, ((AudioFile *)y)->name);
}
static int mediafolder_cmp(const void *x, const void *y) {
    return natural_cmp(((MediaFolder *)x)->name, ((MediaFolder *)y)->name);
}
static int season_cmp(const void *x, const void *y) {
    return natural_cmp(((Season *)x)->name, ((Season *)y)->name);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int has_audio_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; i < AUDIO_EXT_COUNT; i++) {
        if (strcasecmp(dot, AUDIO_EXTENSIONS[i]) == 0) return 1;
    }
    return 0;
}

static char *path_join(const char *dir, const char *name) {
    size_t len = strlen(dir) + 1 + strlen(name) + 1;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dir, name);
    return p;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Returns 1 if dir contains at least one audio file directly. */
static int has_direct_audio_files(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] == '.') continue;
        if (has_audio_ext(ent->d_name)) found = 1;
    }
    closedir(d);
    return found;
}

/* Returns 1 if dir contains at least one subdirectory. */
static int has_subdirs(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (child && path_is_dir(child)) found = 1;
        free(child);
    }
    closedir(d);
    return found;
}

/* -------------------------------------------------------------------------
 * Cover art lookup
 * ---------------------------------------------------------------------- */

static char *find_cover(const char *dir) {
    char *cjpg = path_join(dir, "cover.jpg");
    char *cpng = path_join(dir, "cover.png");
    if (cjpg && path_exists(cjpg)) { free(cpng); return cjpg; }
    if (cpng && path_exists(cpng)) { free(cjpg); return cpng; }
    free(cjpg); free(cpng);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Growth helpers
 * ---------------------------------------------------------------------- */

static void folder_add_file(MediaFolder *f, const char *dir, const char *name) {
    if (f->file_count == f->file_cap) {
        int newcap = f->file_cap ? f->file_cap * 2 : 8;
        f->files    = realloc(f->files, (size_t)newcap * sizeof(AudioFile));
        f->file_cap = newcap;
    }
    AudioFile *af = &f->files[f->file_count++];
    af->path = path_join(dir, name);
    af->name = strdup(name);
}

static void season_add_file(Season *s, const char *dir, const char *name) {
    if (s->file_count == s->file_cap) {
        int newcap = s->file_cap ? s->file_cap * 2 : 8;
        s->files    = realloc(s->files, (size_t)newcap * sizeof(AudioFile));
        s->file_cap = newcap;
    }
    AudioFile *af = &s->files[s->file_count++];
    af->path = path_join(dir, name);
    af->name = strdup(name);
}

static void show_add_season(MediaFolder *show, Season *s) {
    if (show->season_count == show->season_cap) {
        int newcap = show->season_cap ? show->season_cap * 2 : 4;
        show->seasons    = realloc(show->seasons, (size_t)newcap * sizeof(Season));
        show->season_cap = newcap;
    }
    show->seasons[show->season_count++] = *s;
}

static void library_add_folder(MediaLibrary *lib, MediaFolder *f) {
    if (lib->folder_count == lib->folder_cap) {
        int newcap   = lib->folder_cap ? lib->folder_cap * 2 : 16;
        lib->folders = realloc(lib->folders, (size_t)newcap * sizeof(MediaFolder));
        lib->folder_cap = newcap;
    }
    lib->folders[lib->folder_count++] = *f;
}

/* -------------------------------------------------------------------------
 * Scan
 *
 * Detection rules for audiobook folders:
 *
 *   1. Folder has direct audio files
 *      → flat book (is_series=0). All audio files are chapters/parts.
 *
 *   2. Folder has subdirs with audio files, but no direct audio files
 *      → series (is_series=1). Subdirs are individual books within the
 *        series. Single-book series promoted to flat.
 *
 *   3. Neither → recurse (pass-through container).
 * ---------------------------------------------------------------------- */

static void scan_dir(MediaLibrary *lib, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;

    /* Pass 1: count direct audio files and audio-containing subdirs */
    int direct_audio = 0;
    int audio_subdirs = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (!child) continue;
        if (path_is_dir(child)) {
            if (has_direct_audio_files(child))
                audio_subdirs++;
        } else if (has_audio_ext(ent->d_name)) {
            direct_audio++;
        }
        free(child);
    }

    /* --- Case 1: flat book — has direct audio files --- */
    if (direct_audio > 0) {
        MediaFolder book = {0};
        book.path      = strdup(path);
        const char *sl = strrchr(path, '/');
        book.name      = strdup(sl ? sl + 1 : path);
        book.is_series = 0;

        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(path, ent->d_name);
            if (!child) continue;
            if (!path_is_dir(child) && has_audio_ext(ent->d_name))
                folder_add_file(&book, path, ent->d_name);
            free(child);
        }

        qsort(book.files, (size_t)book.file_count, sizeof(AudioFile), audiofile_cmp);
        book.cover = find_cover(path);
        if (!book.cover && book.file_count > 0) {
#ifndef SB_A30
            /* Try to extract embedded artwork from the first audio file.
               Skipped on SB_A30: opening an AVFormatContext during the scan
               leaves memory fragmented so the subsequent player_open OOMs. */
            char covpath[1280];
            snprintf(covpath, sizeof(covpath), "%s/cover.jpg", path);
            if (cover_extract_to_file(book.files[0].path, covpath))
                book.cover = strdup(covpath);
            else
#endif
            {
                /* No embedded/local art — dispatch async API fetch using folder name */
                const char *sl = strrchr(path, '/');
                cover_fetch_async(sl ? sl + 1 : path, NULL, path);
            }
        }
        library_add_folder(lib, &book);
        closedir(d);
        return;
    }

    /* --- Case 3: no audio anywhere direct — recurse --- */
    if (audio_subdirs == 0) {
        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(path, ent->d_name);
            if (child && path_is_dir(child))
                scan_dir(lib, child);
            free(child);
        }
        closedir(d);
        return;
    }

    /* --- Case 2: series — subdirs contain audio files --- */
    MediaFolder series = {0};
    series.path      = strdup(path);
    const char *sl   = strrchr(path, '/');
    series.name      = strdup(sl ? sl + 1 : path);
    series.is_series = 1;

    rewinddir(d);
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (!child) continue;
        if (path_is_dir(child)) {
            if (has_direct_audio_files(child)) {
                /* Build a book (season) */
                Season book = {0};
                book.path = strdup(child);
                const char *bs = strrchr(child, '/');
                book.name = strdup(bs ? bs + 1 : child);

                DIR *sd = opendir(child);
                if (sd) {
                    struct dirent *se;
                    while ((se = readdir(sd)) != NULL) {
                        if (se->d_name[0] == '.') continue;
                        char *sc = path_join(child, se->d_name);
                        if (sc && !path_is_dir(sc) && has_audio_ext(se->d_name))
                            season_add_file(&book, child, se->d_name);
                        free(sc);
                    }
                    closedir(sd);
                }

                if (book.file_count > 0) {
                    qsort(book.files, (size_t)book.file_count,
                          sizeof(AudioFile), audiofile_cmp);
                    book.cover = find_cover(child);
                    if (!book.cover) {
#ifndef SB_A30
                        /* Skipped on SB_A30: same OOM risk as above */
                        char covpath[1280];
                        snprintf(covpath, sizeof(covpath), "%s/cover.jpg", child);
                        if (cover_extract_to_file(book.files[0].path, covpath))
                            book.cover = strdup(covpath);
                        else
#endif
                        {
                            const char *bs = strrchr(child, '/');
                            cover_fetch_async(bs ? bs + 1 : child, NULL, child);
                        }
                    }
                    show_add_season(&series, &book);
                } else {
                    free(book.path); free(book.name); free(book.files);
                }
            } else if (has_subdirs(child)) {
                /* Sub-subdir: recurse rather than nesting further */
                scan_dir(lib, child);
            }
        }
        free(child);
    }
    closedir(d);

    if (series.season_count == 0) {
        free(series.path); free(series.name);
        return;
    }

    qsort(series.seasons, (size_t)series.season_count, sizeof(Season), season_cmp);
    series.cover = find_cover(path);

    /* Single-book series promotion: treat as flat book */
    if (series.season_count == 1) {
        Season *only = &series.seasons[0];
        series.is_series  = 0;
        series.files      = only->files;      only->files = NULL;
        series.file_count = only->file_count;
        series.file_cap   = only->file_cap;
        if (!series.cover && only->cover) {
            series.cover = only->cover; only->cover = NULL;
        }
        free(only->path); free(only->name); free(only->cover);
        free(series.seasons);
        series.seasons      = NULL;
        series.season_count = 0;
        series.season_cap   = 0;
    }

    library_add_folder(lib, &series);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void library_scan(MediaLibrary *lib) {
    memset(lib, 0, sizeof(*lib));
    /* Scan each child of SCAN_ROOT directly so individual books/authors
       appear at the top level rather than being collapsed into one series. */
    DIR *d = opendir(SCAN_ROOT);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(SCAN_ROOT, ent->d_name);
            if (child && path_is_dir(child))
                scan_dir(lib, child);
            free(child);
        }
        closedir(d);
    }
    if (lib->folder_count > 1)
        qsort(lib->folders, (size_t)lib->folder_count,
              sizeof(MediaFolder), mediafolder_cmp);
}

void library_free(MediaLibrary *lib) {
    for (int i = 0; i < lib->folder_count; i++) {
        MediaFolder *f = &lib->folders[i];
        if (f->is_series) {
            for (int si = 0; si < f->season_count; si++) {
                Season *s = &f->seasons[si];
                for (int j = 0; j < s->file_count; j++) {
                    free(s->files[j].path);
                    free(s->files[j].name);
                }
                free(s->files);
                free(s->path);
                free(s->name);
                free(s->cover);
            }
            free(f->seasons);
        } else {
            for (int j = 0; j < f->file_count; j++) {
                free(f->files[j].path);
                free(f->files[j].name);
            }
            free(f->files);
        }
        free(f->path);
        free(f->name);
        free(f->cover);
    }
    free(lib->folders);
    memset(lib, 0, sizeof(*lib));
}
