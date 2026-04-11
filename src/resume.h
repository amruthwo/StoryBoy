#pragma once

/* Per-file and per-book resume positions, persisted to a flat text file.
 *
 * Two entry formats coexist in resume.dat:
 *
 *   Single-file:  path\tposition\tduration\n          (3 fields)
 *   Multi-file:   folder\tfile_idx\tpos_in_file\ttotal_dur\tbook_pos\n  (5 fields)
 *
 * Field count distinguishes them.  Old 2-field lines (no duration) are also
 * accepted.  Entries are appended on update so the last line = most recent.
 *
 * Single-file API:
 *   resume_save / resume_load / resume_clear
 *
 * Multi-file book API:
 *   resume_save_book / resume_load_book / resume_clear_book
 *
 * Shared:
 *   resume_load_all        — all in-progress entries, most-recent first
 *   resume_record_completed / resume_load_completed — history.dat
 *   resume_clear_all       — truncate both files
 */

typedef struct {
    char   path[1024];   /* file path (single) or folder path (book) */
    double position;     /* pos_in_file (single) or pos_in_file (book) */
    double duration;     /* file duration (single) or total book dur (book) */
    /* Book-only fields (file_idx == -1 means single-file entry) */
    int    file_idx;     /* which file within the book */
    double book_pos;     /* absolute position from start of book */
} ResumeEntry;

/* ---- Single-file API ---- */
void   resume_save  (const char *path, double pos_sec, double duration_sec);
double resume_load  (const char *path);
void   resume_clear (const char *path);

/* ---- Multi-file book API ---- */
/* Save book position.
   folder        — directory containing the audio files
   file_idx      — 0-based index of current file
   pos_in_file   — seconds into current file
   total_book_dur— sum of all file durations (0 if not yet known)
   book_pos      — absolute seconds from book start (= sum of prev file durs + pos) */
void resume_save_book(const char *folder, int file_idx, double pos_in_file,
                      double total_book_dur, double book_pos);

/* Load book position.  Returns 1 if found, 0 if no entry.
   Fills *file_idx_out, *pos_out, *total_dur_out, *book_pos_out (may be NULL). */
int  resume_load_book(const char *folder, int *file_idx_out, double *pos_out,
                      double *total_dur_out, double *book_pos_out);

void resume_clear_book(const char *folder);

/* ---- Shared ---- */
/* Load all in-progress entries, most-recent first.
   Returns count; caller must free(*out). */
int  resume_load_all(ResumeEntry **out);

/* Record a completed book/file in history.dat (deduped, capped at 64).
   Pass the folder path for multi-file books, file path for single files. */
void resume_record_completed(const char *path);

/* Load recently-completed paths, most-recent first.
   Returns count; caller must free(*out) (heap array of char[1024]). */
int  resume_load_completed(char (**out)[1024]);

/* Remove a single path from history.dat (no-op if not found). */
void resume_remove_completed(const char *path);

/* Erase all resume and history data (truncates both .dat files). */
void resume_clear_all(void);
