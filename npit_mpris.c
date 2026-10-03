#include "npit_internal.h"

static pthread_t metadata_thread;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t metadata_condition = PTHREAD_COND_INITIALIZER;
static Song metadata_snapshot;
static double metadata_snapshot_at;
static bool metadata_snapshot_found;
static bool metadata_thread_started;
static bool metadata_stop;
static bool metadata_requested;
static pthread_mutex_t properties_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *properties_cache;
static GVariant *player_names;
static double player_names_at;
static atomic_bool player_names_dirty = true;
static atomic_ulong properties_generation;

typedef struct {
    GVariant *properties;
    char *owner;
    double fetched_at;
} PlayerProperties;

static void free_player_properties(gpointer value) {
    PlayerProperties *entry = value;
    g_variant_unref(entry->properties);
    g_free(entry->owner);
    free(entry);
}

static GVariant *cached_player_properties(const char *name, double lifetime, double *age) {
    double now = monotonic_seconds();
    unsigned long generation = atomic_load(&properties_generation);
    pthread_mutex_lock(&properties_mutex);
    if (!properties_cache) properties_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_player_properties);
    PlayerProperties *entry = g_hash_table_lookup(properties_cache, name);
    if (entry && now - entry->fetched_at < lifetime) {
        *age = now - entry->fetched_at;
        GVariant *properties = g_variant_ref(entry->properties);
        pthread_mutex_unlock(&properties_mutex);
        return properties;
    }
    char *known_owner = entry && entry->owner ? g_strdup(entry->owner) : NULL;
    pthread_mutex_unlock(&properties_mutex);
    GVariant *reply = g_dbus_connection_call_sync(mpris_bus, name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL);
    if (!reply) { g_free(known_owner); return NULL; }
    GVariant *properties;
    g_variant_get(reply, "(@a{sv})", &properties);
    g_variant_unref(reply);
    GVariant *owner_reply = known_owner ? NULL : g_dbus_connection_call_sync(mpris_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    PlayerProperties *fresh = calloc(1, sizeof(*fresh));
    if (fresh) {
        fresh->properties = g_variant_ref(properties);
        fresh->fetched_at = atomic_load(&properties_generation) == generation ? now : 0;
        fresh->owner = known_owner;
        if (owner_reply) g_variant_get(owner_reply, "(s)", &fresh->owner);
        pthread_mutex_lock(&properties_mutex);
        g_hash_table_replace(properties_cache, g_strdup(name), fresh);
        pthread_mutex_unlock(&properties_mutex);
    } else g_free(known_owner);
    if (owner_reply) g_variant_unref(owner_reply);
    *age = 0;
    return properties;
}

