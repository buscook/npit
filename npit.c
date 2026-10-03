#include "npit_internal.h"

Config cfg;
static struct termios saved_termios;
static bool terminal_active;
static volatile sig_atomic_t running = 1;
bool cava_enabled;
static int cava_fd = -1;
static pid_t cava_pid = -1;
int player_follow_fd = -1;
pthread_t player_follow_thread;
GMainContext *player_follow_context;
GMainLoop *player_follow_loop;
bool player_follow_started;
static double player_follow_retry_at;
int cava_values[128];
static char cava_frame[16384];
static size_t cava_frame_length;
char next_track[MAX_FIELD] = "";
char ui_notice[MAX_FIELD];
double ui_notice_until;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
bool queue_pending;
bool queue_requested;
bool queue_worker_started;
bool queue_worker_stop;
unsigned long queue_generation;
pthread_t queue_thread;
pthread_cond_t queue_condition = PTHREAD_COND_INITIALIZER;
char current_playlist[MAX_FIELD];
char cached_playlist_uri[MAX_FIELD];
double playlist_checked_at;
Lyric lyrics[MAX_LYRICS];
int lyric_count;
bool lyrics_loaded;
static char config_path[1024];
int cover_r = 0, cover_g = 220, cover_b = 190;
atomic_bool force_redraw = true;

void show_notice(const char *message) {
    set_string(ui_notice, sizeof(ui_notice), message);
    ui_notice_until = monotonic_seconds() + 6.0;
}
int previous_rows, previous_cols;
DetailRow *detail_rows;
int detail_row_count;
WrappedLyric wrapped_lyrics[MAX_LYRICS];
unsigned long lyric_generation;
unsigned long wrapped_generation = (unsigned long)-1;
int wrapped_width;
bool wrapped_clean;
bool frame_changed;
char previous_art_url[MAX_FIELD];
pthread_mutex_t lyric_mutex = PTHREAD_MUTEX_INITIALIZER;
bool lyrics_pending;
int lyric_event_fd = -1;
char lyric_identity[MAX_FIELD];
unsigned long lyric_request_generation;
bool spotify_auth_attempted;
pthread_mutex_t artwork_mutex = PTHREAD_MUTEX_INITIALIZER;
Image current_artwork;
char current_artwork_url[MAX_FIELD];
char desired_artwork_url[MAX_FIELD];
unsigned long artwork_generation;
ArtworkCacheEntry artwork_cache[ART_CACHE_SLOTS];
size_t artwork_cache_bytes;
unsigned long artwork_cache_clock;
char preloading_artwork_url[MAX_FIELD];
double preload_retry_at;
int active_lyric_line = -2;
double lyric_transition_start;
MarqueeState title_marquee;
MarqueeState album_marquee;
MarqueeState playlist_marquee;
MarqueeState next_marquee;
KittyFont kitty_font;
bool graphics_supported;
bool synchronized_updates_supported;
int probed_cell_width;
int probed_cell_height;
GDBusConnection *mpris_bus;
bool profile_enabled;
TimingMetric metadata_timing;
static TimingMetric render_timing;
TimingMetric artwork_timing;
TimingMetric lyrics_timing;
TimingMetric http_timing;
atomic_ullong http_failures;
pthread_key_t http_key;
pthread_once_t http_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static unsigned active_workers;
atomic_bool workers_stopping;



bool reserve_worker(void) {
    pthread_mutex_lock(&worker_mutex);
    bool accepted = !atomic_load(&workers_stopping);
    if (accepted) active_workers++;
    pthread_mutex_unlock(&worker_mutex);
    return accepted;
}

void release_worker(void) {
    pthread_mutex_lock(&worker_mutex);
    active_workers--;
    pthread_cond_broadcast(&worker_condition);
    pthread_mutex_unlock(&worker_mutex);
}

void reset_detail_rows(void) {
    for (int i = 0; i < detail_row_count; i++) free(detail_rows[i].text);
    free(detail_rows);
    detail_rows = NULL;
    detail_row_count = 0;
}

void reset_wrapped_lyrics(void) {
    for (int i = 0; i < MAX_LYRICS; i++) {
        free(wrapped_lyrics[i].text);
        wrapped_lyrics[i].text = NULL;
        wrapped_lyrics[i].height = 0;
    }
}

