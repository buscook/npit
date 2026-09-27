#include "npit_internal.h"
#include <vlc/vlc.h>
#include <dirent.h>

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 180
#define VIDEO_COLOR_CACHE_SLOTS 32

typedef struct {
    char *path;
    char *name;
    bool video;
} LocalEntry;

static LocalEntry *entries;
static size_t entry_count;
static size_t entry_capacity;
static size_t current_entry;
static libvlc_instance_t *vlc_instance;
static libvlc_media_player_t *vlc_player;
static atomic_bool advance_pending;
static atomic_bool playback_error_pending;
static atomic_bool metadata_pending;
static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char video_staging[VIDEO_WIDTH * VIDEO_HEIGHT * 4];
static unsigned char video_pixels[VIDEO_WIDTH * VIDEO_HEIGHT * 4];
static unsigned long video_generation;
static double entry_started_at;
static double last_position_at;
static libvlc_time_t last_position;
static bool repeat_folder;
static int requested_volume = 100;
static bool volume_confirmed;
static double next_volume_check;
static atomic_ulong color_generation;
static pthread_mutex_t color_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool color_ready;
static int color_red, color_green, color_blue;
static unsigned long color_cache_clock;

typedef struct {
    char *path;
    off_t size;
    struct timespec modified;
    unsigned long used;
    int red, green, blue;
} VideoColorCacheEntry;

static VideoColorCacheEntry color_cache[VIDEO_COLOR_CACHE_SLOTS];

typedef struct {
    char *path;
    off_t size;
    struct timespec modified;
    unsigned long generation;
} ColorJob;

static bool color_cache_match(const VideoColorCacheEntry *entry, const char *path, const struct stat *details) {
    return entry->path && !strcmp(entry->path, path) && entry->size == details->st_size && entry->modified.tv_sec == details->st_mtim.tv_sec && entry->modified.tv_nsec == details->st_mtim.tv_nsec;
}

static void save_video_color(const ColorJob *job, int red, int green, int blue) {
    size_t slot = 0;
    for (size_t index = 0; index < VIDEO_COLOR_CACHE_SLOTS; index++) {
        if (!color_cache[index].path) { slot = index; break; }
        if (color_cache[index].used < color_cache[slot].used) slot = index;
    }
    char *path = strdup(job->path);
    if (!path) return;
    free(color_cache[slot].path);
    color_cache[slot] = (VideoColorCacheEntry){.path = path, .size = job->size, .modified = job->modified, .used = ++color_cache_clock, .red = red, .green = green, .blue = blue};
}

