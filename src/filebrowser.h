#pragma once
#include <stddef.h>

/* Supported audio file extensions */
extern const char *AUDIO_EXTENSIONS[];
extern const int   AUDIO_EXT_COUNT;

typedef struct {
    char *path;   /* full absolute path */
    char *name;   /* basename only, for display */
} AudioFile;

/* A book sub-directory inside a series container */
typedef struct {
    char      *path;
    char      *name;        /* e.g. "Book 1" */
    char      *cover;       /* cover.jpg/png, or NULL */
    AudioFile *files;
    int        file_count;
    int        file_cap;
} Season;

typedef struct {
    char      *path;
    char      *name;
    char      *cover;

    /* is_series == 0: flat book (files/file_count/file_cap used) */
    /* is_series == 1: series container (seasons/season_count/season_cap used) */
    int        is_series;

    AudioFile *files;
    int        file_count;
    int        file_cap;

    Season    *seasons;
    int        season_count;
    int        season_cap;
} MediaFolder;

typedef struct {
    MediaFolder *folders;
    int          folder_count;
    int          folder_cap;
} MediaLibrary;

/* Scan the audiobook root path and populate lib.
   Caller must call library_free() when done. */
void library_scan(MediaLibrary *lib);

/* Free all memory owned by lib */
void library_free(MediaLibrary *lib);
