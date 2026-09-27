#include "npit_internal.h"

size_t curl_write(char *data, size_t size, size_t count, void *userdata) {
    Buffer *buffer = userdata;
    size_t bytes = size * count;
    if (buffer->length + bytes + 1 > buffer->capacity) {
        size_t cap = buffer->capacity ? buffer->capacity : 4096;
        while (cap < buffer->length + bytes + 1) cap *= 2;
        unsigned char *next = realloc(buffer->data, cap);
        if (!next) return 0;
        buffer->data = next;
        buffer->capacity = cap;
    }
    memcpy(buffer->data + buffer->length, data, bytes);
    buffer->length += bytes;
    buffer->data[buffer->length] = 0;
    return bytes;
}

static void create_http_key(void) {
    pthread_key_create(&http_key, (void (*)(void *))curl_easy_cleanup);
}

static CURL *reusable_http_handle(void) {
    pthread_once(&http_key_once, create_http_key);
    CURL *curl = pthread_getspecific(http_key);
    if (!curl) {
        curl = curl_easy_init();
        if (!curl) return NULL;
        if (pthread_setspecific(http_key, curl) != 0) { curl_easy_cleanup(curl); return NULL; }
    }
    curl_easy_reset(curl);
    return curl;
}

int http_progress(void *userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    (void)userdata;
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    return atomic_load(&workers_stopping) ? 1 : 0;
}

Buffer http_get(const char *url, const char *authorization) {
    double started_at = profile_enabled ? monotonic_seconds() : 0.0;
    Buffer buffer = {0};
    CURL *curl = reusable_http_handle();
    if (!curl) return buffer;
    struct curl_slist *headers = NULL;
    if (authorization) headers = curl_slist_append(headers, authorization);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "npit/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    for (int attempt = 0; attempt < 2 && !atomic_load(&workers_stopping); attempt++) {
        free(buffer.data);
        memset(&buffer, 0, sizeof(buffer));
        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (result == CURLE_OK && ((status >= 200 && status < 300) || (status == 0 && !strncmp(url, "file://", 7)))) break;
        atomic_fetch_add(&http_failures, 1);
        if (attempt || (result == CURLE_OK && status != 429 && status < 500)) {
            free(buffer.data);
            memset(&buffer, 0, sizeof(buffer));
            break;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000L};
        nanosleep(&delay, NULL);
    }
    record_timing(&http_timing, started_at);
    curl_slist_free_all(headers);
    return buffer;
}
