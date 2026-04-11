#include "resume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef SB_TEST_ROOTS
#  define RESUME_FILE  "/tmp/storyboy_test/resume.dat"
#  define HISTORY_FILE "/tmp/storyboy_test/history.dat"
#else
#  define RESUME_FILE  "/mnt/SDCARD/Saves/CurrentProfile/states/StoryBoy/resume.dat"
#  define HISTORY_FILE "/mnt/SDCARD/Saves/CurrentProfile/states/StoryBoy/history.dat"
#endif

#define MAX_ENTRIES  512
#define HISTORY_MAX   64
/* path (1023) + \t + pos + \t + dur + \n */
#define MAX_LINE     1120

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void mkdir_p(const char *path) {
    char tmp[MAX_LINE];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void ensure_dir(const char *file) {
    char dir[MAX_LINE];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }
}

/* Parse one line from resume.dat into a ResumeEntry.
   Handles both single-file (2-3 fields) and book (5 fields) formats.
   Returns 1 on success, 0 if the line is malformed. */
static int parse_entry(char *line, ResumeEntry *out) {
    /* Strip trailing newline */
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    /* Split on tabs — expect 2-5 fields */
    char *fields[5];
    int   nf = 0;
    fields[nf++] = line;
    for (char *p = line; *p && nf < 5; p++) {
        if (*p == '\t') { *p = '\0'; fields[nf++] = p + 1; }
    }
    if (nf < 2) return 0;

    strncpy(out->path, fields[0], 1023); out->path[1023] = '\0';
    out->file_idx = -1;
    out->book_pos = 0.0;

    if (nf >= 5) {
        /* Book entry: folder\tfile_idx\tpos_in_file\ttotal_dur\tbook_pos */
        out->file_idx = atoi(fields[1]);
        out->position = atof(fields[2]);
        out->duration = atof(fields[3]);
        double bp     = atof(fields[4]);
        /* Clamp corrupted book_pos: must be finite, non-negative, and within
           a reasonable audiobook length (< 1e6 s ≈ 278 h).  Corrupt values
           (NaN, Inf, or astronomically large) are zeroed so history falls
           back to the per-file position instead. */
        out->book_pos = (bp >= 0.0 && bp < 1e6 && bp == bp) ? bp : 0.0;
    } else {
        /* Single-file entry: path\tposition[\tduration] */
        out->position = atof(fields[1]);
        out->duration = (nf >= 3) ? atof(fields[2]) : 0.0;
    }
    return 1;
}

/* Write one entry to an open FILE* */
static void write_entry(FILE *f, const ResumeEntry *e) {
    if (e->file_idx >= 0) {
        /* Book entry */
        fprintf(f, "%s\t%d\t%f\t%f\t%f\n",
                e->path, e->file_idx, e->position, e->duration, e->book_pos);
    } else {
        /* Single-file entry */
        fprintf(f, "%s\t%f\t%f\n", e->path, e->position, e->duration);
    }
}

/* -------------------------------------------------------------------------
 * resume_load — return saved position for path, or -1.0
 * ---------------------------------------------------------------------- */

double resume_load(const char *path) {
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return -1.0;

    char line[MAX_LINE];
    double result = -1.0;
    while (fgets(line, sizeof(line), f)) {
        ResumeEntry e;
        if (!parse_entry(line, &e)) continue;
        if (strcmp(e.path, path) == 0) { result = e.position; break; }
    }
    fclose(f);
    return result;
}

/* -------------------------------------------------------------------------
 * resume_save — write/update position; moves entry to end of file
 * ---------------------------------------------------------------------- */

void resume_save(const char *path, double pos_sec, double duration_sec) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) return;

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            ResumeEntry e;
            if (!parse_entry(line, &e)) continue;
            /* Skip the existing entry for this path (will re-add at end) */
            if (strcmp(e.path, path) == 0) continue;
            entries[count++] = e;
        }
        fclose(f);
    }

    /* Append the updated entry at the end (most recently active = last) */
    if (count < MAX_ENTRIES) {
        strncpy(entries[count].path, path, 1023);
        entries[count].path[1023] = '\0';
        entries[count].position = pos_sec;
        entries[count].duration = (duration_sec > 0.0) ? duration_sec : 0.0;
        count++;
    }

    ensure_dir(RESUME_FILE);
    f = fopen(RESUME_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++) write_entry(f, &entries[i]);
        fclose(f);
    }
    free(entries);
}

/* -------------------------------------------------------------------------
 * resume_clear — remove entry for path
 * ---------------------------------------------------------------------- */

void resume_clear(const char *path) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) return;

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) { free(entries); return; }

    char line[MAX_LINE];
    while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        ResumeEntry e;
        if (!parse_entry(line, &e)) continue;
        if (strcmp(e.path, path) != 0)
            entries[count++] = e;
    }
    fclose(f);

    f = fopen(RESUME_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++) write_entry(f, &entries[i]);
        fclose(f);
    }
    free(entries);
}

/* -------------------------------------------------------------------------
 * Book API — multi-file folder books
 * ---------------------------------------------------------------------- */

