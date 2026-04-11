/* fetch_cover.c — queries Open Library search API (no API key required) and
 * downloads a cover thumbnail into a book directory.
 *
 * Step 1: GET https://openlibrary.org/search.json?title=<title>&limit=5
 * Step 2: Find first result that has a cover_i (numeric cover ID)
 * Step 3: GET https://covers.openlibrary.org/b/id/<cover_i>-L.jpg
 *
 * Usage: fetch_cover <book_dir> <title> [author]
 *
 *   book_dir — directory to write cover.jpg into
 *   title    — book title (URL-encoded by this program)
 *   author   — optional author name
 *
 * Exit codes: 0 = cover.jpg written, 1 = not found / error.
 *
 * Compile dependencies: libcurl (static on device builds).
 * No SDL2 / FFmpeg dependency — intentionally minimal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

/* -------------------------------------------------------------------------
 * Dynamic write buffer for curl
 * ---------------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    Buf *b = (Buf *)userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 4096;
        while (newcap < b->len + n + 1) newcap *= 2;
        char *tmp = realloc(b->data, newcap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static size_t file_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

/* -------------------------------------------------------------------------
 * URL encoding
 * ---------------------------------------------------------------------- */

static void url_encode(const char *src, char *dst, size_t dstsz) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 4 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            di += (size_t)snprintf(dst + di, dstsz - di, "%%%02X", c);
        }
    }
    dst[di] = '\0';
}

/* -------------------------------------------------------------------------
 * JSON helpers — minimal, no external dependency
 * ---------------------------------------------------------------------- */

/* Find the first integer value for a key: "key": 12345 → returns 12345.
   Returns 0 if not found or value is non-positive. */
static long json_int(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p < '0' || *p > '9') return 0;
    return atol(p);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: fetch_cover <book_dir> <title> [author]\n");
        return 1;
    }
    const char *book_dir = argv[1];
    const char *title    = argv[2];
    const char *author   = (argc >= 4) ? argv[3] : NULL;

    /* Build Open Library search URL.
       Folder names are often "Author - Title" — search by title field which
       handles the extra author prefix gracefully. */
    char title_enc[512];
    url_encode(title, title_enc, sizeof(title_enc));

    char query[1024];
    if (author && author[0]) {
        char author_enc[256];
        url_encode(author, author_enc, sizeof(author_enc));
        snprintf(query, sizeof(query),
                 "https://openlibrary.org/search.json"
                 "?title=%s&author=%s&limit=5&fields=cover_i",
                 title_enc, author_enc);
    } else {
        snprintf(query, sizeof(query),
                 "https://openlibrary.org/search.json"
                 "?title=%s&limit=5&fields=cover_i",
                 title_enc);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) { curl_global_cleanup(); return 1; }

    /* ---- Step 1: fetch search JSON ---- */
    Buf json_buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, query);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "StoryBoy/1.0 (audiobook player)");
    /* Embedded devices lack a system CA bundle — skip peer verification.
       fetch_cover only downloads cover art (no credentials), so this is safe. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK || !json_buf.data) {
        fprintf(stderr, "fetch_cover: curl error: %s\n", curl_easy_strerror(res));
        free(json_buf.data);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }

    /* ---- Step 2: extract cover_i (numeric cover ID) ---- */
    long cover_id = json_int(json_buf.data, "cover_i");
    free(json_buf.data);

    if (cover_id <= 0) {
        fprintf(stderr, "fetch_cover: no cover found for \"%s\"\n", title);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }

    /* ---- Step 3: download cover image ---- */
    /* -L = large (typically 400px wide), good quality for the browser grid */
    char cover_url[256];
    snprintf(cover_url, sizeof(cover_url),
             "https://covers.openlibrary.org/b/id/%ld-L.jpg", cover_id);

    char dest[1280];
    snprintf(dest, sizeof(dest), "%s/cover.jpg", book_dir);

    FILE *fp = fopen(dest, "wb");
    if (!fp) {
        fprintf(stderr, "fetch_cover: cannot write %s\n", dest);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, cover_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        fprintf(stderr, "fetch_cover: download error: %s\n", curl_easy_strerror(res));
        unlink(dest);
        return 1;
    }

    /* Sanity check: reject suspiciously small files (error pages, etc.) */
    FILE *check = fopen(dest, "rb");
    if (check) {
        fseek(check, 0, SEEK_END);
        long sz = ftell(check);
        fclose(check);
        if (sz < 1024) {
            fprintf(stderr, "fetch_cover: downloaded file too small (%ld bytes)\n", sz);
            unlink(dest);
            return 1;
        }
    }

    fprintf(stderr, "fetch_cover: saved %s\n", dest);
    return 0;
}
