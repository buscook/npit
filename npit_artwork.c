#include "npit_internal.h"

static bool artwork_pending;
static double artwork_retry_at;
static int artwork_attempts;

static bool decode_png(const unsigned char *data, size_t length, Image *image) {
    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data, length)) return false;
    png.format = PNG_FORMAT_RGB;
    size_t bytes = PNG_IMAGE_SIZE(png);
    image->pixels = malloc(bytes);
    if (!image->pixels || !png_image_finish_read(&png, NULL, image->pixels, 0, NULL)) {
        free(image->pixels);
        image->pixels = NULL;
        png_image_free(&png);
        return false;
    }
    image->width = (int)png.width;
    image->height = (int)png.height;
    image->channels = 3;
    png_image_free(&png);
    return true;
}

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegError;

static void jpeg_fail(j_common_ptr common) {
    JpegError *error = (JpegError *)common->err;
    longjmp(error->jump, 1);
}

static bool decode_jpeg(const unsigned char *data, size_t length, Image *image) {
    struct jpeg_decompress_struct jpeg;
    JpegError error;
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_fail;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&jpeg);
        free(image->pixels);
        memset(image, 0, sizeof(*image));
        return false;
    }
    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, data, length);
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;
    jpeg_start_decompress(&jpeg);
    image->width = (int)jpeg.output_width;
    image->height = (int)jpeg.output_height;
    image->channels = 3;
    size_t bytes = (size_t)image->width * (size_t)image->height * 3;
    image->pixels = malloc(bytes);
    if (!image->pixels) {
        jpeg_destroy_decompress(&jpeg);
        return false;
    }
    while (jpeg.output_scanline < jpeg.output_height) {
        JSAMPROW row = image->pixels + (size_t)jpeg.output_scanline * (size_t)image->width * 3;
        jpeg_read_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_decompress(&jpeg);
    bool complete = error.base.num_warnings == 0;
    jpeg_destroy_decompress(&jpeg);
    if (!complete) { free(image->pixels); memset(image, 0, sizeof(*image)); }
    return complete;
}

static bool decode_image(const unsigned char *data, size_t length, Image *image) {
    memset(image, 0, sizeof(*image));
    if (length > 8 && !png_sig_cmp((png_bytep)data, 0, 8)) return decode_png(data, length, image);
    return decode_jpeg(data, length, image);
}

static Buffer cached_art_bytes(const char *url) {
    Buffer copy = {0};
    pthread_mutex_lock(&artwork_mutex);
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        ArtworkCacheEntry *entry = &artwork_cache[i];
        if (!entry->data || strcmp(entry->url, url)) continue;
        copy.data = malloc(entry->length + 1);
        if (copy.data) {
            memcpy(copy.data, entry->data, entry->length);
            copy.data[entry->length] = 0;
            copy.length = entry->length;
            copy.capacity = entry->length + 1;
            entry->used = ++artwork_cache_clock;
        }
        break;
    }
    pthread_mutex_unlock(&artwork_mutex);
    return copy;
}

static void cache_art_bytes(const char *url, Buffer *buffer) {
    if (!buffer->data || !buffer->length || buffer->length > ART_CACHE_BYTES || strncmp(url, "https://", 8)) return;
    pthread_mutex_lock(&artwork_mutex);
    int slot = -1;
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        if (artwork_cache[i].data && !strcmp(artwork_cache[i].url, url)) {
            artwork_cache[i].used = ++artwork_cache_clock;
            pthread_mutex_unlock(&artwork_mutex);
            return;
        }
        if (!artwork_cache[i].data && slot < 0) slot = i;
    }
    while (slot < 0 || artwork_cache_bytes + buffer->length > ART_CACHE_BYTES) {
        int oldest = -1;
        for (int i = 0; i < ART_CACHE_SLOTS; i++) {
            if (artwork_cache[i].data && (oldest < 0 || artwork_cache[i].used < artwork_cache[oldest].used)) oldest = i;
        }
        if (oldest < 0) break;
        artwork_cache_bytes -= artwork_cache[oldest].length;
        free(artwork_cache[oldest].data);
        memset(&artwork_cache[oldest], 0, sizeof(artwork_cache[oldest]));
        slot = oldest;
    }
    if (slot >= 0) {
        ArtworkCacheEntry *entry = &artwork_cache[slot];
        set_string(entry->url, sizeof(entry->url), url);
        entry->data = buffer->data;
        entry->length = buffer->length;
        entry->used = ++artwork_cache_clock;
        artwork_cache_bytes += buffer->length;
        memset(buffer, 0, sizeof(*buffer));
    }
    pthread_mutex_unlock(&artwork_mutex);
}

static Image load_art(const char *url) {
    Image image = {0};
    if (!url || !*url) return image;
    Buffer buffer = cached_art_bytes(url);
    bool cached = buffer.data != NULL;
    if (!cached) buffer = http_get(url, NULL);
    if (buffer.data && decode_image(buffer.data, buffer.length, &image) && !cached) cache_art_bytes(url, &buffer);
    free(buffer.data);
    return image;
}

static void update_cover_color(const Image *image);

