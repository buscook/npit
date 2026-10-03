#ifndef NPIT_INTERNAL_H
#define NPIT_INTERNAL_H
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <json-c/json.h>
#include <jpeglib.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <png.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <gio/gio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <locale.h>
#include <strings.h>
#include <pthread.h>
#include <stdatomic.h>

#define APP_VERSION "1"
#ifndef NPIT_DATADIR
#define NPIT_DATADIR "/usr/local/share/npit"
#endif
#ifndef NPIT_PREFIX
#define NPIT_PREFIX "/usr/local"
#endif
#ifndef NPIT_BINDIR
#define NPIT_BINDIR "/usr/local/bin"
#endif
int update_app(void);
#define MAX_FIELD 2048
#define MAX_ART 512
#define MAX_LYRICS 512
#define ART_CACHE_SLOTS 8
#define ART_CACHE_BYTES (8 * 1024 * 1024)

typedef struct {
    char player[128];
    char title[MAX_FIELD];
    char artist[MAX_FIELD];
    char album[MAX_FIELD];
    char track[64];
    char status[64];
    char art_url[MAX_FIELD];
    char media_url[MAX_FIELD];
    char codec[128];
    char playlist[MAX_FIELD];
    double position;
    double length;
    double volume;
} Song;

typedef struct {
    char theme[32];
    char preset[32];
    char art_mode[32];
    char characters[128];
    char selected_player[128];
    char terminal_font[256];
    double terminal_font_size;
    bool visualizer;
    bool dim_unplayed;
    bool lyrics;
    bool clean;
    bool minimal;
    bool safe_render;
    bool show_album;
    bool show_track;
    bool show_time;
    bool show_status;
    bool show_volume;
    bool show_next;
    bool show_source;
    bool show_playlist;
    bool keyboard;
    bool smooth_scroll;
    int fps;
    int bars;
    int sensitivity;
    int paused_fps;
    int transition_fps;
    int lyric_lines;
    int extra_word_count;
    char extra_words[32][64];
    double metadata_interval;
    double spotify_interval;
    double lyric_offset;
    double transition_duration;
    double marquee_speed;
} Config;

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct {
    char *text;
    double timestamp;
} Lyric;

typedef struct {
    Lyric lines[MAX_LYRICS];
    int count;
} LyricBatch;

typedef struct {
    Song song;
    unsigned long generation;
} LyricRequest;

typedef struct {
    unsigned char *pixels;
    int width;
    int height;
    int channels;
} Image;

typedef struct {
    char url[MAX_FIELD];
    unsigned long generation;
    bool preload;
} ArtworkRequest;

typedef struct {
    char url[MAX_FIELD];
    unsigned char *data;
    size_t length;
    unsigned long used;
} ArtworkCacheEntry;

typedef struct {
    char text[MAX_FIELD];
    int width;
    int terminal_cols;
    int cell_width;
    int cell_height;
    int color_r;
    int color_g;
    int color_b;
    bool bold;
    bool uploaded;
    bool fades_uploaded;
    uint32_t image_id;
    uint32_t fade_ids[3];
    double started_at;
    double recolored_at;
    bool active;
    bool placed;
    int placed_offset;
    int placed_x;
    int placed_y;
} MarqueeState;

typedef struct {
    char *text;
    const MarqueeState *marquee;
    int x;
    int r, g, b;
    bool bold;
    bool touched;
} DetailRow;

typedef struct {
    char *text;
    int height;
} WrappedLyric;

typedef struct {
    atomic_ullong count;
    atomic_ullong total_ns;
    atomic_ullong max_ns;
} TimingMetric;

typedef struct {
    char regular[256];
    char bold[256];
    double pixel_size;
    int initial_cell_height;
    bool ready;
} KittyFont;