static void cleanup(void) {
    atomic_store(&workers_stopping, true);
    pthread_mutex_lock(&queue_mutex);
    queue_worker_stop = true;
    pthread_cond_signal(&queue_condition);
    pthread_mutex_unlock(&queue_mutex);
    if (queue_worker_started) pthread_join(queue_thread, NULL);
    pthread_mutex_lock(&worker_mutex);
    while (active_workers) pthread_cond_wait(&worker_condition, &worker_mutex);
    pthread_mutex_unlock(&worker_mutex);
    stop_player_follow();
    local_close();
    reset_detail_rows();
    reset_wrapped_lyrics();
    if (cava_pid > 0) {
        kill(cava_pid, SIGTERM);
        waitpid(cava_pid, NULL, 0);
        cava_pid = -1;
    }
    if (cava_fd >= 0) {
        close(cava_fd);
        cava_fd = -1;
    }
    if (mpris_bus) {
        g_object_unref(mpris_bus);
        mpris_bus = NULL;
    }
    if (lyric_event_fd >= 0) close(lyric_event_fd);
    if (terminal_active) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_termios);
        fputs("\033[?2026l\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        terminal_active = false;
    }
}

static void on_signal(int sig) {
    (void)sig;
    running = false;
}

double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void record_timing(TimingMetric *metric, double started_at) {
    if (!profile_enabled) return;
    unsigned long long elapsed = (unsigned long long)((monotonic_seconds() - started_at) * 1000000000.0);
    atomic_fetch_add(&metric->count, 1);
    atomic_fetch_add(&metric->total_ns, elapsed);
    unsigned long long prior = atomic_load(&metric->max_ns);
    while (elapsed > prior && !atomic_compare_exchange_weak(&metric->max_ns, &prior, elapsed)) {}
}

static void report_metric(const char *name, const TimingMetric *metric) {
    unsigned long long count = atomic_load(&metric->count);
    if (!count) return;
    double average = (double)atomic_load(&metric->total_ns) / count / 1000000.0;
    double maximum = (double)atomic_load(&metric->max_ns) / 1000000.0;
    fprintf(stderr, "%s: %llu calls, %.2f ms average, %.2f ms max\n", name, count, average, maximum);
}

static void report_timings(void) {
    if (!profile_enabled) return;
    fputs("npit timings\n", stderr);
    report_metric("metadata", &metadata_timing);
    report_metric("render", &render_timing);
    report_metric("artwork", &artwork_timing);
    report_metric("lyrics", &lyrics_timing);
    report_metric("http", &http_timing);
    fprintf(stderr, "http failures: %llu\n", atomic_load(&http_failures));
}

void trim(char *value) {
    size_t n = strlen(value);
    while (n && (value[n - 1] == '\n' || value[n - 1] == '\r' || value[n - 1] == ' ' || value[n - 1] == '\t')) value[--n] = 0;
    size_t start = 0;
    while (value[start] == ' ' || value[start] == '\t') start++;
    if (start) memmove(value, value + start, strlen(value + start) + 1);
}

static bool parse_bool(const char *value) {
    return !strcasecmp(value, "true") || !strcmp(value, "1") || !strcasecmp(value, "yes");
}

void set_string(char *target, size_t size, const char *value) {
    size_t length = strlen(value);
    if (length >= size) length = size - 1;
    memcpy(target, value, length);
    target[length] = 0;
}


static void defaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    set_string(cfg.theme, sizeof(cfg.theme), "cover");
    set_string(cfg.art_mode, sizeof(cfg.art_mode), "color");
    set_string(cfg.characters, sizeof(cfg.characters), " .,:;irsXA253hMHGS#9B&@");
    cfg.visualizer = true;
    cfg.dim_unplayed = true;
    cfg.lyrics = true;
    cfg.show_album = true;
    cfg.show_track = true;
    cfg.show_time = true;
    cfg.show_status = true;
    cfg.show_volume = true;
    cfg.show_next = true;
    cfg.show_source = true;
    cfg.show_playlist = true;
    cfg.fps = 60;
    cfg.bars = 32;
    cfg.sensitivity = 175;
    cfg.paused_fps = 5;
    cfg.transition_fps = 20;
    cfg.transition_duration = 0.3;
    cfg.lyric_lines = 3;
    cfg.metadata_interval = 0.5;
    cfg.spotify_interval = 1.0;
    cfg.marquee_speed = 3.0;
}