char *capture(char *const argv[]) {
    int pipes[2];
    if (pipe(pipes) != 0) return NULL;
    pid_t child = fork();
    if (child == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) dup2(nullfd, STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);
    if (child < 0) {
        close(pipes[0]);
        return NULL;
    }
    size_t cap = 4096, used = 0;
    char *out = malloc(cap);
    if (!out) {
        close(pipes[0]);
        waitpid(child, NULL, 0);
        return NULL;
    }
    ssize_t n;
    while ((n = read(pipes[0], out + used, cap - used - 1)) > 0) {
        used += (size_t)n;
        if (used + 1024 >= cap) {
            cap *= 2;
            char *next = realloc(out, cap);
            if (!next) {
                free(out);
                close(pipes[0]);
                waitpid(child, NULL, 0);
                return NULL;
            }
            out = next;
        }
    }
    close(pipes[0]);
    int status = 0;
    waitpid(child, &status, 0);
    out[used] = 0;
    trim(out);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

static void capture_field(char *dest, size_t size, const char *src) {
    if (src) set_string(dest, size, src);
}

static bool is_browser_player(const char *name) {
    static const char *browsers[] = {
        "firefox", "chromium", "google-chrome", "chrome", "brave", "librewolf",
        "floorp", "waterfox", "vivaldi", "opera", "microsoft-edge", "edge",
        "epiphany", "falkon", "qutebrowser", "zen", "thorium"
    };
    if (!name) return false;
    for (size_t i = 0; i < sizeof(browsers) / sizeof(browsers[0]); i++) {
        size_t length = strlen(browsers[i]);
        if (!strncasecmp(name, browsers[i], length) && (!name[length] || name[length] == '.' || name[length] == '-' || name[length] == '_')) return true;
    }
    return false;
}

static bool get_song_playerctl(Song *song) {
    char format[256] = "{{playerName}}\x1f{{xesam:title}}\x1f{{xesam:artist}}\x1f{{xesam:album}}\x1f{{xesam:trackNumber}}\x1f{{status}}\x1f{{position}}\x1f{{mpris:length}}\x1f{{volume}}\x1f{{mpris:artUrl}}\x1f{{xesam:url}}\x1f{{xesam:audioCodec}}\x1f{{xesam:playlist}}";
    char *argv[8];
    int at = 0;
    argv[at++] = "playerctl";
    if (cfg.selected_player[0]) {
        argv[at++] = "--player";
        argv[at++] = cfg.selected_player;
    } else argv[at++] = "--all-players";
    argv[at++] = "metadata";
    argv[at++] = "--format";
    argv[at++] = format;
    argv[at] = NULL;
    char *output = capture(argv);
    memset(song, 0, sizeof(*song));
    if (!output || !*output) {
        capture_field(song->title, sizeof(song->title), "no media player found");
        capture_field(song->status, sizeof(song->status), "Stopped");
        free(output);
        return false;
    }
    char chosen[MAX_FIELD * 7] = "";
    int chosen_rank = 99;
    char *save_line = NULL;
    for (char *row = strtok_r(output, "\n", &save_line); row; row = strtok_r(NULL, "\n", &save_line)) {
        char test[MAX_FIELD * 7];
        set_string(test, sizeof(test), row);
        char *scan = test;
        char *test_fields[13] = {0};
        for (int i = 0; i < 12; i++) {
            test_fields[i] = scan;
            char *mark = strchr(scan, 0x1f);
            if (!mark) { scan = NULL; break; }
            *mark = 0;
            scan = mark + 1;
        }
        if (!scan) continue;
        test_fields[12] = scan;
        int status_rank = !strcasecmp(test_fields[5], "playing") ? 0 : !strcasecmp(test_fields[5], "paused") ? 1 : !strcasecmp(test_fields[5], "stopped") ? 2 : 3;
        int rank = status_rank * 2 + (is_browser_player(test_fields[0]) ? 1 : 0);
        if (rank < chosen_rank) { set_string(chosen, sizeof(chosen), row); chosen_rank = rank; }
        if (!chosen_rank) break;
    }
    char *fields[13] = {0};
    char *cursor = chosen;
    for (int i = 0; i < 12 && cursor; i++) {
        fields[i] = cursor;
        char *mark = strchr(cursor, 0x1f);
        if (!mark) { cursor = NULL; break; }
        *mark = 0;
        cursor = mark + 1;
    }
    if (cursor) fields[12] = cursor;
    capture_field(song->player, sizeof(song->player), fields[0]);
    capture_field(song->title, sizeof(song->title), fields[1]);
    capture_field(song->artist, sizeof(song->artist), fields[2]);
    capture_field(song->album, sizeof(song->album), fields[3]);
    capture_field(song->track, sizeof(song->track), fields[4]);
    capture_field(song->status, sizeof(song->status), fields[5]);
    song->position = fields[6] ? atof(fields[6]) / 1000000.0 : 0;
    song->length = fields[7] ? atof(fields[7]) / 1000000.0 : 0;
    song->volume = fields[8] ? atof(fields[8]) * 100 : 0;
    capture_field(song->art_url, sizeof(song->art_url), fields[9]);
    capture_field(song->media_url, sizeof(song->media_url), fields[10]);
    capture_field(song->codec, sizeof(song->codec), fields[11]);
    capture_field(song->playlist, sizeof(song->playlist), fields[12]);
    if (!song->artist[0]) {
        char *split = strrchr(song->title, 0xE2);
        if (split && !strncmp(split, "\xe2\x80\xa2", 3)) {
            *split = 0;
            capture_field(song->artist, sizeof(song->artist), split + 4);
            trim(song->title);
            trim(song->artist);
        }
    }
    free(output);
    return true;
}

static void read_mpris_field(GVariant *metadata, const char *key, char *target, size_t size) {
    if (!metadata) return;
    GVariant *value = g_variant_lookup_value(metadata, key, NULL);
    if (!value) return;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
        set_string(target, size, g_variant_get_string(value, NULL));
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        snprintf(target, size, "%d", g_variant_get_int32(value));
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        snprintf(target, size, "%u", g_variant_get_uint32(value));
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) {
        snprintf(target, size, "%lld", (long long)g_variant_get_int64(value));
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) {
        snprintf(target, size, "%llu", (unsigned long long)g_variant_get_uint64(value));
    }
    g_variant_unref(value);
}

