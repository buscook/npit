#include "npit_internal.h"

static char *base64url(const unsigned char *input, int length) {
    size_t size = 4 * ((size_t)length + 2) / 3;
    char *output = malloc(size + 1);
    if (!output) return NULL;
    EVP_EncodeBlock((unsigned char *)output, input, length);
    for (char *p = output; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
    }
    char *padding = strchr(output, '=');
    if (padding) *padding = 0;
    return output;
}

static void spotify_token_path(char *path, size_t path_size) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char directory[1024];
    if (xdg && *xdg) snprintf(directory, sizeof(directory), "%s/npit", xdg);
    else snprintf(directory, sizeof(directory), "%s/.cache/npit", home ? home : ".");
    mkdir(directory, 0700);
    snprintf(path, path_size, "%s/spotify-token-cache", directory);
}

static bool store_token_response(const char *body, const char *refresh_fallback) {
    struct json_object *payload = json_tokener_parse(body);
    struct json_object *access = NULL, *refresh = NULL, *expires = NULL;
    if (!payload || !json_object_object_get_ex(payload, "access_token", &access)) {
        if (payload) json_object_put(payload);
        return false;
    }
    if (!json_object_object_get_ex(payload, "refresh_token", &refresh) && refresh_fallback) {
        json_object_object_add(payload, "refresh_token", json_object_new_string(refresh_fallback));
    }
    int lifetime = 3600;
    if (json_object_object_get_ex(payload, "expires_in", &expires)) lifetime = json_object_get_int(expires);
    json_object_object_add(payload, "expires_at", json_object_new_int64((int64_t)time(NULL) + lifetime));
    char path[1200];
    spotify_token_path(path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (file) {
        fchmod(fileno(file), 0600);
        fputs(json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN), file);
        fclose(file);
    }
    json_object_put(payload);
    return file != NULL;
}

static bool spotify_token_request(const char *fields, const char *refresh_fallback) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    Buffer body = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "npit/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = result == CURLE_OK && status >= 200 && status < 300 && body.data && store_token_response((char *)body.data, refresh_fallback);
    free(body.data);
    curl_easy_cleanup(curl);
    return ok;
}

