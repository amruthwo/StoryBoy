#pragma once
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>

/* -------------------------------------------------------------------------
 * Cover art pipeline
 *
 * Priority order:
 *   1. Embedded artwork in the audio file (AV_DISPOSITION_ATTACHED_PIC)
 *   2. cover.jpg / cover.png in the same directory
 *   3. Async fetch via fetch_cover binary (Google Books API)
 *      Written as cover.jpg once available; texture reloaded on next call.
 * ---------------------------------------------------------------------- */

/* Load cover art for the given audio file path.
   Checks embedded artwork first, then local files.
   Returns a new SDL_Texture* (caller must SDL_DestroyTexture), or NULL. */
SDL_Texture *cover_load(SDL_Renderer *renderer, const char *audio_path);

/* Load cover art from a local file path (cover.jpg / cover.png).
   Returns a new SDL_Texture* (caller must SDL_DestroyTexture), or NULL. */
SDL_Texture *cover_load_file(SDL_Renderer *renderer, const char *cover_path);

/* Dispatch an async cover fetch for a given book title / author query.
   The fetch_cover binary is launched via posix_spawn (on hardware) or a
   blocking system call (desktop).  The result is written to dest_path.
   Safe to call even if a fetch is already in progress (no-op in that case). */
void cover_fetch_async(const char *title, const char *author,
                       const char *book_dir);

/* Returns 1 if the async fetch has completed and dest_path now exists. */
int  cover_fetch_done(const char *dest_path);

/* Extract embedded artwork from audio_path and write raw bytes to dest_path.
   Returns 1 on success, 0 if no embedded art found or on write error.
   Does not require an SDL renderer — safe to call at scan time. */
int  cover_extract_to_file(const char *audio_path, const char *dest_path);