static void apply_setting(const char *section, const char *key, const char *raw) {
    char value[MAX_FIELD];
    set_string(value, sizeof(value), raw);
    trim(value);
    size_t n = strlen(value);
    if (n >= 2 && ((value[0] == '"' && value[n - 1] == '"') || (value[0] == '\'' && value[n - 1] == '\''))) {
        memmove(value, value + 1, n - 2);
        value[n - 2] = 0;
    }
    if (!strcmp(section, "visualizer")) {
        if (!strcmp(key, "enabled")) cfg.visualizer = parse_bool(value);
        else if (!strcmp(key, "dim_unplayed")) cfg.dim_unplayed = parse_bool(value);
        else if (!strcmp(key, "fps")) cfg.fps = atoi(value);
        else if (!strcmp(key, "bars")) cfg.bars = atoi(value);
        else if (!strcmp(key, "sensitivity")) cfg.sensitivity = atoi(value);
    } else if (!strcmp(section, "lyrics")) {
        if (!strcmp(key, "enabled")) cfg.lyrics = parse_bool(value);
        else if (!strcmp(key, "smooth_scroll")) cfg.smooth_scroll = parse_bool(value);
        else if (!strcmp(key, "transition_fps")) cfg.transition_fps = atoi(value);
        else if (!strcmp(key, "transition_duration")) cfg.transition_duration = atof(value);
        else if (!strcmp(key, "lines")) cfg.lyric_lines = atoi(value);
        else if (!strcmp(key, "offset")) cfg.lyric_offset = atof(value);
    } else if (!strcmp(section, "appearance")) {
        if (!strcmp(key, "theme")) set_string(cfg.theme, sizeof(cfg.theme), value);
        else if (!strcmp(key, "preset")) set_string(cfg.preset, sizeof(cfg.preset), value);
        else if (!strcmp(key, "scroll_speed")) cfg.marquee_speed = atof(value);
        else if (!strcmp(key, "terminal_font")) set_string(cfg.terminal_font, sizeof(cfg.terminal_font), value);
        else if (!strcmp(key, "terminal_font_size")) cfg.terminal_font_size = atof(value);
    } else if (!strcmp(section, "colors")) {
        if (!strcmp(key, "theme")) set_string(cfg.theme, sizeof(cfg.theme), value);
    } else if (!strcmp(section, "artwork")) {
        if (!strcmp(key, "mode")) set_string(cfg.art_mode, sizeof(cfg.art_mode), value);
        else if (!strcmp(key, "characters")) set_string(cfg.characters, sizeof(cfg.characters), value);
    } else if (!strcmp(section, "behavior")) {
        if (!strcmp(key, "player")) set_string(cfg.selected_player, sizeof(cfg.selected_player), value);
        else if (!strcmp(key, "metadata_interval")) cfg.metadata_interval = atof(value);
        else if (!strcmp(key, "spotify_interval")) cfg.spotify_interval = atof(value);
    } else if (!strcmp(section, "layout")) {
        if (!strcmp(key, "minimal")) cfg.minimal = parse_bool(value);
    } else if (!strcmp(section, "performance")) {
        if (!strcmp(key, "safe_render")) cfg.safe_render = parse_bool(value);
        else if (!strcmp(key, "paused_fps")) cfg.paused_fps = atoi(value);
    } else if (!strcmp(section, "display")) {
        if (!strcmp(key, "album")) cfg.show_album = parse_bool(value);
        else if (!strcmp(key, "track_number")) cfg.show_track = parse_bool(value);
        else if (!strcmp(key, "playback_time")) cfg.show_time = parse_bool(value);
        else if (!strcmp(key, "status")) cfg.show_status = parse_bool(value);
        else if (!strcmp(key, "volume")) cfg.show_volume = parse_bool(value);
        else if (!strcmp(key, "next_track")) cfg.show_next = parse_bool(value);
        else if (!strcmp(key, "source")) cfg.show_source = parse_bool(value);
        else if (!strcmp(key, "playlist")) cfg.show_playlist = parse_bool(value);
    } else if (!strcmp(section, "controls")) {
        if (!strcmp(key, "enabled")) cfg.keyboard = parse_bool(value);
    } else if (!strcmp(section, "clean")) {
        if (!strcmp(key, "enabled")) cfg.clean = parse_bool(value);
        else if (!strcmp(key, "extra_words")) {
            cfg.extra_word_count = 0;
            char *cursor = value;
            while (*cursor && cfg.extra_word_count < 32) {
                while (*cursor && (*cursor == '[' || *cursor == ']' || *cursor == ',' || *cursor == ' ' || *cursor == '\'' || *cursor == '"')) cursor++;
                char word[64];
                size_t n = 0;
                while (*cursor && *cursor != ']' && *cursor != ',' && *cursor != '\'' && *cursor != '"' && n + 1 < sizeof(word)) word[n++] = *cursor++;
                word[n] = 0;
                if (n) { lowercase(word); set_string(cfg.extra_words[cfg.extra_word_count++], sizeof(cfg.extra_words[0]), word); }
                while (*cursor && *cursor != ',') cursor++;
                if (*cursor == ',') cursor++;
            }
        }
    }
}