static void free_image(Image *image) {
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

static void *artwork_worker(void *arg) {
    ArtworkRequest *request = arg;
    double started_at = profile_enabled ? monotonic_seconds() : 0.0;
    Image image = load_art(request->url);
    record_timing(&artwork_timing, started_at);
    pthread_mutex_lock(&artwork_mutex);
    if (request->preload) {
        if (!strcmp(preloading_artwork_url, request->url)) {
            preloading_artwork_url[0] = 0;
            if (!image.pixels) preload_retry_at = monotonic_seconds() + 10.0;
        }
    } else if (request->generation == artwork_generation) {
        artwork_pending = false;
        artwork_retry_at = monotonic_seconds() + 5.0;
        free_image(&current_artwork);
        current_artwork = image;
        memset(&image, 0, sizeof(image));
        set_string(current_artwork_url, sizeof(current_artwork_url), request->url);
        update_cover_color(&current_artwork);
        force_redraw = true;
    }
    pthread_mutex_unlock(&artwork_mutex);
    free_image(&image);
    free(request);
    release_worker();
    return NULL;
}

static void artwork_request_failed(const char *url, unsigned long generation) {
    pthread_mutex_lock(&artwork_mutex);
    if (generation == artwork_generation) {
        artwork_pending = false;
        artwork_retry_at = monotonic_seconds() + 5.0;
        set_string(current_artwork_url, sizeof(current_artwork_url), url);
        force_redraw = true;
    }
    pthread_mutex_unlock(&artwork_mutex);
}

void request_artwork(const char *url) {
    pthread_mutex_lock(&artwork_mutex);
    bool same = !strcmp(url ? url : "", desired_artwork_url);
    if (same && (!url || !*url || artwork_pending || current_artwork.pixels || artwork_attempts >= 3 || monotonic_seconds() < artwork_retry_at)) { pthread_mutex_unlock(&artwork_mutex); return; }
    if (!same) artwork_attempts = 0;
    artwork_attempts++;
    set_string(desired_artwork_url, sizeof(desired_artwork_url), url ? url : "");
    artwork_generation++;
    unsigned long generation = artwork_generation;
    free_image(&current_artwork);
    current_artwork_url[0] = 0;
    if (!url || !*url) { pthread_mutex_unlock(&artwork_mutex); force_redraw = true; return; }
    artwork_pending = true;
    pthread_mutex_unlock(&artwork_mutex);
    ArtworkRequest *request = malloc(sizeof(*request));
    if (!request) { artwork_request_failed(url, generation); return; }
    set_string(request->url, sizeof(request->url), url);
    request->generation = generation;
    request->preload = false;
    pthread_t thread;
    if (!reserve_worker()) { artwork_request_failed(url, generation); free(request); return; }
    if (pthread_create(&thread, NULL, artwork_worker, request) == 0) pthread_detach(thread);
    else { artwork_request_failed(url, generation); release_worker(); free(request); }
}

void preload_artwork(const char *url) {
    if (!url || !*url || strncmp(url, "https://", 8)) return;
    pthread_mutex_lock(&artwork_mutex);
    bool skip = !strcmp(url, desired_artwork_url) || !strcmp(url, preloading_artwork_url) || monotonic_seconds() < preload_retry_at || !current_artwork.pixels;
    for (int i = 0; i < ART_CACHE_SLOTS && !skip; i++) if (artwork_cache[i].data && !strcmp(artwork_cache[i].url, url)) skip = true;
    if (skip) { pthread_mutex_unlock(&artwork_mutex); return; }
    set_string(preloading_artwork_url, sizeof(preloading_artwork_url), url);
    pthread_mutex_unlock(&artwork_mutex);
    ArtworkRequest *request = calloc(1, sizeof(*request));
    if (!request) {
        pthread_mutex_lock(&artwork_mutex);
        if (!strcmp(preloading_artwork_url, url)) preloading_artwork_url[0] = 0;
        pthread_mutex_unlock(&artwork_mutex);
        return;
    }
    set_string(request->url, sizeof(request->url), url);
    request->preload = true;
    pthread_t thread;
    if (!reserve_worker()) { free(request); return; }
    if (pthread_create(&thread, NULL, artwork_worker, request) == 0) pthread_detach(thread);
    else {
        release_worker();
        free(request);
        pthread_mutex_lock(&artwork_mutex);
        if (!strcmp(preloading_artwork_url, url)) preloading_artwork_url[0] = 0;
        pthread_mutex_unlock(&artwork_mutex);
    }
}

static void update_cover_color(const Image *image) {
    if (!image->pixels) return;
    uint64_t sums[3] = {0};
    int samples = 0;
    int step_x = image->width > 64 ? image->width / 64 : 1;
    int step_y = image->height > 64 ? image->height / 64 : 1;
    for (int y = 0; y < image->height; y += step_y) {
        for (int x = 0; x < image->width; x += step_x) {
            size_t at = ((size_t)y * (size_t)image->width + (size_t)x) * 3;
            sums[0] += image->pixels[at];
            sums[1] += image->pixels[at + 1];
            sums[2] += image->pixels[at + 2];
            samples++;
        }
    }
    if (!samples) return;
    cover_r = (int)(sums[0] / (uint64_t)samples);
    cover_g = (int)(sums[1] / (uint64_t)samples);
    cover_b = (int)(sums[2] / (uint64_t)samples);
    int brightness = (int)(0.2126 * cover_r + 0.7152 * cover_g + 0.0722 * cover_b);
    if (brightness < 150) {
        int add = 150 - brightness;
        cover_r = cover_r + add > 255 ? 255 : cover_r + add;
        cover_g = cover_g + add > 255 ? 255 : cover_g + add;
        cover_b = cover_b + add > 255 ? 255 : cover_b + add;
    }
}