static int get_song_dbus(Song *song) {
    if (!mpris_bus) return -1;
    double now = monotonic_seconds();
    bool names_dirty = atomic_exchange(&player_names_dirty, false);
    if (!player_names || names_dirty || now - player_names_at >= 5.0) {
        GVariant *reply = g_dbus_connection_call_sync(mpris_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
        if (!reply) return -1;
        if (player_names) g_variant_unref(player_names);
        g_variant_get(reply, "(@as)", &player_names);
        player_names_at = now;
        g_variant_unref(reply);
    }
    GVariant *names = g_variant_ref(player_names);
    GVariantIter iter;
    g_variant_iter_init(&iter, names);
    const gchar *name;
    int best_rank = 99;
    bool best_has_position = false;
    Song best = {0};
    char best_bus[256] = "";
    bool saw_player = false;
    pthread_mutex_lock(&metadata_mutex);
    char active_player[128];
    set_string(active_player, sizeof(active_player), metadata_snapshot.player);
    pthread_mutex_unlock(&metadata_mutex);
    while (g_variant_iter_loop(&iter, "&s", &name)) {
        static const char prefix[] = "org.mpris.MediaPlayer2.";
        if (strncmp(name, prefix, sizeof(prefix) - 1)) continue;
        const char *player = name + sizeof(prefix) - 1;
        if (cfg.selected_player[0]) {
            size_t length = strlen(cfg.selected_player);
            if (strncasecmp(player, cfg.selected_player, length) || (player[length] && player[length] != '.')) continue;
        }
        saw_player = true;
        double properties_age = 0;
        double lifetime = !strcmp(player, active_player) ? fmax(0.2, cfg.metadata_interval > 0 ? cfg.metadata_interval : 0.5) : 5.0;
        GVariant *props = cached_player_properties(name, lifetime, &properties_age);
        if (!props) continue;
        Song candidate = {0};
        set_string(candidate.player, sizeof(candidate.player), player);
        const gchar *status = NULL;
        if (g_variant_lookup(props, "PlaybackStatus", "&s", &status)) set_string(candidate.status, sizeof(candidate.status), status);
        gint64 position = 0;
        bool has_position = g_variant_lookup(props, "Position", "x", &position);
        if (has_position) {
            candidate.position = (double)position / 1000000.0;
            if (!strcasecmp(candidate.status, "playing")) candidate.position += properties_age;
        }
        gdouble volume = 0.0;
        if (g_variant_lookup(props, "Volume", "d", &volume)) candidate.volume = volume * 100.0;
        GVariant *metadata = g_variant_lookup_value(props, "Metadata", G_VARIANT_TYPE_VARDICT);
        if (metadata) {
            read_mpris_field(metadata, "xesam:title", candidate.title, sizeof(candidate.title));
            read_mpris_field(metadata, "xesam:album", candidate.album, sizeof(candidate.album));
            read_mpris_field(metadata, "xesam:trackNumber", candidate.track, sizeof(candidate.track));
            read_mpris_field(metadata, "mpris:artUrl", candidate.art_url, sizeof(candidate.art_url));
            read_mpris_field(metadata, "xesam:url", candidate.media_url, sizeof(candidate.media_url));
            read_mpris_field(metadata, "xesam:audioCodec", candidate.codec, sizeof(candidate.codec));
            read_mpris_field(metadata, "xesam:playlist", candidate.playlist, sizeof(candidate.playlist));
            GVariant *length = g_variant_lookup_value(metadata, "mpris:length", NULL);
            if (length) {
                if (g_variant_is_of_type(length, G_VARIANT_TYPE_INT64)) candidate.length = (double)g_variant_get_int64(length) / 1000000.0;
                else if (g_variant_is_of_type(length, G_VARIANT_TYPE_UINT64)) candidate.length = (double)g_variant_get_uint64(length) / 1000000.0;
                g_variant_unref(length);
            }
            GVariant *artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE("as"));
            if (artists) {
                GVariantIter artist_iter;
                g_variant_iter_init(&artist_iter, artists);
                const gchar *artist;
                while (g_variant_iter_loop(&artist_iter, "&s", &artist)) {
                    size_t used = strlen(candidate.artist);
                    if (used && used + 2 < sizeof(candidate.artist)) { memcpy(candidate.artist + used, ", ", 2); used += 2; candidate.artist[used] = 0; }
                    if (used + 1 < sizeof(candidate.artist)) snprintf(candidate.artist + used, sizeof(candidate.artist) - used, "%s", artist);
                }
                g_variant_unref(artists);
            }
            g_variant_unref(metadata);
        }
        g_variant_unref(props);
        int status_rank = !strcasecmp(candidate.status, "playing") ? 0 : !strcasecmp(candidate.status, "paused") ? 1 : !strcasecmp(candidate.status, "stopped") ? 2 : 3;
        int rank = status_rank * 2 + (is_browser_player(candidate.player) ? 1 : 0);
        if (rank < best_rank) {
            best = candidate;
            best_rank = rank;
            best_has_position = has_position && properties_age == 0;
            set_string(best_bus, sizeof(best_bus), name);
        }
        if (!best_rank) break;
    }
    g_variant_unref(names);
    if (best_rank == 99) return saw_player ? -1 : 0;
    *song = best;
    if (!best_has_position) {
        GVariant *position_reply = g_dbus_connection_call_sync(mpris_bus, best_bus, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
        if (position_reply) {
            GVariant *position_value = NULL;
            g_variant_get(position_reply, "(@v)", &position_value);
            GVariant *unboxed = g_variant_get_variant(position_value);
            if (g_variant_is_of_type(unboxed, G_VARIANT_TYPE_INT64)) song->position = (double)g_variant_get_int64(unboxed) / 1000000.0;
            g_variant_unref(unboxed);
            g_variant_unref(position_value);
            g_variant_unref(position_reply);
        }
    }
    if (!song->artist[0]) {
        char *split = strrchr(song->title, 0xE2);
        if (split && !strncmp(split, "\xe2\x80\xa2", 3)) {
            *split = 0;
            set_string(song->artist, sizeof(song->artist), split + 4);
            trim(song->title);
            trim(song->artist);
        }
    }
    return 1;
}

static bool fetch_song(Song *song) {
    double started_at = profile_enabled ? monotonic_seconds() : 0.0;
    memset(song, 0, sizeof(*song));
    int result = get_song_dbus(song);
    bool found;
    if (result < 0) found = get_song_playerctl(song);
    else if (result > 0) found = true;
    else {
        set_string(song->title, sizeof(song->title), "no media player found");
        set_string(song->status, sizeof(song->status), "Stopped");
        found = false;
    }
    record_timing(&metadata_timing, started_at);
    return found;
}

bool get_song(Song *song) {
    if (!metadata_thread_started) return fetch_song(song);
    pthread_mutex_lock(&metadata_mutex);
    *song = metadata_snapshot;
    bool found = metadata_snapshot_found;
    double elapsed = monotonic_seconds() - metadata_snapshot_at;
    pthread_mutex_unlock(&metadata_mutex);
    if (found && !strcasecmp(song->status, "playing") && elapsed > 0) song->position += elapsed;
    return found;
}

void request_song_refresh(void) {
    pthread_mutex_lock(&metadata_mutex);
    metadata_requested = true;
    pthread_cond_signal(&metadata_condition);
    pthread_mutex_unlock(&metadata_mutex);
}

static void *metadata_loop(void *userdata) {
    (void)userdata;
    for (;;) {
        pthread_mutex_lock(&metadata_mutex);
        while (!metadata_requested && !metadata_stop) pthread_cond_wait(&metadata_condition, &metadata_mutex);
        if (metadata_stop) { pthread_mutex_unlock(&metadata_mutex); break; }
        metadata_requested = false;
        pthread_mutex_unlock(&metadata_mutex);
        Song fresh;
        bool found = fetch_song(&fresh);
        double received_at = monotonic_seconds();
        pthread_mutex_lock(&metadata_mutex);
        metadata_snapshot = fresh;
        metadata_snapshot_found = found;
        metadata_snapshot_at = received_at;
        pthread_mutex_unlock(&metadata_mutex);
        uint64_t event = 1;
        if (player_follow_fd >= 0) (void)write(player_follow_fd, &event, sizeof(event));
    }
    return NULL;
}

static void player_signal(GDBusConnection *connection, const gchar *sender, const gchar *path, const gchar *interface, const gchar *signal, GVariant *parameters, gpointer userdata) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)userdata;
    if (!strcmp(interface, "org.freedesktop.DBus") && !strcmp(signal, "NameOwnerChanged")) {
        const gchar *name, *old_owner, *new_owner;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23)) return;
        atomic_fetch_add(&properties_generation, 1);
        atomic_store(&player_names_dirty, true);
        pthread_mutex_lock(&properties_mutex);
        if (properties_cache) g_hash_table_remove(properties_cache, name);
        pthread_mutex_unlock(&properties_mutex);
    } else if (!strcmp(interface, "org.freedesktop.DBus.Properties")) {
        const gchar *changed_interface;
        g_variant_get_child(parameters, 0, "&s", &changed_interface);
        if (strcmp(changed_interface, "org.mpris.MediaPlayer2.Player")) return;
        atomic_fetch_add(&properties_generation, 1);
        GVariant *changed = g_variant_get_child_value(parameters, 1);
        GVariant *invalidated = g_variant_get_child_value(parameters, 2);
        pthread_mutex_lock(&properties_mutex);
        if (properties_cache) {
            GHashTableIter entries;
            gpointer value;
            g_hash_table_iter_init(&entries, properties_cache);
            while (g_hash_table_iter_next(&entries, NULL, &value)) {
                PlayerProperties *entry = value;
                if (!entry->owner || strcmp(entry->owner, sender)) continue;
                GVariantDict dictionary;
                g_variant_dict_init(&dictionary, entry->properties);
                GVariantIter fields;
                const gchar *key;
                GVariant *field;
                g_variant_iter_init(&fields, changed);
                while (g_variant_iter_next(&fields, "{&sv}", &key, &field)) {
                    g_variant_dict_insert_value(&dictionary, key, field);
                    g_variant_unref(field);
                }
                if (g_variant_n_children(invalidated)) entry->fetched_at = 0;
                g_variant_unref(entry->properties);
                entry->properties = g_variant_ref_sink(g_variant_dict_end(&dictionary));
            }
        }
        pthread_mutex_unlock(&properties_mutex);
        g_variant_unref(changed);
        g_variant_unref(invalidated);
    }
    request_song_refresh();
}