static size_t capture_process(char *const arguments[], unsigned char *output, size_t capacity) {
    int ends[2];
    if (pipe(ends)) return 0;
    pid_t child = fork();
    if (child < 0) { close(ends[0]); close(ends[1]); return 0; }
    if (!child) {
        close(ends[0]);
        dup2(ends[1], STDOUT_FILENO);
        close(ends[1]);
        int quiet = open("/dev/null", O_WRONLY);
        if (quiet >= 0) { dup2(quiet, STDERR_FILENO); close(quiet); }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(ends[1]);
    size_t length = 0;
    while (length < capacity) {
        ssize_t amount = read(ends[0], output + length, capacity - length);
        if (amount <= 0) break;
        length += (size_t)amount;
    }
    close(ends[0]);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return length;
}

static void *scan_video_color(void *data) {
    ColorJob *job = data;
    unsigned char duration_text[128] = {0};
    char *probe[] = {"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", job->path, NULL};
    size_t duration_bytes = capture_process(probe, duration_text, sizeof(duration_text) - 1);
    double duration = duration_bytes ? strtod((char *)duration_text, NULL) : 0;
    if (duration > 0 && isfinite(duration)) {
        unsigned char frame[32 * 18 * 3];
        unsigned long long sums[3] = {0};
        unsigned long long count = 0;
        for (int sample = 0; sample < 16 && atomic_load(&color_generation) == job->generation; sample++) {
            char position[40];
            snprintf(position, sizeof(position), "%.3f", duration * (sample + 0.5) / 16.0);
            char *command[] = {"ffmpeg", "-nostdin", "-v", "error", "-ss", position, "-i", job->path, "-frames:v", "1", "-vf", "scale=32:18", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1", NULL};
            if (capture_process(command, frame, sizeof(frame)) != sizeof(frame)) continue;
            for (size_t pixel = 0; pixel < sizeof(frame); pixel += 3) {
                sums[0] += frame[pixel];
                sums[1] += frame[pixel + 1];
                sums[2] += frame[pixel + 2];
                count++;
            }
        }
        if (count && atomic_load(&color_generation) == job->generation) {
            int r = (int)(sums[0] / count), g = (int)(sums[1] / count), b = (int)(sums[2] / count);
            int brightness = (r * 3 + g * 6 + b) / 10;
            if (brightness < 8) r = g = b = 120;
            else if (brightness < 120) {
                double factor = 120.0 / brightness;
                r = (int)fmin(255, r * factor);
                g = (int)fmin(255, g * factor);
                b = (int)fmin(255, b * factor);
            }
            pthread_mutex_lock(&color_mutex);
            if (atomic_load(&color_generation) == job->generation) {
                save_video_color(job, r, g, b);
                color_red = r; color_green = g; color_blue = b;
                color_ready = true;
            }
            pthread_mutex_unlock(&color_mutex);
        }
    }
    free(job->path);
    free(job);
    return NULL;
}

static void begin_video_color(const char *path) {
    unsigned long generation = atomic_fetch_add(&color_generation, 1) + 1;
    struct stat details;
    bool valid = path && !stat(path, &details);
    pthread_mutex_lock(&color_mutex);
    color_ready = false;
    if (valid) {
        for (size_t index = 0; index < VIDEO_COLOR_CACHE_SLOTS; index++) {
            if (!color_cache_match(&color_cache[index], path, &details)) continue;
            color_cache[index].used = ++color_cache_clock;
            color_red = color_cache[index].red;
            color_green = color_cache[index].green;
            color_blue = color_cache[index].blue;
            color_ready = true;
            break;
        }
    }
    bool cached = color_ready;
    pthread_mutex_unlock(&color_mutex);
    if (!valid || cached) return;
    ColorJob *job = calloc(1, sizeof(*job));
    if (!job) return;
    job->path = strdup(path);
    job->size = details.st_size;
    job->modified = details.st_mtim;
    job->generation = generation;
    if (!job->path) { free(job); return; }
    pthread_t worker;
    if (!pthread_create(&worker, NULL, scan_video_color, job)) pthread_detach(worker);
    else { free(job->path); free(job); }
}

static bool media_extension(const char *name, bool *video) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *audio[] = {"mp3", "flac", "wav", "wave", "m4a", "aac", "ogg", "oga", "opus", "wma", "aif", "aiff", "ape", "wv", "tta", "alac", "dsf", "dff", "mka", "mid", "midi"};
    static const char *movies[] = {"mp4", "mkv", "webm", "avi", "mov", "m4v", "mpg", "mpeg", "ts", "m2ts", "wmv", "flv", "ogv", "3gp"};
    for (size_t i = 0; i < sizeof(audio) / sizeof(*audio); i++) if (!strcasecmp(dot + 1, audio[i])) { *video = false; return true; }
    for (size_t i = 0; i < sizeof(movies) / sizeof(*movies); i++) if (!strcasecmp(dot + 1, movies[i])) { *video = true; return true; }
    return false;
}

static void entry_title(size_t index, char *target, size_t size) {
    set_string(target, size, entries[index].name);
    char *dot = strrchr(target, '.');
    if (dot) *dot = 0;
}

static void update_next(void) {
    pthread_mutex_lock(&queue_mutex);
    next_track[0] = 0;
    if (current_entry + 1 < entry_count) entry_title(current_entry + 1, next_track, sizeof(next_track));
    else if (repeat_folder && entry_count > 1) entry_title(0, next_track, sizeof(next_track));
    pthread_mutex_unlock(&queue_mutex);
}

static void *video_lock(void *opaque, void **planes) {
    (void)opaque;
    *planes = video_staging;
    return NULL;
}

static void video_unlock(void *opaque, void *picture, void *const *planes) {
    (void)opaque;
    (void)picture;
    (void)planes;
    pthread_mutex_lock(&video_mutex);
    memcpy(video_pixels, video_staging, sizeof(video_pixels));
    video_generation++;
    pthread_mutex_unlock(&video_mutex);
}

static void video_display(void *opaque, void *picture) {
    (void)opaque;
    (void)picture;
}

static void media_event(const libvlc_event_t *event, void *userdata) {
    (void)userdata;
    if (event->type == libvlc_MediaPlayerEncounteredError) atomic_store(&playback_error_pending, true);
    if (event->type == libvlc_MediaPlayerEndReached || event->type == libvlc_MediaPlayerEncounteredError) atomic_store(&advance_pending, true);
}

static void metadata_event(const libvlc_event_t *event, void *userdata) {
    (void)event;
    (void)userdata;
    atomic_store(&metadata_pending, true);
}

static bool play_entry(size_t index) {
    if (!vlc_player || index >= entry_count) return false;
    libvlc_media_player_stop(vlc_player);
    current_entry = index;
    entry_started_at = monotonic_seconds();
    last_position_at = entry_started_at;
    last_position = 0;
    volume_confirmed = false;
    next_volume_check = entry_started_at + 0.3;
    pthread_mutex_lock(&video_mutex);
    video_generation = 0;
    pthread_mutex_unlock(&video_mutex);
    libvlc_media_t *media = libvlc_media_new_path(vlc_instance, entries[index].path);
    if (!media) return false;
    libvlc_event_manager_t *media_events = libvlc_media_event_manager(media);
    libvlc_event_attach(media_events, libvlc_MediaMetaChanged, metadata_event, NULL);
    libvlc_event_attach(media_events, libvlc_MediaParsedChanged, metadata_event, NULL);
    libvlc_media_player_set_media(vlc_player, media);
    libvlc_media_parse_with_options(media, libvlc_media_parse_local, 1000);
    libvlc_media_release(media);
    atomic_store(&advance_pending, false);
    atomic_store(&playback_error_pending, false);
    update_next();
    bool started = libvlc_media_player_play(vlc_player) == 0;
    if (started) libvlc_audio_set_volume(vlc_player, requested_volume);
    begin_video_color(started && entries[index].video ? entries[index].path : NULL);
    return started;
}

static bool start_local_player(char *error, size_t error_size) {
    const char *options[] = {"--quiet", "--no-video-title-show"};
    vlc_instance = libvlc_new((int)(sizeof(options) / sizeof(*options)), options);
    if (vlc_instance) vlc_player = libvlc_media_player_new(vlc_instance);
    if (!vlc_player) { snprintf(error, error_size, "could not start the built-in media player"); local_close(); return false; }
    libvlc_video_set_callbacks(vlc_player, video_lock, video_unlock, video_display, NULL);
    libvlc_video_set_format(vlc_player, "RV32", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_WIDTH * 4);
    libvlc_event_manager_t *events = libvlc_media_player_event_manager(vlc_player);
    libvlc_event_attach(events, libvlc_MediaPlayerEndReached, media_event, NULL);
    libvlc_event_attach(events, libvlc_MediaPlayerEncounteredError, media_event, NULL);
    for (size_t index = 0; index < entry_count; index++) if (play_entry(index)) return true;
    snprintf(error, error_size, "none of the media files could be played");
    local_close();
    return false;
}

bool local_open_folder(const char *folder, char *error, size_t error_size) {
    char *resolved = realpath(folder, NULL);
    if (!resolved) { snprintf(error, error_size, "cannot open folder: %s", folder); return false; }
    DIR *directory = opendir(resolved);
    if (!directory) { snprintf(error, error_size, "not a readable folder: %s", folder); free(resolved); return false; }
    LocalEntry *found = NULL;
    size_t found_count = 0, found_capacity = 0;
    bool exhausted = false;
    struct dirent *item;
    while ((item = readdir(directory))) {
        bool video;
        if (item->d_name[0] == '.' || !media_extension(item->d_name, &video)) continue;
        size_t length = strlen(resolved) + strlen(item->d_name) + 2;
        char *path = malloc(length);
        if (!path) { exhausted = true; break; }
        snprintf(path, length, "%s/%s", resolved, item->d_name);
        struct stat details;
        if (stat(path, &details) || !S_ISREG(details.st_mode)) { free(path); continue; }
        if (found_count == found_capacity) {
            size_t capacity = found_capacity ? found_capacity * 2 : 32;
            LocalEntry *grown = realloc(found, capacity * sizeof(*found));
            if (!grown) { free(path); exhausted = true; break; }
            found = grown;
            found_capacity = capacity;
        }
        char *name = strdup(item->d_name);
        if (!name) { free(path); exhausted = true; break; }
        LocalEntry *entry = &found[found_count];
        *entry = (LocalEntry){.path = path, .name = name, .video = video};
        found_count++;
    }
    closedir(directory);
    free(resolved);
    if (exhausted || !found_count) {
        for (size_t index = 0; index < found_count; index++) { free(found[index].path); free(found[index].name); }
        free(found);
        snprintf(error, error_size, exhausted ? "could not scan folder: %s" : "no playable media found in: %s", folder);
        return false;
    }
    local_close();
    entries = found;
    entry_count = found_count;
    entry_capacity = found_capacity;
    return start_local_player(error, error_size);
}

bool local_open_path(const char *path, char *error, size_t error_size) {
    char *resolved = realpath(path, NULL);
    if (!resolved) { snprintf(error, error_size, "cannot open media: %s", path); return false; }
    struct stat details;
    if (stat(resolved, &details)) { snprintf(error, error_size, "cannot read media: %s", path); free(resolved); return false; }
    if (S_ISDIR(details.st_mode)) { free(resolved); return local_open_folder(path, error, error_size); }
    const char *name = strrchr(resolved, '/');
    name = name ? name + 1 : resolved;
    bool video;
    if (!S_ISREG(details.st_mode) || !media_extension(name, &video)) {
        snprintf(error, error_size, "unsupported media file: %s", path);
        free(resolved);
        return false;
    }
    char *copy = strdup(name);
    if (!copy) { free(resolved); snprintf(error, error_size, "out of memory"); return false; }
    local_close();
    entries = calloc(1, sizeof(*entries));
    if (!entries) { free(copy); free(resolved); snprintf(error, error_size, "out of memory"); return false; }
    entries[0] = (LocalEntry){.path = resolved, .name = copy, .video = video};
    entry_count = entry_capacity = 1;
    return start_local_player(error, error_size);
}

bool local_is_active(void) {
    return vlc_player != NULL;
}

bool local_metadata_changed(void) {
    return atomic_exchange(&metadata_pending, false);
}

bool local_update(void) {
    if (!vlc_player) return false;
    bool advance = atomic_exchange(&advance_pending, false);
    bool failed = atomic_exchange(&playback_error_pending, false);
    if (!advance) {
        double now = monotonic_seconds();
        if (libvlc_media_player_get_state(vlc_player) != libvlc_Playing) { last_position_at = now; return false; }
        libvlc_time_t position = libvlc_media_player_get_time(vlc_player);
        if (position > last_position + 250 || position + 1000 < last_position) {
            last_position = position;
            last_position_at = now;
        }
        if (now - entry_started_at < 8.0 || now - last_position_at < 7.0) return false;
        failed = true;
    }
    if (failed) {
        char message[MAX_FIELD];
        snprintf(message, sizeof(message), "Could not play %s; trying next file", entries[current_entry].name);
        show_notice(message);
    }
    size_t next = current_entry + 1;
    if (next >= entry_count && repeat_folder) next = 0;
    while (next < entry_count) {
        if (play_entry(next)) return true;
        show_notice("Could not start a file; trying next file");
        next++;
    }
    libvlc_media_player_stop(vlc_player);
    pthread_mutex_lock(&queue_mutex);
    next_track[0] = 0;
    pthread_mutex_unlock(&queue_mutex);
    return true;
}

static void read_meta(libvlc_media_t *media, libvlc_meta_t key, char *target, size_t size) {
    char *value = libvlc_media_get_meta(media, key);
    if (value) {
        if (strspn(value, " \t\r\n") != strlen(value)) set_string(target, size, value);
        libvlc_free(value);
    }
}

bool local_get_song(Song *song) {
    if (!vlc_player) return false;
    memset(song, 0, sizeof(*song));
    set_string(song->player, sizeof(song->player), "local");
    entry_title(current_entry, song->title, sizeof(song->title));
    set_string(song->media_url, sizeof(song->media_url), entries[current_entry].path);
    const char *dot = strrchr(entries[current_entry].name, '.');
    if (dot) set_string(song->codec, sizeof(song->codec), dot + 1);
    snprintf(song->track, sizeof(song->track), "%zu", current_entry + 1);
    libvlc_media_t *media = libvlc_media_player_get_media(vlc_player);
    if (media) {
        char metadata_title[MAX_FIELD] = {0};
        read_meta(media, libvlc_meta_Title, metadata_title, sizeof(metadata_title));
        if (metadata_title[0] && strcmp(metadata_title, entries[current_entry].name)) set_string(song->title, sizeof(song->title), metadata_title);
        if (!entries[current_entry].video) read_meta(media, libvlc_meta_Artist, song->artist, sizeof(song->artist));
        read_meta(media, libvlc_meta_ArtworkURL, song->art_url, sizeof(song->art_url));
        libvlc_media_release(media);
    }
    if (entries[current_entry].video) song->art_url[0] = 0;
    else if (strspn(song->artist, " \t\r\n") == strlen(song->artist)) set_string(song->artist, sizeof(song->artist), song->title);
    libvlc_state_t state = libvlc_media_player_get_state(vlc_player);
    if (state == libvlc_Playing && !volume_confirmed && monotonic_seconds() >= next_volume_check) {
        int actual = libvlc_audio_get_volume(vlc_player);
        if (actual != requested_volume) libvlc_audio_set_volume(vlc_player, requested_volume);
        actual = libvlc_audio_get_volume(vlc_player);
        volume_confirmed = actual == requested_volume;
        next_volume_check = monotonic_seconds() + 0.5;
    }
    set_string(song->status, sizeof(song->status), state == libvlc_Paused ? "Paused" : state == libvlc_Playing ? "Playing" : "Stopped");
    libvlc_time_t position = libvlc_media_player_get_time(vlc_player);
    libvlc_time_t duration = libvlc_media_player_get_length(vlc_player);
    song->position = position > 0 ? (double)position / 1000.0 : 0.0;
    song->length = duration > 0 ? (double)duration / 1000.0 : 0.0;
    int volume = libvlc_audio_get_volume(vlc_player);
    song->volume = volume >= 0 ? volume : 100;
    return true;
}

bool local_is_loading(void) {
    if (!vlc_player) return false;
    libvlc_state_t state = libvlc_media_player_get_state(vlc_player);
    if (monotonic_seconds() - entry_started_at >= 4.0) return false;
    if (state != libvlc_Playing && state != libvlc_Paused) return true;
    if (!entries[current_entry].video) return false;
    pthread_mutex_lock(&video_mutex);
    bool ready = video_generation != 0;
    pthread_mutex_unlock(&video_mutex);
    return !ready;
}

bool local_has_video(void) {
    return vlc_player && entries[current_entry].video;
}

bool local_video_color(int *red, int *green, int *blue) {
    if (!local_has_video()) return false;
    pthread_mutex_lock(&color_mutex);
    bool ready = color_ready;
    if (ready) { *red = color_red; *green = color_green; *blue = color_blue; }
    pthread_mutex_unlock(&color_mutex);
    return ready;
}

bool local_copy_video(Image *image, unsigned long *generation) {
    if (!local_has_video()) return false;
    pthread_mutex_lock(&video_mutex);
    if (!video_generation || (generation && *generation == video_generation)) { pthread_mutex_unlock(&video_mutex); return false; }
    image->pixels = malloc(VIDEO_WIDTH * VIDEO_HEIGHT * 3);
    if (!image->pixels) { pthread_mutex_unlock(&video_mutex); return false; }
    for (size_t pixel = 0; pixel < VIDEO_WIDTH * VIDEO_HEIGHT; pixel++) {
        image->pixels[pixel * 3] = video_pixels[pixel * 4 + 2];
        image->pixels[pixel * 3 + 1] = video_pixels[pixel * 4 + 1];
        image->pixels[pixel * 3 + 2] = video_pixels[pixel * 4];
    }
    image->width = VIDEO_WIDTH;
    image->height = VIDEO_HEIGHT;
    image->channels = 3;
    if (generation) *generation = video_generation;
    pthread_mutex_unlock(&video_mutex);
    return true;
}

bool local_command(const char *action, const char *value) {
    if (!vlc_player) return false;
    if (!strcmp(action, "play-pause")) libvlc_media_player_set_pause(vlc_player, libvlc_media_player_is_playing(vlc_player) ? 1 : 0);
    else if (!strcmp(action, "next")) {
        if (current_entry + 1 < entry_count) play_entry(current_entry + 1);
        else if (repeat_folder) play_entry(0);
    } else if (!strcmp(action, "previous")) {
        if (libvlc_media_player_get_time(vlc_player) > 3000) libvlc_media_player_set_time(vlc_player, 0);
        else if (current_entry) play_entry(current_entry - 1);
    } else if (!strcmp(action, "volume")) {
        int volume = requested_volume;
        volume += value && !strcmp(value, "down") ? -5 : 5;
        requested_volume = volume < 0 ? 0 : volume > 200 ? 200 : volume;
        libvlc_audio_set_volume(vlc_player, requested_volume);
        volume_confirmed = false;
        next_volume_check = monotonic_seconds() + 0.3;
    } else if (!strcmp(action, "seek")) {
        libvlc_time_t position = libvlc_media_player_get_time(vlc_player);
        libvlc_time_t length = libvlc_media_player_get_length(vlc_player);
        if (position < 0) position = 0;
        position += value && !strcmp(value, "back") ? -5000 : 5000;
        if (position < 0) position = 0;
        if (length > 0 && position >= length) position = length > 100 ? length - 100 : 0;
        libvlc_media_player_set_time(vlc_player, position);
    } else if (!strcmp(action, "loop")) { repeat_folder = !repeat_folder; update_next(); }
    return true;
}

void local_close(void) {
    begin_video_color(NULL);
    if (vlc_player) {
        libvlc_media_player_stop(vlc_player);
        libvlc_media_player_release(vlc_player);
        vlc_player = NULL;
    }
    if (vlc_instance) { libvlc_release(vlc_instance); vlc_instance = NULL; }
    for (size_t i = 0; i < entry_count; i++) { free(entries[i].path); free(entries[i].name); }
    free(entries);
    entries = NULL;
    entry_count = 0;
    entry_capacity = 0;
}
