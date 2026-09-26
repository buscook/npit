#include "npit_internal.h"

void clear_lyrics(void) {
    for (int i = 0; i < lyric_count; i++) free(lyrics[i].text);
    lyric_count = 0;
    lyric_generation++;
}

static void add_lyric(LyricBatch *batch, double stamp, const char *line) {
    if (batch->count >= MAX_LYRICS) return;
    char *copy = strdup(line);
    if (!copy) return;
    trim(copy);
    if (!copy[0]) { free(copy); return; }
    batch->lines[batch->count].timestamp = stamp;
    batch->lines[batch->count].text = copy;
    batch->count++;
}

static void parse_lrc_text(LyricBatch *batch, const char *text) {
    char *copy = strdup(text ? text : "");
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line && batch->count < MAX_LYRICS; line = strtok_r(NULL, "\n", &save)) {
        trim(line);
        char *text_start = line;
        double timestamp = -1;
        while (*text_start == '[') {
            char *close = strchr(text_start, ']');
            if (!close) break;
            int minutes = 0;
            double seconds = 0;
            if (sscanf(text_start + 1, "%d:%lf", &minutes, &seconds) == 2) timestamp = minutes * 60.0 + seconds;
            text_start = close + 1;
        }
        if (timestamp >= 0) add_lyric(batch, timestamp, text_start);
    }
    free(copy);
}

static void load_local_lrc(const Song *song, LyricBatch *batch) {
    if (strncmp(song->media_url, "file://", 7)) return;
    int path_length = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return;
    char *path = curl_easy_unescape(curl, song->media_url + 7, 0, &path_length);
    curl_easy_cleanup(curl);
    if (!path) return;
    char *dot = strrchr(path, '.');
    if (dot) *dot = 0;
    char lrc_path[MAX_FIELD];
    snprintf(lrc_path, sizeof(lrc_path), "%s.lrc", path);
    free(path);
    FILE *file = fopen(lrc_path, "r");
    if (!file) return;
    char line[4096];
    size_t capacity = 4096, length = 0;
    char *contents = malloc(capacity);
    if (!contents) { fclose(file); return; }
    while (fgets(line, sizeof(line), file)) {
        size_t n = strlen(line);
        if (length + n + 1 > capacity) {
            capacity *= 2;
            char *next = realloc(contents, capacity);
            if (!next) { free(contents); fclose(file); return; }
            contents = next;
        }
        memcpy(contents + length, line, n);
        length += n;
    }
    fclose(file);
    contents[length] = 0;
    parse_lrc_text(batch, contents);
    free(contents);
}

static void *lyrics_worker(void *arg) {
    double started_at = profile_enabled ? monotonic_seconds() : 0.0;
    LyricRequest request = *(LyricRequest *)arg;
    free(arg);
    Song song = request.song;
    LyricBatch batch = {0};
    load_local_lrc(&song, &batch);
    if (!batch.count && song.title[0]) {
        CURL *curl = curl_easy_init();
        if (curl) {
            char *title = curl_easy_escape(curl, song.title, 0);
            char *artist = curl_easy_escape(curl, song.artist, 0);
            char *album = curl_easy_escape(curl, song.album, 0);
            char url[8192];
            snprintf(url, sizeof(url), "https://lrclib.net/api/get?track_name=%s&artist_name=%s&album_name=%s&duration=%d", title ? title : "", artist ? artist : "", album ? album : "", (int)song.length);
            if (title) curl_free(title);
            if (artist) curl_free(artist);
            if (album) curl_free(album);
            curl_easy_cleanup(curl);
            Buffer body = http_get(url, NULL);
            if (body.data) {
                struct json_object *payload = json_tokener_parse((char *)body.data);
                struct json_object *synced = NULL;
                if (payload && json_object_object_get_ex(payload, "syncedLyrics", &synced) && json_object_get_type(synced) == json_type_string) {
                    parse_lrc_text(&batch, json_object_get_string(synced));
                }
                if (payload) json_object_put(payload);
            }
            free(body.data);
        }
    }
    pthread_mutex_lock(&lyric_mutex);
    char identity[MAX_FIELD];
    bool updated = false;
    snprintf(identity, sizeof(identity), "%.680s\x1f%.680s\x1f%.680s", song.title, song.artist, song.album);
    if (request.generation == lyric_request_generation && !strcmp(lyric_identity, identity)) {
        clear_lyrics();
        lyric_count = batch.count;
        for (int i = 0; i < batch.count; i++) {
            lyrics[i] = batch.lines[i];
            batch.lines[i].text = NULL;
        }
        lyrics_loaded = true;
        lyrics_pending = false;
        updated = true;
    }
    pthread_mutex_unlock(&lyric_mutex);
    if (updated && lyric_event_fd >= 0) {
        uint64_t signal = 1;
        (void)write(lyric_event_fd, &signal, sizeof(signal));
    }
    for (int i = 0; i < batch.count; i++) free(batch.lines[i].text);
    record_timing(&lyrics_timing, started_at);
    release_worker();
    return NULL;
}

void request_lyrics(const Song *song) {
    if (!cfg.lyrics || !song->title[0]) return;
    char identity[MAX_FIELD];
    snprintf(identity, sizeof(identity), "%.680s\x1f%.680s\x1f%.680s", song->title, song->artist, song->album);
    LyricRequest *copy = malloc(sizeof(*copy));
    if (!copy) return;
    copy->song = *song;
    pthread_mutex_lock(&lyric_mutex);
    if (!strcmp(lyric_identity, identity)) { pthread_mutex_unlock(&lyric_mutex); free(copy); return; }
    set_string(lyric_identity, sizeof(lyric_identity), identity);
    copy->generation = ++lyric_request_generation;
    clear_lyrics();
    lyrics_pending = true;
    lyrics_loaded = false;
    pthread_mutex_unlock(&lyric_mutex);
    pthread_t thread;
    if (!reserve_worker()) { free(copy); return; }
    if (pthread_create(&thread, NULL, lyrics_worker, copy) == 0) pthread_detach(thread);
    else {
        release_worker();
        unsigned long generation = copy->generation;
        free(copy);
        pthread_mutex_lock(&lyric_mutex);
        if (generation == lyric_request_generation && !strcmp(lyric_identity, identity)) {
            lyric_identity[0] = 0;
            lyrics_pending = false;
        }
        pthread_mutex_unlock(&lyric_mutex);
    }
}

double next_lyric_delay(double position) {
    double delay = INFINITY;
    pthread_mutex_lock(&lyric_mutex);
    if (lyrics_loaded) {
        for (int i = 0; i < lyric_count; i++) {
            double remaining = lyrics[i].timestamp - position;
            if (remaining > 0.0 && remaining < delay) delay = remaining;
        }
    }
    pthread_mutex_unlock(&lyric_mutex);
    return delay;
}