static void load_config(const char *custom_path) {
    if (custom_path && *custom_path) {
        if (custom_path[0] == '~' && custom_path[1] == '/') snprintf(config_path, sizeof(config_path), "%s/%s", getenv("HOME") ? getenv("HOME") : ".", custom_path + 2);
        else set_string(config_path, sizeof(config_path), custom_path);
    } else {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char directory[1024];
    if (xdg && *xdg) snprintf(directory, sizeof(directory), "%s/npit", xdg);
    else snprintf(directory, sizeof(directory), "%s/.config/npit", getenv("HOME") ? getenv("HOME") : ".");
    mkdir(directory, 0700);
    snprintf(config_path, sizeof(config_path), "%.1000s/settings.toml", directory);
    }
    FILE *file = fopen(config_path, "r");
    if (!file) {
        char share[1024];
        snprintf(share, sizeof(share), "%s/settings.toml", NPIT_DATADIR);
        FILE *template = fopen(share, "r");
        if (!template) template = fopen("settings.toml", "r");
        if (template) {
            file = fopen(config_path, "w");
            if (file) {
                char copy[4096];
                size_t count;
                while ((count = fread(copy, 1, sizeof(copy), template)) > 0) fwrite(copy, 1, count, file);
                fclose(file);
            }
            fclose(template);
            file = fopen(config_path, "r");
        }
    }
    if (!file) return;
    char section[128] = "";
    char line[4096];
    while (fgets(line, sizeof(line), file)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = 0;
                set_string(section, sizeof(section), line + 1);
            }
            continue;
        }
        char *equal = strchr(line, '=');
        if (!equal) continue;
        *equal = 0;
        trim(line);
        apply_setting(section, line, equal + 1);
    }
    fclose(file);
    if (cfg.fps < 0) cfg.fps = 0;
    if (cfg.fps > 360) cfg.fps = 360;
    if (cfg.fps && cfg.fps < 10) cfg.fps = 10;
    if (cfg.bars < 8) cfg.bars = 8;
    if (cfg.bars > 128) cfg.bars = 128;
    if (cfg.transition_fps < 5) cfg.transition_fps = 5;
    if (cfg.transition_fps > 360) cfg.transition_fps = 360;
    if (cfg.lyric_lines < 1) cfg.lyric_lines = 1;
    if (cfg.lyric_lines > 9) cfg.lyric_lines = 9;
    if (cfg.transition_duration < 0.1) cfg.transition_duration = 0.1;
    if (cfg.transition_duration > 2.0) cfg.transition_duration = 2.0;
    if (cfg.marquee_speed < 0.5) cfg.marquee_speed = 0.5;
    if (cfg.marquee_speed > 20.0) cfg.marquee_speed = 20.0;
    if (cfg.paused_fps < 1) cfg.paused_fps = 1;
    if (cfg.paused_fps > 30) cfg.paused_fps = 30;
    if (!strcmp(cfg.preset, "minimal")) cfg.minimal = true;
    if (!strcmp(cfg.preset, "compact")) { cfg.show_track = false; cfg.show_next = false; cfg.show_status = false; cfg.lyric_lines = 1; }
    if (!strcmp(cfg.preset, "cinema")) { cfg.lyrics = true; cfg.lyric_lines = 5; cfg.bars = 48; }
}

static int detect_refresh_rate(void) {
    char *argv[] = {"xrandr", "--current", NULL};
    char *output = capture(argv);
    int rate = 60;
    if (!output) return rate;
    char *save_line = NULL;
    for (char *line = strtok_r(output, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
        char *cursor = line;
        while (*cursor) {
            char *end = NULL;
            double candidate = strtod(cursor, &end);
            if (end != cursor) {
                bool active = strchr(cursor, '*') && strchr(cursor, '*') < end + 3;
                if (active && candidate >= 20 && candidate <= 360) rate = (int)round(candidate);
                cursor = end;
            } else cursor++;
        }
    }
    free(output);
    return rate;
}

static void handle_keypress(char key, const Song *song) {
    if (key == 'q' || key == 3) { running = false; return; }
    if (!cfg.keyboard) return;
    if (key == ' ') player_command(song, "play-pause", NULL);
    else if (key == 'l' || key == 'L') {
        cfg.lyrics = !cfg.lyrics;
        if (cfg.lyrics) request_lyrics(song);
        force_redraw = true;
    } else if (key == 'k' || key == 'K') { cfg.clean = !cfg.clean; force_redraw = true; }
    else if (key == 's' || key == 'S') player_command(song, "shuffle", "toggle");
    else if (key == 'r' || key == 'R') player_command(song, "loop", "toggle");
    else if (key == '+' || key == '=') player_command(song, "volume", "up");
    else if (key == '-') player_command(song, "volume", "down");
    else if (key == '[') player_command(song, "seek", "back");
    else if (key == ']') player_command(song, "seek", "forward");
}

static void start_cava(void) {
    if (!cfg.visualizer || access("/usr/bin/cava", X_OK) != 0) return;
    int pipes[2];
    if (pipe(pipes) != 0) return;
    char config[1024];
    snprintf(config, sizeof(config), "%s/npit-cava.conf", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    FILE *file = fopen(config, "w");
    if (!file) { close(pipes[0]); close(pipes[1]); return; }
    fprintf(file, "[general]\nbars = %d\nframerate = %d\nautosens = 1\nsensitivity = %d\n[input]\nmethod = pipewire\nsource = auto\n[output]\nmethod = raw\nraw_target = /dev/stdout\ndata_format = ascii\nascii_max_range = 1000\nbar_delimiter = 59\nframe_delimiter = 10\nchannels = mono\nmono_option = average\n", cfg.bars, cfg.fps ? cfg.fps : detect_refresh_rate(), cfg.sensitivity);
    fclose(file);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]); close(pipes[1]);
        execlp("cava", "cava", "-p", config, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return; }
    cava_pid = pid;
    cava_fd = pipes[0];
    fcntl(cava_fd, F_SETFL, O_NONBLOCK);
    cava_enabled = true;
}