extern Config cfg;
extern bool cava_enabled;
extern int player_follow_fd;
extern pthread_t player_follow_thread;
extern GMainContext *player_follow_context;
extern GMainLoop *player_follow_loop;
extern bool player_follow_started;
extern int cava_values[128];
extern char next_track[MAX_FIELD];
extern pthread_mutex_t queue_mutex;
extern bool queue_pending;
extern bool queue_requested;
extern bool queue_worker_started;
extern bool queue_worker_stop;
extern unsigned long queue_generation;
extern pthread_t queue_thread;
extern pthread_cond_t queue_condition;
extern char current_playlist[MAX_FIELD];
extern char cached_playlist_uri[MAX_FIELD];
extern double playlist_checked_at;
extern Lyric lyrics[MAX_LYRICS];
extern int lyric_count;
extern bool lyrics_loaded;
extern int cover_r, cover_g, cover_b;
extern atomic_bool force_redraw;
extern int previous_rows, previous_cols;
extern DetailRow *detail_rows;
extern int detail_row_count;
extern WrappedLyric wrapped_lyrics[MAX_LYRICS];
extern unsigned long lyric_generation;
extern unsigned long wrapped_generation;
extern int wrapped_width;
extern bool wrapped_clean;
extern bool frame_changed;
extern char previous_art_url[MAX_FIELD];
extern pthread_mutex_t lyric_mutex;
extern bool lyrics_pending;
extern int lyric_event_fd;
extern char lyric_identity[MAX_FIELD];
extern unsigned long lyric_request_generation;
extern bool spotify_auth_attempted;
extern pthread_mutex_t artwork_mutex;
extern Image current_artwork;
extern char current_artwork_url[MAX_FIELD];
extern char desired_artwork_url[MAX_FIELD];
extern unsigned long artwork_generation;
extern ArtworkCacheEntry artwork_cache[ART_CACHE_SLOTS];
extern size_t artwork_cache_bytes;
extern unsigned long artwork_cache_clock;
extern char preloading_artwork_url[MAX_FIELD];
extern double preload_retry_at;
extern int active_lyric_line;
extern double lyric_transition_start;
extern MarqueeState title_marquee;
extern MarqueeState album_marquee;
extern MarqueeState playlist_marquee;
extern MarqueeState next_marquee;
extern KittyFont kitty_font;
extern bool graphics_supported;
extern bool synchronized_updates_supported;
extern int probed_cell_width;
extern int probed_cell_height;
extern GDBusConnection *mpris_bus;
extern bool profile_enabled;
extern TimingMetric metadata_timing;
extern TimingMetric artwork_timing;
extern TimingMetric lyrics_timing;
extern TimingMetric http_timing;
extern atomic_ullong http_failures;
extern pthread_key_t http_key;
extern pthread_once_t http_key_once;
extern atomic_bool workers_stopping;

bool reserve_worker(void);
void release_worker(void);
void reset_detail_rows(void);
void reset_wrapped_lyrics(void);
double monotonic_seconds(void);
void record_timing(TimingMetric *metric, double started_at);
void trim(char *value);
void set_string(char *target, size_t size, const char *value);
char *capture(char *const argv[]);
bool get_song(Song *song);
void request_song_refresh(void);
void stop_player_follow(void);
bool start_player_follow(void);
bool read_player_follow(double now);
void player_command(const Song *song, const char *action, const char *value);
void show_notice(const char *message);
extern char ui_notice[MAX_FIELD];
extern double ui_notice_until;
bool local_open_folder(const char *folder, char *error, size_t error_size);
bool local_open_path(const char *path, char *error, size_t error_size);
bool local_is_active(void);
bool local_get_song(Song *song);
bool local_update(void);
bool local_metadata_changed(void);
bool local_is_loading(void);
bool local_has_video(void);
bool local_copy_video(Image *image, unsigned long *generation);
bool local_video_color(int *red, int *green, int *blue);
bool local_command(const char *action, const char *value);
void local_close(void);
size_t curl_write(char *data, size_t size, size_t count, void *userdata);
int http_progress(void *userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded);
Buffer http_get(const char *url, const char *authorization);
void clear_lyrics(void);
void request_lyrics(const Song *song);
double next_lyric_delay(double position);
void request_artwork(const char *url);
void preload_artwork(const char *url);
void lowercase(char *text);
void detect_terminal_font(void);
void probe_terminal_features(void);
void render(const Song *song, double now);
void request_spotify_queue(void);
#endif