static void *player_signal_loop(void *userdata) {
    (void)userdata;
    g_main_context_push_thread_default(player_follow_context);
    guint properties = g_dbus_connection_signal_subscribe(mpris_bus, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", G_DBUS_SIGNAL_FLAGS_NONE, player_signal, NULL, NULL);
    guint seeked = g_dbus_connection_signal_subscribe(mpris_bus, NULL, "org.mpris.MediaPlayer2.Player", "Seeked", "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE, player_signal, NULL, NULL);
    guint owners = g_dbus_connection_signal_subscribe(mpris_bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, player_signal, NULL, NULL);
    g_main_loop_run(player_follow_loop);
    g_dbus_connection_signal_unsubscribe(mpris_bus, properties);
    g_dbus_connection_signal_unsubscribe(mpris_bus, seeked);
    g_dbus_connection_signal_unsubscribe(mpris_bus, owners);
    g_main_context_pop_thread_default(player_follow_context);
    return NULL;
}

static gboolean quit_player_signal_loop(gpointer userdata) {
    g_main_loop_quit(userdata);
    return G_SOURCE_REMOVE;
}

void stop_player_follow(void) {
    if (metadata_thread_started) {
        pthread_mutex_lock(&metadata_mutex);
        metadata_stop = true;
        pthread_cond_signal(&metadata_condition);
        pthread_mutex_unlock(&metadata_mutex);
        pthread_join(metadata_thread, NULL);
        metadata_thread_started = false;
    }
    if (player_follow_started) {
        GSource *quit = g_idle_source_new();
        g_source_set_callback(quit, quit_player_signal_loop, player_follow_loop, NULL);
        g_source_attach(quit, player_follow_context);
        g_source_unref(quit);
        pthread_join(player_follow_thread, NULL);
        player_follow_started = false;
    }
    if (player_follow_loop) { g_main_loop_unref(player_follow_loop); player_follow_loop = NULL; }
    if (player_follow_context) { g_main_context_unref(player_follow_context); player_follow_context = NULL; }
    if (player_follow_fd >= 0) { close(player_follow_fd); player_follow_fd = -1; }
    pthread_mutex_lock(&properties_mutex);
    if (properties_cache) { g_hash_table_unref(properties_cache); properties_cache = NULL; }
    pthread_mutex_unlock(&properties_mutex);
    if (player_names) { g_variant_unref(player_names); player_names = NULL; }
    atomic_store(&player_names_dirty, true);
}

bool start_player_follow(void) {
    if (metadata_thread_started) return true;
    player_follow_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (player_follow_fd < 0) return false;
    pthread_mutex_lock(&metadata_mutex);
    metadata_stop = false;
    metadata_requested = true;
    pthread_mutex_unlock(&metadata_mutex);
    if (pthread_create(&metadata_thread, NULL, metadata_loop, NULL) != 0) {
        stop_player_follow();
        return false;
    }
    metadata_thread_started = true;
    if (!mpris_bus) return true;
    player_follow_context = g_main_context_new();
    if (!player_follow_context) { stop_player_follow(); return false; }
    player_follow_loop = g_main_loop_new(player_follow_context, FALSE);
    if (!player_follow_loop || pthread_create(&player_follow_thread, NULL, player_signal_loop, NULL) != 0) {
        stop_player_follow();
        return false;
    }
    player_follow_started = true;
    return true;
}

bool read_player_follow(double now) {
    (void)now;
    uint64_t events;
    return player_follow_fd >= 0 && read(player_follow_fd, &events, sizeof(events)) == sizeof(events);
}

static void player_command_fallback(const Song *song, const char *action, const char *value) {
    char *argv[8];
    int at = 0;
    argv[at++] = "playerctl";
    argv[at++] = "--player";
    argv[at++] = (char *)song->player;
    argv[at++] = (char *)action;
    if (value) argv[at++] = (char *)value;
    argv[at] = NULL;
    char *result = capture(argv);
    free(result);
}

static GVariant *player_property(const char *bus_name, const char *property) {
    GVariant *reply = g_dbus_connection_call_sync(mpris_bus, bus_name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", property), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL);
    if (!reply) return NULL;
    GVariant *wrapped = NULL;
    g_variant_get(reply, "(@v)", &wrapped);
    GVariant *value = g_variant_get_variant(wrapped);
    g_variant_unref(wrapped);
    g_variant_unref(reply);
    return value;
}

static void set_player_property(const char *bus_name, const char *property, GVariant *value) {
    GVariant *reply = g_dbus_connection_call_sync(mpris_bus, bus_name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Set", g_variant_new("(ssv)", "org.mpris.MediaPlayer2.Player", property, value), G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL);
    if (reply) g_variant_unref(reply);
}

void player_command(const Song *song, const char *action, const char *value) {
    if (local_is_active()) { local_command(action, value); return; }
    if (!song->player[0]) return;
    if (!mpris_bus) {
        const char *fallback_value = value;
        if (!strcmp(action, "seek")) {
            player_command_fallback(song, "position", value && !strcmp(value, "back") ? "5-" : "5+");
            return;
        }
        if (!strcmp(action, "volume")) fallback_value = value && !strcmp(value, "up") ? "0.05+" : "0.05-";
        player_command_fallback(song, action, fallback_value);
        return;
    }
    char bus_name[256];
    snprintf(bus_name, sizeof(bus_name), "org.mpris.MediaPlayer2.%s", song->player);
    const char *method = !strcmp(action, "play-pause") ? "PlayPause" : !strcmp(action, "next") ? "Next" : !strcmp(action, "previous") ? "Previous" : NULL;
    if (method) {
        GVariant *reply = g_dbus_connection_call_sync(mpris_bus, bus_name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", method, NULL, G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL);
        if (reply) g_variant_unref(reply);
    } else if (!strcmp(action, "seek")) {
        gint64 offset = value && !strcmp(value, "back") ? -5000000 : 5000000;
        GVariant *reply = g_dbus_connection_call_sync(mpris_bus, bus_name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Seek", g_variant_new("(x)", offset), G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL, NULL);
        if (reply) g_variant_unref(reply);
    } else if (!strcmp(action, "shuffle")) {
        GVariant *current = player_property(bus_name, "Shuffle");
        if (current) {
            if (g_variant_is_of_type(current, G_VARIANT_TYPE_BOOLEAN)) set_player_property(bus_name, "Shuffle", g_variant_new_boolean(!g_variant_get_boolean(current)));
            g_variant_unref(current);
        }
    } else if (!strcmp(action, "loop")) {
        GVariant *current = player_property(bus_name, "LoopStatus");
        if (current) {
            if (g_variant_is_of_type(current, G_VARIANT_TYPE_STRING)) {
                const char *status = g_variant_get_string(current, NULL);
                const char *next = !strcmp(status, "None") ? "Playlist" : !strcmp(status, "Playlist") ? "Track" : "None";
                set_player_property(bus_name, "LoopStatus", g_variant_new_string(next));
            }
            g_variant_unref(current);
        }
    } else if (!strcmp(action, "volume")) {
        GVariant *current = player_property(bus_name, "Volume");
        if (current) {
            if (g_variant_is_of_type(current, G_VARIANT_TYPE_DOUBLE)) {
                double step = value && !strcmp(value, "up") ? 0.05 : -0.05;
                double volume = fmax(0.0, fmin(1.0, g_variant_get_double(current) + step));
                set_player_property(bus_name, "Volume", g_variant_new_double(volume));
            }
            g_variant_unref(current);
        }
    }
}