static bool update_cava(void) {
    if (cava_fd < 0) return false;
    int previous[128];
    memcpy(previous, cava_values, sizeof(previous));
    char buffer[4096];
    ssize_t length;
    while ((length = read(cava_fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < length; i++) {
            if (buffer[i] == '\n') {
                cava_frame[cava_frame_length] = 0;
                int values[128] = {0};
                int index = 0;
                char *save = NULL;
                for (char *token = strtok_r(cava_frame, ";\r", &save); token && index < 128; token = strtok_r(NULL, ";\r", &save)) {
                    int value = atoi(token);
                    values[index++] = value < 0 ? 0 : value > 1000 ? 1000 : value;
                }
                if (index > 0) memcpy(cava_values, values, sizeof(cava_values));
                cava_frame_length = 0;
            } else if (cava_frame_length + 1 < sizeof(cava_frame)) {
                cava_frame[cava_frame_length++] = buffer[i];
            } else {
                cava_frame_length = 0;
            }
        }
    }
    if (length == 0) {
        close(cava_fd);
        cava_fd = -1;
        cava_enabled = false;
        memset(cava_values, 0, sizeof(cava_values));
    }
    return memcmp(previous, cava_values, sizeof(previous)) != 0;
}

static void usage(void) {
    puts("npit - now playing in terminal\n\nusage: npit [options] [FOLDER]\n\nFOLDER plays audio and video files in the order returned by the folder. Track numbers reflect that order. Video appears as moving ASCII.\n\n  -h, --help                 show this help\n      --version              show version\n      --update               build and install the latest GitHub main\n      --config PATH          use a settings file\n      --print-config-path    show settings location\n      --diagnose             show runtime dependency status\n      --profile              print timing summary after exit\n      --player NAME          choose an MPRIS player\n      --theme NAME           cover, cyan, green, amber, purple, monochrome, rainbow\n      --art-mode MODE        color, monochrome, blocks, none\n      --preset NAME          default, compact, cinema, minimal\n      --fps NUMBER           visualizer and text scroll FPS, 10-360; 0 detects monitor rate\n      --bars NUMBER          CAVA bar count\n      --sensitivity NUMBER   CAVA input sensitivity\n      --spotify-interval SEC Spotify queue refresh interval\n      --no-visualizer        disable CAVA spectrum\n      --lyrics               enable lyrics\n      --no-lyrics            disable lyrics\n      --clean                censor explicit words\n      --no-clean             show unfiltered text\n      --minimal              show title, artist, spectrum, and time\n      --safe-render          limit redraws for slower terminals\n\nwhen [controls].enabled is true: space=play/pause, arrows=skip, [ and ]=seek 5 seconds, +/-=volume, s=shuffle, r=repeat, l=lyrics, k=clean. q and ctrl+c always quit.");
}

static bool dropped_path(const char *input, char *path, size_t size) {
    while (isspace((unsigned char)*input)) input++;
    size_t length = strlen(input);
    while (length && isspace((unsigned char)input[length - 1])) length--;
    if (length > 1 && (input[0] == '\'' || input[0] == '"') && input[length - 1] == input[0]) { input++; length -= 2; }
    char raw[8192];
    if (length >= sizeof(raw)) return false;
    memcpy(raw, input, length);
    raw[length] = 0;
    if (!strncmp(raw, "file://", 7)) {
        char *decoded = g_filename_from_uri(raw, NULL, NULL);
        if (!decoded) return false;
        set_string(path, size, decoded);
        g_free(decoded);
        return true;
    }
    size_t output = 0;
    for (size_t i = 0; i < length && output + 1 < size; i++) {
        if (raw[i] == '\\' && i + 1 < length) i++;
        path[output++] = raw[i];
    }
    path[output] = 0;
    if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        char expanded[8192];
        size_t home_length = home ? strlen(home) : 0;
        size_t suffix_length = strlen(path + 1);
        if (home && home_length + suffix_length < sizeof(expanded)) {
            memcpy(expanded, home, home_length);
            memcpy(expanded + home_length, path + 1, suffix_length + 1);
            set_string(path, size, expanded);
        }
    }
    return path[0] != 0;
}