static bool spotify_authorize(void) {
    const char *client_id = getenv("SPOTIPY_CLIENT_ID");
    const char *redirect = getenv("SPOTIPY_REDIRECT_URI");
    if (!client_id || !*client_id) return false;
    if (!redirect || !*redirect) redirect = "http://127.0.0.1:8888/callback";
    const char *colon = strrchr(redirect, ':');
    int port = colon ? atoi(colon + 1) : 8888;
    if (port < 1 || port > 65535) return false;
    unsigned char random_bytes[48], digest[SHA256_DIGEST_LENGTH];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) return false;
    char *verifier = base64url(random_bytes, sizeof(random_bytes));
    SHA256((unsigned char *)verifier, strlen(verifier), digest);
    char *challenge = base64url(digest, sizeof(digest));
    unsigned char state_bytes[24];
    RAND_bytes(state_bytes, sizeof(state_bytes));
    char *state = base64url(state_bytes, sizeof(state_bytes));
    CURL *curl = curl_easy_init();
    if (!curl || !verifier || !challenge || !state) { if (curl) curl_easy_cleanup(curl); free(verifier); free(challenge); free(state); return false; }
    char *escaped_client = curl_easy_escape(curl, client_id, 0);
    char *escaped_redirect = curl_easy_escape(curl, redirect, 0);
    char url[4096];
    snprintf(url, sizeof(url), "https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&code_challenge_method=S256&code_challenge=%s&state=%s&scope=user-read-currently-playing%%20user-read-playback-state%%20playlist-read-private%%20playlist-read-collaborative", escaped_client, escaped_redirect, challenge, state);
    if (escaped_client) curl_free(escaped_client);
    if (escaped_redirect) curl_free(escaped_redirect);
    curl_easy_cleanup(curl);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { free(verifier); free(challenge); free(state); return false; }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 1) != 0) { close(server); free(verifier); free(challenge); free(state); return false; }
    pid_t browser = fork();
    if (browser == 0) { execlp("xdg-open", "xdg-open", url, (char *)NULL); _exit(127); }
    if (browser > 0) waitpid(browser, NULL, WNOHANG);
    struct pollfd waiting = {.fd = server, .events = POLLIN};
    int ready = 0;
    for (int elapsed = 0; elapsed < 180000 && !atomic_load(&workers_stopping); elapsed += 250) {
        ready = poll(&waiting, 1, 250);
        if (ready != 0) break;
    }
    int client = ready > 0 ? accept(server, NULL, NULL) : -1;
    close(server);
    if (client < 0) { free(verifier); free(challenge); free(state); return false; }
    struct timeval receive_timeout = {.tv_sec = 2};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    char request[8192] = {0};
    ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    const char response[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<!doctype html><title>npit authorization</title><p>Authorization received. You can close this tab and return to npit.</p>";
    send(client, response, sizeof(response) - 1, 0);
    close(client);
    char code_value[4096] = "", state_value[256] = "";
    curl = curl_easy_init();
    if (received > 0 && curl) {
        char *query = strchr(request, '?');
        if (query) {
            char *end = strchr(query, ' ');
            if (end) *end = 0;
            char *save = NULL;
            for (char *item = strtok_r(query + 1, "&", &save); item; item = strtok_r(NULL, "&", &save)) {
                if (!strncmp(item, "code=", 5)) { char *value = curl_easy_unescape(curl, item + 5, 0, NULL); if (value) { set_string(code_value, sizeof(code_value), value); curl_free(value); } }
                if (!strncmp(item, "state=", 6)) { char *value = curl_easy_unescape(curl, item + 6, 0, NULL); if (value) { set_string(state_value, sizeof(state_value), value); curl_free(value); } }
            }
        }
    }
    if (curl) curl_easy_cleanup(curl);
    bool ok = false;
    if (code_value[0] && !strcmp(state, state_value)) {
        curl = curl_easy_init();
        if (curl) {
            char *code = curl_easy_escape(curl, code_value, 0);
            escaped_client = curl_easy_escape(curl, client_id, 0);
            escaped_redirect = curl_easy_escape(curl, redirect, 0);
            char *escaped_verifier = curl_easy_escape(curl, verifier, 0);
            char fields[8192];
            snprintf(fields, sizeof(fields), "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s", code, escaped_redirect, escaped_client, escaped_verifier);
            ok = spotify_token_request(fields, NULL);
            if (code) curl_free(code);
            if (escaped_client) curl_free(escaped_client);
            if (escaped_redirect) curl_free(escaped_redirect);
            if (escaped_verifier) curl_free(escaped_verifier);
            curl_easy_cleanup(curl);
        }
    }
    free(verifier); free(challenge); free(state);
    return ok;
}