void resume_save_book(const char *folder, int file_idx, double pos_in_file,
                      double total_book_dur, double book_pos) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) return;

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            ResumeEntry e;
            if (!parse_entry(line, &e)) continue;
            if (strcmp(e.path, folder) == 0) continue; /* will re-add at end */
            entries[count++] = e;
        }
        fclose(f);
    }

    if (count < MAX_ENTRIES) {
        strncpy(entries[count].path, folder, 1023);
        entries[count].path[1023] = '\0';
        entries[count].file_idx = file_idx;
        entries[count].position = pos_in_file;
        entries[count].duration = (total_book_dur > 0.0) ? total_book_dur : 0.0;
        entries[count].book_pos = book_pos;
        count++;
    }

    ensure_dir(RESUME_FILE);
    f = fopen(RESUME_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++) write_entry(f, &entries[i]);
        fclose(f);
    }
    free(entries);
}

int resume_load_book(const char *folder, int *file_idx_out, double *pos_out,
                     double *total_dur_out, double *book_pos_out) {
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0;

    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        ResumeEntry e;
        if (!parse_entry(line, &e)) continue;
        if (e.file_idx < 0) continue;   /* skip single-file entries */
        if (strcmp(e.path, folder) != 0) continue;
        if (file_idx_out)  *file_idx_out  = e.file_idx;
        if (pos_out)       *pos_out       = e.position;
        if (total_dur_out) *total_dur_out = e.duration;
        if (book_pos_out)  *book_pos_out  = e.book_pos;
        found = 1;
    }
    fclose(f);
    return found;
}

void resume_clear_book(const char *folder) {
    resume_clear(folder); /* same logic — just removes by path key */
}

/* -------------------------------------------------------------------------
 * resume_load_all — load all entries, most-recent first
 * ---------------------------------------------------------------------- */

int resume_load_all(ResumeEntry **out) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) { *out = NULL; return 0; }

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            ResumeEntry e;
            if (parse_entry(line, &e))
                entries[count++] = e;
        }
        fclose(f);
    }

    /* Filter: remove per-file entries whose parent folder has a book entry.
       This suppresses old per-file chapter entries once a book-level entry
       exists for that folder (e.g. after saving progress mid-book). */
    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].file_idx >= 0) {
            /* Book entry — always keep */
            entries[new_count++] = entries[i];
            continue;
        }
        /* Single-file entry — skip if its parent dir matches any book entry */
        char dir[1024];
        strncpy(dir, entries[i].path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *sl = strrchr(dir, '/');
        if (sl) *sl = '\0';
        int in_book = 0;
        for (int j = 0; j < count; j++) {
            if (entries[j].file_idx >= 0 &&
                strcmp(dir, entries[j].path) == 0) {
                in_book = 1;
                break;
            }
        }
        if (!in_book)
            entries[new_count++] = entries[i];
    }
    count = new_count;

    /* Reverse in-place: last entry in file = most recently used = first result */
    for (int i = 0, j = count - 1; i < j; i++, j--) {
        ResumeEntry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
    }

    *out = entries;
    return count;
}

/* -------------------------------------------------------------------------
 * resume_record_completed — append path to history.dat (deduped, capped)
 * ---------------------------------------------------------------------- */

void resume_record_completed(const char *path) {
    char (*paths)[1024] = malloc(HISTORY_MAX * 1024);
    if (!paths) return;

    int count = 0;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
            /* Strip newline */
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;
            /* Skip duplicate */
            if (strcmp(line, path) == 0) continue;
            strncpy(paths[count], line, 1023); paths[count][1023] = '\0';
            count++;
        }
        fclose(f);
    }

    /* If at capacity, drop oldest entries to make room */
    if (count >= HISTORY_MAX) {
        /* Shift entries: drop oldest (index 0), keep [1..count-1] */
        memmove(paths[0], paths[1], (size_t)(count - 1) * 1024);
        count--;
    }

    /* Append new entry at end */
    strncpy(paths[count], path, 1023); paths[count][1023] = '\0';
    count++;

    ensure_dir(HISTORY_FILE);
    f = fopen(HISTORY_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%s\n", paths[i]);
        fclose(f);
    }
    free(paths);
}

/* -------------------------------------------------------------------------
 * resume_load_completed — load recently-completed paths, most-recent first
 * ---------------------------------------------------------------------- */

int resume_load_completed(char (**out)[1024]) {
    char (*paths)[1024] = malloc(HISTORY_MAX * 1024);
    if (!paths) { *out = NULL; return 0; }

    int count = 0;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;
            strncpy(paths[count], line, 1023); paths[count][1023] = '\0';
            count++;
        }
        fclose(f);
    }

    /* Reverse: last line = most recently completed = first result */
    for (int i = 0, j = count - 1; i < j; i++, j--) {
        char tmp[1024];
        memcpy(tmp, paths[i], 1024);
        memcpy(paths[i], paths[j], 1024);
        memcpy(paths[j], tmp, 1024);
    }

    *out = paths;
    return count;
}

void resume_remove_completed(const char *path) {
    char (*paths)[1024] = malloc(HISTORY_MAX * 1024);
    if (!paths) return;

    int count = 0;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) { free(paths); return; }

    char line[MAX_LINE];
    while (count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, path) == 0) continue; /* skip target */
        strncpy(paths[count], line, 1023); paths[count][1023] = '\0';
        count++;
    }
    fclose(f);

    f = fopen(HISTORY_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%s\n", paths[i]);
        fclose(f);
    }
    free(paths);
}

void resume_clear_all(void) {
    FILE *f;
    f = fopen(RESUME_FILE,  "w"); if (f) fclose(f);
    f = fopen(HISTORY_FILE, "w"); if (f) fclose(f);
}