int main(int argc, char **argv) {
    defaults();
    const char *custom_config = NULL;
    const char *media_folder = NULL;
    const char *cli_player = NULL, *cli_theme = NULL, *cli_art_mode = NULL, *cli_preset = NULL;
    int cli_fps = -1, cli_lyrics = -1, cli_bars = -1, cli_sensitivity = -1;
    double cli_spotify_interval = -1;
    bool cli_no_visualizer = false, cli_clean = false, cli_no_clean = false, cli_minimal = false, cli_safe = false;
    bool show_help = false, diagnose = false, print_config_path = false, update = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) show_help = true;
        else if (!strcmp(argv[i], "--update")) update = true;
        else if (!strcmp(argv[i], "--version")) { puts("Now Playing In Terminal " APP_VERSION); return 0; }
        else if (!strcmp(argv[i], "--print-config-path")) print_config_path = true;
        else if (!strcmp(argv[i], "--diagnose")) diagnose = true;
        else if (!strcmp(argv[i], "--profile")) profile_enabled = true;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) custom_config = argv[++i];
        else if (!strcmp(argv[i], "--player") && i + 1 < argc) cli_player = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) cli_theme = argv[++i];
        else if (!strcmp(argv[i], "--art-mode") && i + 1 < argc) cli_art_mode = argv[++i];
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) cli_preset = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) cli_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bars") && i + 1 < argc) cli_bars = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sensitivity") && i + 1 < argc) cli_sensitivity = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spotify-interval") && i + 1 < argc) cli_spotify_interval = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-visualizer")) cli_no_visualizer = true;
        else if (!strcmp(argv[i], "--lyrics")) cli_lyrics = 1;
        else if (!strcmp(argv[i], "--no-lyrics")) cli_lyrics = 0;
        else if (!strcmp(argv[i], "--clean")) cli_clean = true;
        else if (!strcmp(argv[i], "--no-clean")) cli_no_clean = true;
        else if (!strcmp(argv[i], "--minimal")) cli_minimal = true;
        else if (!strcmp(argv[i], "--safe-render")) cli_safe = true;
        else if (argv[i][0] != '-' && !media_folder) media_folder = argv[i];
        else { fprintf(stderr, "unknown option or extra folder: %s\n", argv[i]); return 2; }
    }
    if (show_help) { usage(); return 0; }
    if (update) {
        if (argc != 2) { fputs("npit: use --update on its own\n", stderr); return 2; }
        return update_app();
    }
    load_config(custom_config);
    if (print_config_path) { puts(config_path); return 0; }
    if (cli_player) set_string(cfg.selected_player, sizeof(cfg.selected_player), cli_player);
    if (cli_theme) set_string(cfg.theme, sizeof(cfg.theme), cli_theme);
    if (cli_art_mode) set_string(cfg.art_mode, sizeof(cfg.art_mode), cli_art_mode);
    if (cli_preset) set_string(cfg.preset, sizeof(cfg.preset), cli_preset);
    if (cli_fps >= 0) cfg.fps = cli_fps > 360 ? 360 : cli_fps;
    if (cli_bars >= 0) cfg.bars = cli_bars;
    if (cli_sensitivity >= 0) cfg.sensitivity = cli_sensitivity;
    if (cli_spotify_interval > 0) cfg.spotify_interval = cli_spotify_interval;
    if (cli_no_visualizer) cfg.visualizer = false;
    if (cli_lyrics >= 0) cfg.lyrics = cli_lyrics != 0;
    if (cli_clean) cfg.clean = true;
    if (cli_no_clean) cfg.clean = false;
    if (cli_minimal) cfg.minimal = true;
    if (cli_safe) cfg.safe_render = true;
    if (!strcmp(cfg.preset, "minimal")) cfg.minimal = true;
    if (!strcmp(cfg.preset, "compact")) { cfg.show_track = false; cfg.show_next = false; cfg.show_status = false; cfg.lyric_lines = 1; }
    if (!strcmp(cfg.preset, "cinema")) { cfg.lyrics = true; cfg.lyric_lines = 5; cfg.bars = 48; }
    if (cfg.fps == 0) cfg.fps = detect_refresh_rate();
    if (cfg.bars < 8) cfg.bars = 8;
    if (cfg.bars > 128) cfg.bars = 128;
    if (cfg.sensitivity < 1) cfg.sensitivity = 1;
    if (cfg.sensitivity > 1000) cfg.sensitivity = 1000;
    if (cfg.safe_render && cfg.fps > 15) cfg.fps = 15;
    if (diagnose) {
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        printf("npit %s\nconfig: %s\nsession D-Bus: %s\nplayerctl (optional fallback): %s\ncava: %s\n", APP_VERSION, config_path, bus ? "available" : "missing", access("/usr/bin/playerctl", X_OK) == 0 ? "available" : "missing", access("/usr/bin/cava", X_OK) == 0 ? "available" : "missing");
        if (bus) g_object_unref(bus);
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { fputs("npit requires an interactive terminal\n", stderr); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    setlocale(LC_ALL, "");
    if (media_folder) {
        char error[512];
        if (!local_open_folder(media_folder, error, sizeof(error))) { fprintf(stderr, "npit: %s\n", error); return 1; }
    }
    mpris_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    detect_terminal_font();
    atexit(report_timings);
    atexit(cleanup);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        struct termios raw = saved_termios;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
        terminal_active = true;
    }
    if (terminal_active) probe_terminal_features();
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    lyric_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    start_cava();
    Song song = {0};
    if (!local_is_active()) start_player_follow();
    if (local_is_active()) local_get_song(&song); else get_song(&song);
    request_artwork(song.art_url);
    double next_metadata = monotonic_seconds() + 0.1;
    double next_queue = 0;
    double sync_position = song.position;
    double sync_time = monotonic_seconds();
    if (song.player[0]) request_lyrics(&song);
    char key;
    char pasted_input[8192] = {0};
    size_t pasted_length = 0;
    double pasted_at = 0.0;
    double last_render_at = -INFINITY;
    while (running) {
        double now = monotonic_seconds();
        bool playing = !strcasecmp(song.status, "playing");
        int target_fps = playing ? 10 : cfg.paused_fps;
        if (playing && local_has_video() && !cfg.minimal && strcmp(cfg.art_mode, "none") && target_fps < 30) target_fps = 30;
        if (target_fps < 1) target_fps = 1;
        if ((title_marquee.active || album_marquee.active || playlist_marquee.active || next_marquee.active) && target_fps < cfg.fps) target_fps = cfg.fps;
        if (cfg.smooth_scroll && now - lyric_transition_start < cfg.transition_duration && cfg.transition_fps > target_fps) target_fps = cfg.transition_fps;
        double wait_seconds = last_render_at + 1.0 / target_fps - now;
        if (wait_seconds < 0.0) wait_seconds = 0.0;
        if (next_metadata - now < wait_seconds) wait_seconds = fmax(0.0, next_metadata - now);
        if (pasted_length && pasted_at + 0.2 - now < wait_seconds) wait_seconds = fmax(0.0, pasted_at + 0.2 - now);
        if (cfg.show_next && song.player[0] && strstr(song.player, "spotify") && next_queue - now < wait_seconds) wait_seconds = fmax(0.0, next_queue - now);
        if (playing && cfg.lyrics && !cfg.minimal) {
            double position = sync_position + now - sync_time + cfg.lyric_offset;
            double delay = next_lyric_delay(position);
            if (delay >= 0.0 && delay + 0.001 < wait_seconds) wait_seconds = delay + 0.001;
        }
        long long wait_ns = (long long)(wait_seconds * 1000000000.0);
        struct timespec timeout = {.tv_sec = (time_t)(wait_ns / 1000000000LL), .tv_nsec = (long)(wait_ns % 1000000000LL)};
        struct pollfd inputs[4] = {{.fd = STDIN_FILENO, .events = POLLIN}, {.fd = player_follow_fd, .events = POLLIN}, {.fd = lyric_event_fd, .events = POLLIN}, {.fd = playing && cfg.visualizer ? cava_fd : -1, .events = POLLIN}};
        ppoll(inputs, 4, &timeout, NULL);
        now = monotonic_seconds();
        bool lyrics_changed = inputs[2].revents & POLLIN;
        if (inputs[2].revents & POLLIN) {
            uint64_t signal;
            (void)read(lyric_event_fd, &signal, sizeof(signal));
        }
        bool playback_changed = player_follow_fd >= 0 && (inputs[1].revents & (POLLIN | POLLHUP | POLLERR)) && read_player_follow(now);
        if (inputs[0].revents & POLLIN) {
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (pasted_length || key == '/' || key == '~' || key == '.' || key == '\'' || key == '"' || key == 'f') {
                    if (key == '\r' || key == '\n') pasted_at = 0;
                    else if (key == 127 || key == '\b') { if (pasted_length) pasted_input[--pasted_length] = 0; }
                    else if (pasted_length + 1 < sizeof(pasted_input)) {
                        pasted_input[pasted_length++] = key;
                        pasted_input[pasted_length] = 0;
                        pasted_at = now;
                    } else pasted_length = 0;
                } else if (key == 27) {
                    struct pollfd arrow = {.fd = STDIN_FILENO, .events = POLLIN};
                    if (poll(&arrow, 1, 25) > 0) {
                        char sequence[2];
                        if (read(STDIN_FILENO, sequence, 2) == 2 && sequence[0] == '[' && cfg.keyboard) {
                            if (sequence[1] == 'C') player_command(&song, "next", NULL);
                            if (sequence[1] == 'D') player_command(&song, "previous", NULL);
                        }
                    }
                } else {
                    handle_keypress(key, &song);
                    if (cfg.keyboard && (key == '[' || key == ']')) next_metadata = 0;
                }
            }
        }
        bool dropped_media = false;
        if (pasted_length && (pasted_at == 0 || now - pasted_at >= 0.2)) {
            char path[8192];
            char error[512];
            if (!dropped_path(pasted_input, path, sizeof(path))) show_notice("Could not read dropped path");
            else if (local_open_path(path, error, sizeof(error))) {
                stop_player_follow();
                dropped_media = true;
                atomic_store(&force_redraw, true);
            } else show_notice(error);
            pasted_length = 0;
            pasted_input[0] = 0;
        }
        bool frame_due = now >= last_render_at + 1.0 / target_fps;
        bool visualizer_changed = (inputs[3].revents & (POLLIN | POLLHUP | POLLERR)) || frame_due ? update_cava() : false;
        if (!local_is_active() && player_follow_fd < 0 && now >= player_follow_retry_at) {
            if (!start_player_follow()) player_follow_retry_at = now + 5.0;
        }
        bool local_playback_changed = local_update();
        bool local_tags_changed = local_metadata_changed();
        if (!local_is_active() && now >= next_metadata) request_song_refresh();
        bool metadata_changed = dropped_media || playback_changed || local_playback_changed || local_tags_changed || (local_is_active() && now >= next_metadata);
        if (metadata_changed) {
            Song latest = {0};
            bool found = local_is_active() ? local_get_song(&latest) : get_song(&latest);
            bool changed = strcmp(song.title, latest.title) || strcmp(song.art_url, latest.art_url);
            bool track_changed = strcmp(song.player, latest.player) || strcmp(song.title, latest.title) || strcmp(song.artist, latest.artist) || strcmp(song.album, latest.album);
            if (found && (strcmp(song.title, latest.title) || strcmp(song.artist, latest.artist) || strcmp(song.album, latest.album))) request_lyrics(&latest);
            if (!found && song.player[0]) {
                pthread_mutex_lock(&lyric_mutex);
                lyric_request_generation++;
                lyric_identity[0] = 0;
                lyrics_pending = false;
                lyrics_loaded = false;
                clear_lyrics();
                pthread_mutex_unlock(&lyric_mutex);
                active_lyric_line = -2;
            }
            if (track_changed) {
                pthread_mutex_lock(&queue_mutex);
                queue_generation++;
                if (!local_is_active()) next_track[0] = 0;
                if (!strstr(latest.player, "spotify")) current_playlist[0] = 0;
                pthread_mutex_unlock(&queue_mutex);
                next_queue = 0;
            }
            song = latest;
            if (track_changed && local_is_active()) atomic_store(&force_redraw, true);
            if (changed) request_artwork(song.art_url);
            sync_position = song.position;
            sync_time = now;
            next_metadata = now + fmax(0.2, cfg.metadata_interval > 0 ? cfg.metadata_interval : 0.5);
        }
        if (!local_is_active() && now >= next_metadata) next_metadata = now + fmax(0.2, cfg.metadata_interval > 0 ? cfg.metadata_interval : 0.5);
        song.position = sync_position + (!strcasecmp(song.status, "playing") ? now - sync_time : 0.0);
        if (now >= next_queue && cfg.show_next && song.player[0] && strstr(song.player, "spotify")) {
            request_spotify_queue();
            next_queue = now + (cfg.spotify_interval > 0 ? cfg.spotify_interval : 1.0);
        }
        if (frame_due || visualizer_changed || lyrics_changed || metadata_changed || atomic_load(&force_redraw)) {
            double render_started = profile_enabled ? monotonic_seconds() : 0.0;
            render(&song, now);
            record_timing(&render_timing, render_started);
            last_render_at = now;
        }
    }
    return 0;
}