static void update_spotify_playlist(const char *authorization) {
    double now = monotonic_seconds();
    if (now - playlist_checked_at < 5.0) return;
    playlist_checked_at = now;
    Buffer playback = http_get("https://api.spotify.com/v1/me/player", authorization);
    if (!playback.data) return;
    struct json_object *root = json_tokener_parse((char *)playback.data);
    struct json_object *context = NULL, *type = NULL, *uri = NULL;
    char new_uri[MAX_FIELD] = "";
    if (root && json_object_object_get_ex(root, "context", &context) && context && json_object_get_type(context) == json_type_object && json_object_object_get_ex(context, "type", &type) && !strcmp(json_object_get_string(type), "playlist") && json_object_object_get_ex(context, "uri", &uri)) set_string(new_uri, sizeof(new_uri), json_object_get_string(uri));
    if (root) json_object_put(root);
    free(playback.data);
    if (!new_uri[0]) {
        cached_playlist_uri[0] = 0;
        pthread_mutex_lock(&queue_mutex);
        current_playlist[0] = 0;
        pthread_mutex_unlock(&queue_mutex);
        return;
    }
    if (!strcmp(new_uri, cached_playlist_uri)) return;
    pthread_mutex_lock(&queue_mutex);
    current_playlist[0] = 0;
    pthread_mutex_unlock(&queue_mutex);
    const char *id = strrchr(new_uri, ':');
    if (!id || !id[1]) return;
    char url[1024];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/playlists/%.200s?fields=name", id + 1);
    Buffer response = http_get(url, authorization);
    if (!response.data) return;
    root = json_tokener_parse((char *)response.data);
    struct json_object *name = NULL;
    char playlist_name[MAX_FIELD] = "";
    if (root && json_object_object_get_ex(root, "name", &name)) set_string(playlist_name, sizeof(playlist_name), json_object_get_string(name));
    if (playlist_name[0]) {
        set_string(cached_playlist_uri, sizeof(cached_playlist_uri), new_uri);
        pthread_mutex_lock(&queue_mutex);
        set_string(current_playlist, sizeof(current_playlist), playlist_name);
        pthread_mutex_unlock(&queue_mutex);
    }
    if (root) json_object_put(root);
    free(response.data);
}

static bool get_next_spotify(char *result_text, size_t result_size, bool *received) {
    result_text[0] = 0;
    *received = false;
    char next_art_url[MAX_FIELD] = "";
    char path[1024];
    spotify_token_path(path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) {
        if (!spotify_auth_attempted) { spotify_auth_attempted = true; return spotify_authorize() && get_next_spotify(result_text, result_size, received); }
        return false;
    }
    char content[16384];
    size_t size = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    content[size] = 0;
    struct json_object *root = json_tokener_parse(content);
    if (!root) return false;
    struct json_object *token_obj = NULL;
    const char *token = NULL;
    if (json_object_object_get_ex(root, "access_token", &token_obj)) token = json_object_get_string(token_obj);
    struct json_object *expiry_obj = NULL;
    if (json_object_object_get_ex(root, "expires_at", &expiry_obj) && json_object_get_int64(expiry_obj) < (int64_t)time(NULL) + 30) {
        struct json_object *refresh_obj = NULL;
        const char *refresh_token = NULL;
        const char *client_id = getenv("SPOTIPY_CLIENT_ID");
        if (json_object_object_get_ex(root, "refresh_token", &refresh_obj)) refresh_token = json_object_get_string(refresh_obj);
        if (refresh_token && client_id && *client_id) {
            CURL *curl = curl_easy_init();
            if (curl) {
                char *escaped_refresh = curl_easy_escape(curl, refresh_token, 0);
                char *escaped_client = curl_easy_escape(curl, client_id, 0);
                char post[8192];
                snprintf(post, sizeof(post), "grant_type=refresh_token&refresh_token=%s&client_id=%s", escaped_refresh ? escaped_refresh : "", escaped_client ? escaped_client : "");
                Buffer response = {0};
                curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "npit/" APP_VERSION);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                CURLcode refresh_result = curl_easy_perform(curl);
                long refresh_status = 0;
                if (refresh_result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &refresh_status);
                if (refresh_result == CURLE_OK && refresh_status >= 200 && refresh_status < 300 && response.data) {
                    struct json_object *refresh = json_tokener_parse((char *)response.data);
                    struct json_object *new_access = NULL, *expires = NULL, *new_refresh = NULL;
                    if (refresh && json_object_object_get_ex(refresh, "access_token", &new_access)) {
                        json_object_object_add(root, "access_token", json_object_new_string(json_object_get_string(new_access)));
                        int lifetime = 3600;
                        if (json_object_object_get_ex(refresh, "expires_in", &expires)) lifetime = json_object_get_int(expires);
                        json_object_object_add(root, "expires_at", json_object_new_int64((int64_t)time(NULL) + lifetime));
                        if (json_object_object_get_ex(refresh, "refresh_token", &new_refresh)) json_object_object_add(root, "refresh_token", json_object_new_string(json_object_get_string(new_refresh)));
                        json_object_object_get_ex(root, "access_token", &token_obj);
                        token = json_object_get_string(token_obj);
                        file = fopen(path, "w");
                        if (file) { fchmod(fileno(file), 0600); fputs(json_object_to_json_string(root), file); fclose(file); }
                    }
                    if (refresh) json_object_put(refresh);
                }
                free(response.data);
                if (escaped_refresh) curl_free(escaped_refresh);
                if (escaped_client) curl_free(escaped_client);
                curl_easy_cleanup(curl);
            }
        }
    }
    if (!token) { json_object_put(root); return false; }
    char auth[4096];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    update_spotify_playlist(auth);
    Buffer response = http_get("https://api.spotify.com/v1/me/player/queue", auth);
    bool found = false;
    if (response.data) {
        struct json_object *queue = json_tokener_parse((char *)response.data);
        *received = queue && json_object_get_type(queue) == json_type_object;
        struct json_object *items = NULL, *item = NULL, *name = NULL, *artists = NULL, *artist = NULL, *artist_name = NULL;
        if (queue && json_object_object_get_ex(queue, "queue", &items) && json_object_array_length(items)) {
            item = json_object_array_get_idx(items, 0);
            if (json_object_object_get_ex(item, "name", &name)) set_string(result_text, result_size, json_object_get_string(name));
            if (json_object_object_get_ex(item, "artists", &artists) && json_object_array_length(artists)) {
                artist = json_object_array_get_idx(artists, 0);
                if (json_object_object_get_ex(artist, "name", &artist_name)) {
                    size_t used = strlen(result_text);
                    snprintf(result_text + used, result_size - used, " — %.1800s", json_object_get_string(artist_name));
                }
            }
            struct json_object *album = NULL, *images = NULL, *image = NULL, *image_url = NULL;
            if (json_object_object_get_ex(item, "album", &album) && album) json_object_object_get_ex(album, "images", &images);
            else json_object_object_get_ex(item, "images", &images);
            if (images && json_object_get_type(images) == json_type_array && json_object_array_length(images)) {
                image = json_object_array_get_idx(images, 0);
                if (image && json_object_object_get_ex(image, "url", &image_url) && json_object_get_type(image_url) == json_type_string) {
                    set_string(next_art_url, sizeof(next_art_url), json_object_get_string(image_url));
                }
            }
            found = true;
        }
        if (queue) json_object_put(queue);
    }
    free(response.data);
    json_object_put(root);
    preload_artwork(next_art_url);
    return found;
}

static void *spotify_queue_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (!queue_requested && !queue_worker_stop) pthread_cond_wait(&queue_condition, &queue_mutex);
        if (queue_worker_stop) { pthread_mutex_unlock(&queue_mutex); break; }
        queue_requested = false;
        unsigned long generation = queue_generation;
        pthread_mutex_unlock(&queue_mutex);
        char result[MAX_FIELD] = "";
        bool received = false;
        get_next_spotify(result, sizeof(result), &received);
        pthread_mutex_lock(&queue_mutex);
        if (generation == queue_generation && received) set_string(next_track, sizeof(next_track), result);
        queue_pending = false;
        pthread_mutex_unlock(&queue_mutex);
    }
    return NULL;
}

void request_spotify_queue(void) {
    pthread_mutex_lock(&queue_mutex);
    if (queue_pending) { pthread_mutex_unlock(&queue_mutex); return; }
    if (!queue_worker_started) {
        if (pthread_create(&queue_thread, NULL, spotify_queue_worker, NULL) != 0) { pthread_mutex_unlock(&queue_mutex); return; }
        queue_worker_started = true;
    }
    queue_pending = true;
    queue_requested = true;
    pthread_cond_signal(&queue_condition);
    pthread_mutex_unlock(&queue_mutex);
}
