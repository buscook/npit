#include "npit_internal.h"

static void color_for(const Song *song, int x, int y, int *r, int *g, int *b) {
    if (!strcmp(cfg.theme, "rainbow")) {
        double hue = fmod(monotonic_seconds() / 8.0 + x / 300.0 + y / 600.0, 1.0);
        double h = hue * 6.0;
        double c = 255.0;
        double q = c * (1.0 - fabs(fmod(h, 2.0) - 1.0));
        if (h < 1) {*r=(int)c;*g=(int)q;*b=0;}
        else if (h < 2) {*r=(int)q;*g=(int)c;*b=0;}
        else if (h < 3) {*r=0;*g=(int)c;*b=(int)q;}
        else if (h < 4) {*r=0;*g=(int)q;*b=(int)c;}
        else if (h < 5) {*r=(int)q;*g=0;*b=(int)c;}
        else {*r=(int)c;*g=0;*b=(int)q;}
        return;
    }
    if (!strcmp(cfg.theme, "cyan")) {*r=30;*g=240;*b=220;return;}
    if (!strcmp(cfg.theme, "green")) {*r=50;*g=230;*b=100;return;}
    if (!strcmp(cfg.theme, "amber")) {*r=255;*g=190;*b=20;return;}
    if (!strcmp(cfg.theme, "purple")) {*r=220;*g=100;*b=255;return;}
    if (!strcmp(cfg.theme, "monochrome")) {*r=230;*g=230;*b=230;return;}
    (void)song;
    *r = cover_r; *g = cover_g; *b = cover_b;
}

static void print_fit(const char *text, int limit) {
    mbstate_t state = {0};
    int width = 0;
    const char *p = text;
    size_t left = strlen(text);
    while (left && width < limit) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, left, &state);
        if (n == (size_t)-1 || n == (size_t)-2) { putchar((unsigned char)*p); p++; left--; memset(&state, 0, sizeof(state)); width++; continue; }
        if (!n) break;
        int cells = wcwidth(wc);
        if (cells > 0 && width + cells > limit) break;
        fwrite(p, 1, n, stdout);
        if (cells > 0) width += cells;
        p += n;
        left -= n;
    }
}

static void clean_text(char *text) {
    if (!cfg.clean) return;
    static const char *builtins[] = {"fuck", "fucking", "shit", "bitch", "bastard", "damn", "asshole", "motherfucker", "nigger", "nigga", "faggot", "retard", "cunt", "whore"};
    const char *words[48];
    size_t word_count = sizeof(builtins) / sizeof(builtins[0]);
    for (size_t i = 0; i < word_count; i++) words[i] = builtins[i];
    for (int i = 0; i < cfg.extra_word_count && word_count < 47; i++) words[word_count++] = cfg.extra_words[i];
    words[word_count] = NULL;
    for (size_t i = 0; words[i]; i++) {
        size_t n = strlen(words[i]);
        for (char *p = text; *p; p++) {
            size_t j = 0;
            while (j < n && p[j] && tolower((unsigned char)p[j]) == words[i][j]) j++;
            bool left = p == text || !isalnum((unsigned char)p[-1]);
            bool right = !p[j] || !isalnum((unsigned char)p[j]);
            if (j == n && left && right) memset(p, '*', n);
        }
    }
}

void lowercase(char *text) {
    for (unsigned char *p = (unsigned char *)text; *p; p++) *p = (unsigned char)tolower(*p);
}

static const char *lossless_label(const Song *song) {
    static const char *codecs[][2] = {
        {"flac", "FLAC"}, {"alac", "ALAC"}, {"wavpack", "WavPack"},
        {"ape", "APE"}, {"pcm", "PCM"}, {"dsf", "DSD"}, {"dff", "DSD"},
        {"tta", "TTA"}, {"wave", "WAV"}, {"wav", "WAV"}
    };
    char codec[256];
    set_string(codec, sizeof(codec), song->codec);
    lowercase(codec);
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        if (strstr(codec, codecs[i][0])) {
            static char result[64];
            snprintf(result, sizeof(result), "lossless (%s)", codecs[i][1]);
            return result;
        }
    }
    const char *path = strrchr(song->media_url, '/');
    if (!path) path = song->media_url;
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    char extension[16];
    set_string(extension, sizeof(extension), dot + 1);
    lowercase(extension);
    const char *label = NULL;
    if (!strcmp(extension, "flac")) label = "FLAC";
    else if (!strcmp(extension, "alac")) label = "ALAC";
    else if (!strcmp(extension, "wav") || !strcmp(extension, "wave")) label = "WAV";
    else if (!strcmp(extension, "aif") || !strcmp(extension, "aiff")) label = "AIFF";
    else if (!strcmp(extension, "ape")) label = "APE";
    else if (!strcmp(extension, "wv")) label = "WavPack";
    else if (!strcmp(extension, "tta")) label = "TTA";
    else if (!strcmp(extension, "dsf") || !strcmp(extension, "dff")) label = "DSD";
    if (!label) return NULL;
    static char result[64];
    snprintf(result, sizeof(result), "lossless (%s)", label);
    return result;
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static void move_to(int x, int y) {
    printf("\033[%d;%dH", y + 1, x + 1);
}

static void rgb(int r, int g, int b) {
    printf("\033[38;2;%d;%d;%dm", r, g, b);
}

static void reset_color(void) {
    fputs("\033[0m", stdout);
}

static size_t next_glyph(const char *text, size_t remaining, mbstate_t *state, int *cells) {
    wchar_t wc;
    size_t bytes = mbrtowc(&wc, text, remaining, state);
    if (bytes == (size_t)-1 || bytes == (size_t)-2) {
        bytes = 1;
        wc = (unsigned char)*text;
        memset(state, 0, sizeof(*state));
    }
    if (!bytes) return 0;
    *cells = wcwidth(wc);
    if (*cells < 0) *cells = 1;
    return bytes;
}

static int text_cell_width(const char *text) {
    mbstate_t state = {0};
    size_t remaining = strlen(text);
    int total = 0;
    while (remaining) {
        int cells;
        size_t bytes = next_glyph(text, remaining, &state, &cells);
        if (!bytes) break;
        total += cells;
        text += bytes;
        remaining -= bytes;
    }
    return total;
}

static size_t lyric_wrap_segment(const char *text, int limit, const char **next) {
    size_t used = 0, break_at = 0, after_break = 0;
    size_t length = strlen(text);
    int cells = 0;
    mbstate_t state = {0};
    while (text[used]) {
        if (text[used] == '\n') {
            *next = text + used + 1;
            return used;
        }
        int glyph_cells;
        size_t bytes = next_glyph(text + used, length - used, &state, &glyph_cells);
        if (!bytes) break;
        if (glyph_cells > 0 && cells + glyph_cells > limit) break;
        if (text[used] == ' ' || text[used] == '\t') {
            break_at = used;
            after_break = used + bytes;
        }
        cells += glyph_cells;
        used += bytes;
    }
    if (!text[used]) {
        *next = text + used;
        return used;
    }
    if (break_at) {
        while (text[after_break] == ' ' || text[after_break] == '\t') after_break++;
        *next = text + after_break;
        return break_at;
    }
    if (!used) {
        int glyph_cells;
        used = next_glyph(text, length, &state, &glyph_cells);
    }
    *next = text + used;
    return used;
}

static int lyric_wrap_rows(const char *text, int limit) {
    int rows = 0;
    const char *cursor = text;
    do {
        const char *next;
        lyric_wrap_segment(cursor, limit, &next);
        rows++;
        if (next <= cursor) break;
        cursor = next;
    } while (*cursor);
    return rows;
}

static bool prepare_wrapped_lyric(int index, int width) {
    if (!wrapped_lyrics[index].text) {
        wrapped_lyrics[index].text = strdup(lyrics[index].text ? lyrics[index].text : "");
        if (!wrapped_lyrics[index].text) return false;
        clean_text(wrapped_lyrics[index].text);
        wrapped_lyrics[index].height = lyric_wrap_rows(wrapped_lyrics[index].text, width);
    }
    return true;
}

static DetailRow *touch_detail_row(int x, int y, const MarqueeState *marquee, int r, int g, int b, bool bold) {
    if (!detail_rows || y < 0 || y >= detail_row_count) return NULL;
    DetailRow *row = &detail_rows[y];
    bool changed = row->x != x || row->marquee != marquee || row->r != r || row->g != g || row->b != b || row->bold != bold;
    if (changed && (row->text || row->marquee)) {
        move_to(row->x < x ? row->x : x, y);
        fputs("\033[0K", stdout);
        frame_changed = true;
    }
    if (changed) {
        free(row->text);
        row->text = NULL;
    }
    row->x = x;
    row->marquee = marquee;
    row->r = r;
    row->g = g;
    row->b = b;
    row->bold = bold;
    row->touched = true;
    return row;
}

static void draw_detail_row(const char *text, int x, int y, int r, int g, int b, bool bold) {
    if (!text) return;
    DetailRow *row = touch_detail_row(x, y, NULL, r, g, b, bold);
    if (!row || (row->text && !strcmp(row->text, text))) return;
    if (row->text) {
        move_to(x, y);
        fputs("\033[0K", stdout);
    }
    char *copy = strdup(text);
    if (!copy) return;
    free(row->text);
    row->text = copy;
    move_to(x, y);
    rgb(r, g, b);
    if (bold) fputs("\033[1m", stdout);
    fputs(text, stdout);
    reset_color();
    frame_changed = true;
}

static bool prepare_marquee_row(MarqueeState *state, int x, int y, int r, int g, int b, bool bold) {
    bool changed = true;
    if (detail_rows && y >= 0 && y < detail_row_count) {
        DetailRow *old = &detail_rows[y];
        changed = old->marquee != state || old->x != x || old->r != r || old->g != g || old->b != b || old->bold != bold;
    }
    DetailRow *row = touch_detail_row(x, y, state, r, g, b, bold);
    if (!row) return false;
    if (changed) state->placed = false;
    return changed;
}

static void finish_detail_rows(void) {
    for (int y = 0; y < detail_row_count; y++) {
        DetailRow *row = &detail_rows[y];
        if (!row->touched && (row->text || row->marquee)) {
            move_to(row->x, y);
            fputs("\033[0K", stdout);
            frame_changed = true;
            free(row->text);
            memset(row, 0, sizeof(*row));
        }
    }
}

static void draw_static_field(const char *text, int width, int x, int y, int r, int g, int b) {
    if (width < 1) return;
    char output[MAX_FIELD + 8];
    int limit = text_cell_width(text) > width ? width - 1 : width;
    int cells = 0;
    size_t used = 0, remaining = strlen(text);
    mbstate_t state = {0};
    while (remaining && used < MAX_FIELD) {
        int glyph_cells;
        size_t bytes = next_glyph(text, remaining, &state, &glyph_cells);
        if (!bytes || cells + glyph_cells > limit || used + bytes >= sizeof(output) - 4) break;
        memcpy(output + used, text, bytes);
        used += bytes;
        text += bytes;
        remaining -= bytes;
        cells += glyph_cells;
    }
    if (remaining && width > 0) { memcpy(output + used, "…", 3); used += 3; }
    output[used] = 0;
    draw_detail_row(output, x, y, r, g, b, false);
}

static int draw_lyric_segments(const char *text, int width, int skip, int limit, int x, int y, bool active, int r, int g, int b, int arrow_r, int arrow_g, int arrow_b) {
    const char *cursor = text;
    int segment = 0, drawn = 0;
    do {
        const char *next;
        size_t bytes = lyric_wrap_segment(cursor, width, &next);
        if (segment >= skip && drawn < limit) {
            char lyric_row[8192];
            const char *prefix = active && segment == 0 ? "› " : "  ";
            size_t prefix_size = strlen(prefix);
            memcpy(lyric_row, prefix, prefix_size);
            size_t used = prefix_size;
            if (active && segment == 0 && (r != arrow_r || g != arrow_g || b != arrow_b)) {
                int color_size = snprintf(lyric_row + used, sizeof(lyric_row) - used, "\033[38;2;%d;%d;%dm", r, g, b);
                if (color_size > 0 && (size_t)color_size < sizeof(lyric_row) - used) used += (size_t)color_size;
            }
            if (bytes > sizeof(lyric_row) - used - 1) bytes = sizeof(lyric_row) - used - 1;
            memcpy(lyric_row + used, cursor, bytes);
            lyric_row[used + bytes] = 0;
            draw_detail_row(lyric_row, x, y + drawn, active && segment == 0 ? arrow_r : r, active && segment == 0 ? arrow_g : g, active && segment == 0 ? arrow_b : b, false);
            drawn++;
        }
        segment++;
        if (next <= cursor) break;
        cursor = next;
    } while (*cursor && drawn < limit);
    return drawn;
}

static bool resolve_font_family(const char *postscript_name, char *family, size_t family_size) {
    if (!postscript_name[0]) return false;
    FcPattern *pattern = FcPatternCreate();
    if (!pattern) return false;
    FcPatternAddString(pattern, FC_POSTSCRIPT_NAME, (const FcChar8 *)postscript_name);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return false;
    FcChar8 *matched_name = NULL, *matched_family = NULL;
    bool found = FcPatternGetString(match, FC_POSTSCRIPT_NAME, 0, &matched_name) == FcResultMatch &&
                 FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
                 !strcmp((const char *)matched_name, postscript_name);
    if (found) set_string(family, family_size, (const char *)matched_family);
    FcPatternDestroy(match);
    return found;
}

static bool resolve_named_font(const char *name, char *family, size_t family_size) {
    if (!name[0]) return false;
    FcPattern *pattern = FcPatternCreate();
    if (!pattern) return false;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)name);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return false;
    FcChar8 *matched_family = NULL;
    bool found = FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
                 !strcasecmp((const char *)matched_family, name);
    if (found) set_string(family, family_size, (const char *)matched_family);
    FcPatternDestroy(match);
    return found;
}

static void detect_kitty_font(void) {
    const char *term = getenv("TERM");
    if (!term || !strstr(term, "kitty")) return;
    FILE *query = popen("kitty +kitten query_terminal --wait-for=0.7 font_family bold_font font_size dpi_y 2>/dev/null", "r");
    if (!query) return;
    char regular_name[256] = "", bold_name[256] = "", line[512];
    double points = 0.0, dpi = 96.0;
    while (fgets(line, sizeof(line), query)) {
        trim(line);
        char *value = strchr(line, ':');
        if (!value) continue;
        *value++ = 0;
        trim(value);
        if (!strcmp(line, "font_family")) set_string(regular_name, sizeof(regular_name), value);
        else if (!strcmp(line, "bold_font")) set_string(bold_name, sizeof(bold_name), value);
        else if (!strcmp(line, "font_size")) points = atof(value);
        else if (!strcmp(line, "dpi_y")) dpi = atof(value);
    }
    pclose(query);
    if (points <= 0.0 || dpi <= 0.0 || !resolve_font_family(regular_name, kitty_font.regular, sizeof(kitty_font.regular))) return;
    if (!resolve_font_family(bold_name, kitty_font.bold, sizeof(kitty_font.bold))) set_string(kitty_font.bold, sizeof(kitty_font.bold), kitty_font.regular);
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_row || !ws.ws_ypixel) return;
    kitty_font.initial_cell_height = ws.ws_ypixel / ws.ws_row;
    if (kitty_font.initial_cell_height < 1) return;
    kitty_font.pixel_size = points * dpi / 72.0;
    kitty_font.ready = true;
}

void detect_terminal_font(void) {
    if (!cfg.terminal_font[0]) {
        detect_kitty_font();
        if (kitty_font.ready) return;
    }
    char family[256] = "";
    double points = cfg.terminal_font_size;
    if (cfg.terminal_font[0]) set_string(family, sizeof(family), cfg.terminal_font);
    else if (getenv("GHOSTTY_RESOURCES_DIR")) {
        FILE *query = popen("ghostty +show-config --default 2>/dev/null", "r");
        if (query) {
            char line[512];
            while (fgets(line, sizeof(line), query)) {
                char *equals = strchr(line, '=');
                if (!equals) continue;
                *equals++ = 0;
                trim(line);
                trim(equals);
                size_t length = strlen(equals);
                if (length >= 2 && equals[0] == '"' && equals[length - 1] == '"') {
                    equals[length - 1] = 0;
                    equals++;
                }
                if (!strcmp(line, "font-family") && equals[0] && !family[0]) set_string(family, sizeof(family), equals);
                else if (!strcmp(line, "font-size") && points <= 0.0) points = atof(equals);
            }
            pclose(query);
        }
        if (!family[0]) set_string(family, sizeof(family), "JetBrains Mono");
    } else if (getenv("WEZTERM_PANE")) {
        FILE *query = popen("wezterm ls-fonts 2>/dev/null", "r");
        if (query) {
            char line[512];
            while (fgets(line, sizeof(line), query)) {
                if (family[0]) continue;
                char *start = strstr(line, "{family=\"");
                if (!start) continue;
                start += 9;
                char *end = strchr(start, '"');
                if (!end) continue;
                *end = 0;
                set_string(family, sizeof(family), start);
            }
            pclose(query);
        }
        if (!family[0]) set_string(family, sizeof(family), "JetBrains Mono");
    }
    if (!resolve_named_font(family, kitty_font.regular, sizeof(kitty_font.regular))) return;
    set_string(kitty_font.bold, sizeof(kitty_font.bold), kitty_font.regular);
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_row) return;
    kitty_font.initial_cell_height = ws.ws_ypixel && ws.ws_ypixel >= ws.ws_row ? ws.ws_ypixel / ws.ws_row : 0;
    kitty_font.pixel_size = points > 0.0 ? points * 96.0 / 72.0 : 0.0;
    kitty_font.ready = true;
}

void probe_terminal_features(void) {
    fputs("\033_Gi=190741,s=1,v=1,a=q,t=d,f=24;AAAA\033\\\033[?2026$p\033[14t", stdout);
    fflush(stdout);
    char response[2048] = "";
    size_t used = 0;
    double deadline = monotonic_seconds() + 0.45;
    while (monotonic_seconds() < deadline && used + 1 < sizeof(response)) {
        struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
        int remaining = (int)((deadline - monotonic_seconds()) * 1000.0);
        if (remaining < 1) remaining = 1;
        if (poll(&fd, 1, remaining) <= 0) break;
        ssize_t count = read(STDIN_FILENO, response + used, sizeof(response) - used - 1);
        if (count <= 0) break;
        used += (size_t)count;
        response[used] = 0;
        if (strstr(response, "\033_Gi=190741;OK")) graphics_supported = true;
        if (strstr(response, "\033[?2026;1$y") || strstr(response, "\033[?2026;2$y")) synchronized_updates_supported = true;
        for (char *at = response; (at = strstr(at, "\033[4;")); at++) {
            int height = 0, width = 0;
            if (strchr(at, 't') && sscanf(at, "\033[4;%d;%dt", &height, &width) == 2 && height > 0 && width > 0) {
                struct winsize ws = {0};
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
                    probed_cell_width = width / ws.ws_col;
                    probed_cell_height = height / ws.ws_row;
                }
                break;
            }
        }
        if (graphics_supported && synchronized_updates_supported && probed_cell_width && probed_cell_height) break;
    }
    const char *term = getenv("TERM");
    if (term && strstr(term, "kitty")) {
        graphics_supported = true;
        synchronized_updates_supported = true;
    }
    if (kitty_font.ready && !kitty_font.initial_cell_height) kitty_font.initial_cell_height = probed_cell_height;
    if (kitty_font.ready && kitty_font.pixel_size <= 0.0 && kitty_font.initial_cell_height > 0) kitty_font.pixel_size = kitty_font.initial_cell_height * 0.8;
    if (kitty_font.ready && (!kitty_font.initial_cell_height || kitty_font.pixel_size <= 0.0)) kitty_font.ready = false;
}

static bool kitty_graphics_ready(int *cell_width, int *cell_height) {
    struct winsize ws = {0};
    if (!graphics_supported || !kitty_font.ready || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_col || !ws.ws_row) return false;
    *cell_width = ws.ws_xpixel ? ws.ws_xpixel / ws.ws_col : probed_cell_width;
    *cell_height = ws.ws_ypixel ? ws.ws_ypixel / ws.ws_row : probed_cell_height;
    return *cell_width > 0 && *cell_height > 0;
}

static cairo_status_t marquee_png_write(void *closure, const unsigned char *data, unsigned int length) {
    Buffer *output = closure;
    if (output->length + length > output->capacity) {
        size_t capacity = output->capacity ? output->capacity * 2 : 8192;
        while (capacity < output->length + length) capacity *= 2;
        unsigned char *expanded = realloc(output->data, capacity);
        if (!expanded) return CAIRO_STATUS_NO_MEMORY;
        output->data = expanded;
        output->capacity = capacity;
    }
    memcpy(output->data + output->length, data, length);
    output->length += length;
    return CAIRO_STATUS_SUCCESS;
}

static void kitty_delete_marquee(MarqueeState *state) {
    state->placed = false;
    if (!state->uploaded) return;
    printf("\033_Ga=d,d=I,i=%u,q=1;\033\\", state->image_id);
    for (size_t i = 0; i < 3; i++) if (state->fade_ids[i]) printf("\033_Ga=d,d=I,i=%u,q=1;\033\\", state->fade_ids[i]);
    state->uploaded = false;
    state->fades_uploaded = false;
    frame_changed = true;
}

static bool kitty_upload_marquee(MarqueeState *state, const char *text, int cells, int r, int g, int b, bool bold, double opacity, uint32_t *image_id) {
    int pixel_width = cells * state->cell_width;
    if (pixel_width < 1 || pixel_width > 32768 || state->cell_height > 256) return false;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_width, state->cell_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(surface); return false; }
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, bold ? kitty_font.bold : kitty_font.regular);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font, (int)(kitty_font.pixel_size * state->cell_height / kitty_font.initial_cell_height * PANGO_SCALE));
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    int natural_width, natural_height;
    pango_layout_get_pixel_size(layout, &natural_width, &natural_height);
    if (natural_width > 0) {
        cairo_scale(cr, (double)pixel_width / natural_width, 1.0);
        cairo_move_to(cr, 0, (state->cell_height - natural_height) / 2.0);
        cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, opacity);
        pango_cairo_show_layout(cr, layout);
    }
    Buffer png = {0};
    cairo_status_t status = cairo_surface_write_to_png_stream(surface, marquee_png_write, &png);
    pango_font_description_free(font);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS || !png.length) { free(png.data); return false; }
    if (!*image_id) {
        if (RAND_bytes((unsigned char *)image_id, sizeof(*image_id)) != 1 || !*image_id) *image_id = (uint32_t)getpid() ^ (uint32_t)time(NULL);
        if (!*image_id) *image_id = 1;
    }
    for (size_t at = 0; at < png.length; at += 3072) {
        size_t count = png.length - at;
        if (count > 3072) count = 3072;
        char encoded[4097];
        EVP_EncodeBlock((unsigned char *)encoded, png.data + at, (int)count);
        if (at == 0) printf("\033_Ga=t,f=100,i=%u,q=1,m=%d;%s\033\\", *image_id, at + count < png.length, encoded);
        else printf("\033_Gm=%d;%s\033\\", at + count < png.length, encoded);
    }
    free(png.data);
    return true;
}

static void draw_marquee(MarqueeState *state, const char *text, int width, int terminal_cols, int x, int y, int r, int g, int b, bool bold, double now) {
    if (width < 1) return;
    int cell_width = 0, cell_height = 0;
    bool kitty = kitty_graphics_ready(&cell_width, &cell_height);
    bool content_changed = strcmp(state->text, text) || state->width != width || state->terminal_cols != terminal_cols || state->cell_width != cell_width || state->cell_height != cell_height || state->bold != bold;
    bool color_changed = state->color_r != r || state->color_g != g || state->color_b != b;
    if (content_changed || (kitty && color_changed && now - state->recolored_at >= 0.2)) {
        kitty_delete_marquee(state);
        set_string(state->text, sizeof(state->text), text);
        state->width = width;
        state->terminal_cols = terminal_cols;
        state->cell_width = cell_width;
        state->cell_height = cell_height;
        state->color_r = r;
        state->color_g = g;
        state->color_b = b;
        state->bold = bold;
        if (content_changed) state->started_at = now;
        state->recolored_at = now;
    }
    if (!kitty && color_changed) {
        state->color_r = r;
        state->color_g = g;
        state->color_b = b;
        state->placed = false;
    }
    int total = text_cell_width(text);
    state->active = total > width;
    int offset = 0;
    double fractional = 0.0;
    double pixel_position = 0.0;
    if (total > width) {
        int overflow = total - width;
        double travel = overflow / cfg.marquee_speed;
        double frame_time = floor(fmax(0.0, now - state->started_at) * cfg.fps) / cfg.fps;
        double phase = fmod(frame_time, 1.2 + travel + 1.0 + travel);
        double position = 0.0;
        if (phase < 1.2) position = 0.0;
        else if (phase < 1.2 + travel) {
            double progress = (phase - 1.2) / travel;
            position = overflow * progress * progress * (3.0 - 2.0 * progress);
        } else if (phase < 2.2 + travel) position = overflow;
        else {
            double progress = (phase - 2.2 - travel) / travel;
            position = overflow * (1.0 - progress * progress * (3.0 - 2.0 * progress));
        }
        if (position < 0.0) position = 0.0;
        if (position > overflow) position = overflow;
        offset = (int)position;
        fractional = position - offset;
        pixel_position = position;
    }
    if (kitty && total > width && (state->uploaded || (state->uploaded = kitty_upload_marquee(state, text, total, r, g, b, bold, 1.0, &state->image_id)))) {
        int pixel_offset = (int)lround(pixel_position * cell_width);
        int max_offset = (total - width) * cell_width;
        if (pixel_offset > max_offset) pixel_offset = max_offset;
        if (width >= 8 && !state->fades_uploaded) {
            const double opacity[] = {0.74, 0.45, 0.18};
            state->fades_uploaded = true;
            for (size_t i = 0; i < 3; i++) if (!kitty_upload_marquee(state, text, total, r, g, b, bold, opacity[i], &state->fade_ids[i])) state->fades_uploaded = false;
        }
        int fade_width = state->fades_uploaded && width >= 8 ? 3 : 0;
        if (state->placed && state->placed_offset == pixel_offset && state->placed_x == x && state->placed_y == y) return;
        move_to(x, y);
        if (!state->placed) fputs("\033[0K", stdout);
        frame_changed = true;
        printf("\033_Ga=p,i=%u,p=1,x=%d,y=0,w=%d,h=%d,c=%d,r=1,C=1,q=1;\033\\", state->image_id, pixel_offset, (width - fade_width) * cell_width, cell_height, width - fade_width);
        for (int i = 0; i < fade_width; i++) {
            move_to(x + width - fade_width + i, y);
            printf("\033_Ga=p,i=%u,p=1,x=%d,y=0,w=%d,h=%d,c=1,r=1,C=1,q=1;\033\\", state->fade_ids[i], pixel_offset + (width - fade_width + i) * cell_width, cell_width, cell_height);
        }
        state->placed = true;
        state->placed_offset = pixel_offset;
        state->placed_x = x;
        state->placed_y = y;
        return;
    }
    if (state->placed && state->placed_offset == offset && state->placed_x == x && state->placed_y == y) return;
    move_to(x, y);
    fputs("\033[0K", stdout);
    frame_changed = true;
    rgb(r, g, b);
    if (bold) fputs("\033[1m", stdout);
    if (total <= width) {
        print_fit(text, width);
        reset_color();
        state->placed = true;
        state->placed_offset = offset;
        state->placed_x = x;
        state->placed_y = y;
        return;
    }
    mbstate_t glyph_state = {0};
    size_t remaining = strlen(text);
    int position = 0;
    int visible = 0;
    int fade_length = width >= 16 ? 4 : 2;
    int last_fade = 0;
    while (remaining && visible < width) {
        int cells;
        size_t bytes = next_glyph(text, remaining, &glyph_state, &cells);
        if (!bytes) break;
        int next_position = position + cells;
        if (next_position > offset && cells > 0) {
            if (position < offset) {
                putchar(' ');
                visible++;
            } else if (visible + cells <= width) {
                int fade = visible >= width - fade_length ? visible - (width - fade_length) + 1 : 0;
                if (fade != last_fade) {
                    double opacity = 1.0 - 0.15 * fade;
                    rgb((int)(r * opacity + 25 * (1.0 - opacity)), (int)(g * opacity + 25 * (1.0 - opacity)), (int)(b * opacity + 25 * (1.0 - opacity)));
                    last_fade = fade;
                }
                if (visible == 0 && fractional > 0.0) {
                    double opacity = 1.0 - 0.6 * fractional;
                    rgb((int)(r * opacity + 25 * (1.0 - opacity)), (int)(g * opacity + 25 * (1.0 - opacity)), (int)(b * opacity + 25 * (1.0 - opacity)));
                }
                fwrite(text, 1, bytes, stdout);
                if (visible == 0 && fractional > 0.0) rgb(r, g, b);
                visible += cells;
            } else break;
        }
        position = next_position;
        text += bytes;
        remaining -= bytes;
    }
    reset_color();
    state->placed = true;
    state->placed_offset = offset;
    state->placed_x = x;
    state->placed_y = y;
}

static void format_time(double value, char *result, size_t result_size) {
    if (value < 0) value = 0;
    snprintf(result, result_size, "%d:%02d", (int)value / 60, (int)value % 60);
}

typedef struct {
    char glyph;
    unsigned char red, green, blue;
} VideoCell;

static void draw_art(const Song *song, const Image *image, int x, int y, int width, int height, int base_r, int base_g, int base_b, bool force) {
    static VideoCell *previous;
    static int previous_x, previous_y, previous_width, previous_height;
    static bool previous_valid;
    (void)song;
    if (!image->pixels || !strcmp(cfg.art_mode, "none")) {
        move_to(x, y);
        rgb(base_r, base_g, base_b);
        fputs("album art unavailable", stdout);
        reset_color();
        return;
    }
    const char *chars = !strcmp(cfg.art_mode, "blocks") ? " .oO#@" : cfg.characters[0] ? cfg.characters : " .,:;irsXA253hMHGS#9B&@";
    size_t chars_len = strlen(chars);
    int draw_width = width, draw_height = height;
    int x_offset = 0, y_offset = 0;
    if (local_has_video()) {
        double aspect = (double)image->width / image->height;
        draw_height = (int)round(width / (2.0 * aspect));
        if (draw_height > height) {
            draw_height = height;
            draw_width = (int)round(height * 2.0 * aspect);
        }
        if (draw_width > width) draw_width = width;
        if (draw_height < 1) draw_height = 1;
        if (draw_width < 1) draw_width = 1;
        x_offset = (width - draw_width) / 2;
        y_offset = (height - draw_height) / 2;
    }
    bool incremental = local_has_video();
    bool compare = incremental && previous_valid && !force && x == previous_x && y == previous_y && width == previous_width && height == previous_height;
    if (incremental && (!previous || width != previous_width || height != previous_height)) {
        VideoCell *resized = realloc(previous, (size_t)width * (size_t)height * sizeof(*previous));
        if (resized) previous = resized;
        else { free(previous); previous = NULL; compare = false; }
    }
    for (int row = 0; row < height; row++) {
        bool active = false;
        bool printed = false;
        int last_r = -1, last_g = -1, last_b = -1;
        for (int col = 0; col < width; col++) {
            VideoCell cell = {0};
            if (col < x_offset || col >= x_offset + draw_width || row < y_offset || row >= y_offset + draw_height) {
                cell.glyph = ' ';
            } else {
                int sx = (int)((double)(col - x_offset) / draw_width * image->width);
                int sy = (int)((double)(row - y_offset) / draw_height * image->height);
                if (sx >= image->width) sx = image->width - 1;
                if (sy >= image->height) sy = image->height - 1;
                size_t at = ((size_t)sy * (size_t)image->width + (size_t)sx) * 3;
                int red = image->pixels[at], green = image->pixels[at + 1], blue = image->pixels[at + 2];
                int bright = (int)(0.2126 * red + 0.7152 * green + 0.0722 * blue);
                cell.glyph = chars[(size_t)bright * (chars_len - 1) / 255];
                if (!strcmp(cfg.art_mode, "monochrome")) red = base_r, green = base_g, blue = base_b;
                cell.red = (unsigned char)red;
                cell.green = (unsigned char)green;
                cell.blue = (unsigned char)blue;
            }
            size_t index = (size_t)row * (size_t)width + (size_t)col;
            if (compare && !memcmp(&previous[index], &cell, sizeof(cell))) { active = false; continue; }
            if (!active) { move_to(x + col, y + row); active = true; last_r = last_g = last_b = -1; }
            if (cell.glyph != ' ' && (last_r != cell.red || last_g != cell.green || last_b != cell.blue)) {
                rgb(cell.red, cell.green, cell.blue);
                last_r = cell.red; last_g = cell.green; last_b = cell.blue;
            }
            putchar(cell.glyph);
            printed = true;
            if (previous && incremental) previous[index] = cell;
            frame_changed = true;
        }
        if (printed) reset_color();
    }
    if (incremental) {
        previous_x = x; previous_y = y; previous_width = width; previous_height = height;
        previous_valid = previous != NULL;
    } else previous_valid = false;
}

static bool draw_notice(int rows, int cols, double now, bool force) {
    static char previous[MAX_FIELD];
    static int previous_row = -1;
    const char *message = now < ui_notice_until ? ui_notice : "";
    int row = rows - 1;
    if (!force && row == previous_row && !strcmp(previous, message)) return false;
    if (previous_row >= 0 && previous_row < rows && previous_row != row) {
        move_to(0, previous_row);
        fputs("\033[0K", stdout);
    }
    move_to(0, row);
    fputs("\033[0K", stdout);
    if (message[0] && cols > 2) {
        move_to(1, row);
        rgb(255, 145, 115);
        print_fit(message, cols - 2);
        reset_color();
    }
    set_string(previous, sizeof(previous), message);
    previous_row = row;
    frame_changed = true;
    return true;
}

void render(const Song *song, double now) {
    frame_changed = false;
    int rows, cols;
    get_terminal_size(&rows, &cols);
    static int placeholder_screen = 0;
    static bool first_media_ready = false;
    static double first_media_seen_at = 0.0;
    bool local_loading = local_is_loading();
    bool no_media = (!song->player[0] || !song->title[0] || !strcasecmp(song->status, "stopped")) && !local_loading;
    bool loading = local_loading;
    if (!no_media) {
        if (!first_media_ready) {
            if (first_media_seen_at == 0.0) first_media_seen_at = now;
            if (now - first_media_seen_at < 0.5) loading = true;
            else first_media_ready = true;
        }
        if (rows >= 18 && cols >= 66 && !cfg.minimal && strcmp(cfg.art_mode, "none") && song->art_url[0]) {
            pthread_mutex_lock(&artwork_mutex);
            if (strcmp(current_artwork_url, song->art_url)) loading = true;
            pthread_mutex_unlock(&artwork_mutex);
        }
    }
    int placeholder = loading ? 2 : no_media ? 1 : 0;
    if (placeholder) {
        bool redraw_placeholder = placeholder != placeholder_screen || rows != previous_rows || cols != previous_cols;
        atomic_store(&force_redraw, false);
        if (redraw_placeholder) {
            if (synchronized_updates_supported) fputs("\033[?2026h", stdout);
            kitty_delete_marquee(&title_marquee);
            kitty_delete_marquee(&album_marquee);
            kitty_delete_marquee(&playlist_marquee);
            kitty_delete_marquee(&next_marquee);
            title_marquee.active = false;
            album_marquee.active = false;
            playlist_marquee.active = false;
            next_marquee.active = false;
            fputs("\033[2J", stdout);
            reset_detail_rows();
            previous_rows = rows;
            previous_cols = cols;
            previous_art_url[0] = 0;
            if (no_media) {
                const char *message = cols >= 16 ? "no media playing" : cols >= 8 ? "no media" : "idle";
                int width = (int)strlen(message);
                if (cols >= width) {
                    move_to((cols - width) / 2, (rows - 1) / 2);
                    fputs(message, stdout);
                }
            }
            if (synchronized_updates_supported) fputs("\033[?2026l", stdout);
            fflush(stdout);
        }
        if (draw_notice(rows, cols, now, redraw_placeholder)) fflush(stdout);
        placeholder_screen = placeholder;
        return;
    }
    if (placeholder_screen) {
        placeholder_screen = 0;
        atomic_store(&force_redraw, true);
    }
    bool synchronized = synchronized_updates_supported;
    if (synchronized) fputs("\033[?2026h", stdout);
    title_marquee.active = false;
    album_marquee.active = false;
    playlist_marquee.active = false;
    next_marquee.active = false;
    if (rows != previous_rows || cols != previous_cols || strcmp(previous_art_url, song->art_url)) force_redraw = true;
    bool redraw_art = atomic_exchange(&force_redraw, false);
    if (redraw_art) {
        frame_changed = true;
        kitty_delete_marquee(&title_marquee);
        kitty_delete_marquee(&album_marquee);
        kitty_delete_marquee(&playlist_marquee);
        kitty_delete_marquee(&next_marquee);
        fputs("\033[2J", stdout);
        reset_detail_rows();
        previous_rows = rows;
        previous_cols = cols;
        set_string(previous_art_url, sizeof(previous_art_url), song->art_url);
    }
    char queued_track[MAX_FIELD];
    char playlist_name[MAX_FIELD];
    pthread_mutex_lock(&queue_mutex);
    set_string(queued_track, sizeof(queued_track), next_track);
    set_string(playlist_name, sizeof(playlist_name), current_playlist);
    pthread_mutex_unlock(&queue_mutex);
    const char *playlist_display = song->playlist[0] ? song->playlist : playlist_name;
    bool tiny = rows < 18 || cols < 66;
    int art_h = tiny ? 0 : rows - 4;
    if (art_h < 1) art_h = 1;
    int art_w = tiny || cfg.minimal || !strcmp(cfg.art_mode, "none") ? 0 : (int)(art_h * 2.0);
    int panel_w = cfg.show_time ? 13 : 1;
    if (cfg.visualizer && cava_enabled && cfg.bars > panel_w) panel_w = cfg.bars;
    const char *fields[] = {song->title, song->artist, song->album, playlist_display, queued_track};
    bool visible[] = {true, true, !tiny && !cfg.minimal && cfg.show_album,
                      !tiny && !cfg.minimal && cfg.show_playlist,
                      !tiny && !cfg.minimal && cfg.show_next};
    int prefixes[] = {0, 0, 0, 10, 6};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        if (!visible[i] || !fields[i][0]) continue;
        char field[MAX_FIELD];
        set_string(field, sizeof(field), fields[i]);
        clean_text(field);
        int width = text_cell_width(field) + prefixes[i];
        if (width > panel_w) panel_w = width;
    }
    if (!tiny && !cfg.minimal) {
        int width = 0;
        if (cfg.show_source) width = text_cell_width(song->player) + 8 + (lossless_label(song) ? 11 : 0);
        if (cfg.show_track && text_cell_width(song->track) + 6 > width) width = text_cell_width(song->track) + 6;
        if (cfg.show_status && text_cell_width(song->status) + 8 > width) width = text_cell_width(song->status) + 8;
        if (cfg.show_volume && width < 12) width = 12;
        if (width > panel_w) panel_w = width;
    }
    if (panel_w > 48) panel_w = 48;
    if (panel_w < 12) panel_w = 12;
    if (panel_w > cols - 2) panel_w = cols > 2 ? cols - 2 : 1;
    int total = art_w + 3 + panel_w;
    if (!tiny && total > cols - 2) {
        art_w = (cols - panel_w - 5) / 2 * 2;
        if (art_w < 8) art_w = 8;
        art_h = art_w / 2;
    }
    if (!tiny && local_has_video() && !cfg.minimal && strcmp(cfg.art_mode, "none")) {
        int available_width = cols - panel_w - 5;
        int ideal_width = (int)round((rows - 4) * 3.5);
        art_w = available_width < ideal_width ? available_width : ideal_width;
        if (art_w < 8) art_w = 8;
        art_h = (int)round(art_w / 3.5);
        if (art_h < 1) art_h = 1;
    }
    if (art_h > rows - 4) art_h = rows - 4;
    int group_w = art_w ? art_w + 3 + panel_w : panel_w;
    if (group_w > cols - 1) group_w = cols - 1;
    int left = (cols - group_w) / 2;
    int top = tiny ? 0 : (rows - art_h) / 2;
    int details_x = art_w ? left + art_w + 3 : left;
    int text_w = panel_w;
    static int layout_left = -1, layout_art_w = -1, layout_panel_w = -1;
    if (left != layout_left || art_w != layout_art_w || panel_w != layout_panel_w) {
        layout_left = left;
        layout_art_w = art_w;
        layout_panel_w = panel_w;
        redraw_art = true;
        frame_changed = true;
        kitty_delete_marquee(&title_marquee);
        kitty_delete_marquee(&album_marquee);
        kitty_delete_marquee(&playlist_marquee);
        kitty_delete_marquee(&next_marquee);
        fputs("\033[2J", stdout);
        reset_detail_rows();
    }
    if (detail_row_count != rows) {
        reset_detail_rows();
        detail_rows = calloc((size_t)rows, sizeof(*detail_rows));
        if (detail_rows) detail_row_count = rows;
    }
    if (detail_rows) for (int i = 0; i < detail_row_count; i++) detail_rows[i].touched = false;
    static unsigned long shown_video_generation;
    if (redraw_art || !local_has_video()) shown_video_generation = 0;
    pthread_mutex_lock(&artwork_mutex);
    int r, g, b;
    color_for(song, left, top, &r, &g, &b);
    if (local_has_video()) {
        if (!strcmp(cfg.theme, "cover")) {
            if (!local_video_color(&r, &g, &b)) r = g = b = 190;
        }
        Image frame = {0};
        if (!tiny && !cfg.minimal && strcmp(cfg.art_mode, "none") && local_copy_video(&frame, &shown_video_generation)) {
            draw_art(song, &frame, left, top, art_w, art_h, r, g, b, redraw_art);
        }
    }
    if (!tiny && !cfg.minimal && strcmp(cfg.art_mode, "none")) {
        if (!local_has_video() && redraw_art) draw_art(song, &current_artwork, left, top, art_w, art_h, r, g, b, true);
    }
    pthread_mutex_unlock(&artwork_mutex);
    char text[MAX_FIELD];
    bool show_album = !tiny && !cfg.minimal && cfg.show_album && song->album[0];
    bool show_track = !tiny && !cfg.minimal && cfg.show_track && song->track[0];
    bool show_playlist = !tiny && !cfg.minimal && cfg.show_playlist && song->player[0] && playlist_display[0];
    bool show_source = !tiny && !cfg.minimal && cfg.show_source && song->player[0];
    bool show_status = !tiny && !cfg.minimal && cfg.show_status;
    bool show_volume = !tiny && !cfg.minimal && cfg.show_volume;
    bool show_next = !tiny && !cfg.minimal && cfg.show_next && song->player[0] && queued_track[0];
    pthread_mutex_lock(&lyric_mutex);
    bool show_lyrics = !tiny && !cfg.minimal && cfg.lyrics && lyrics_loaded && lyric_count > 0;
    pthread_mutex_unlock(&lyric_mutex);
    int detail_height = 3 + show_album + show_track + show_playlist + show_source + show_status + show_volume + show_next;
    detail_height += (cfg.visualizer && cava_enabled) || cfg.show_time;
    detail_height += cfg.show_time;
    int lyric_budget = cfg.lyric_lines + 2;
    int available_lyrics = rows - 2 - detail_height - show_next;
    if (lyric_budget > available_lyrics) lyric_budget = available_lyrics;
    if (lyric_budget < 3) show_lyrics = false;
    if (show_lyrics) detail_height += lyric_budget + show_next;
    int details_y = art_w ? top + (art_h - detail_height) / 2 : (rows - detail_height) / 2;
    if (details_y < 0) details_y = 0;
    if (details_y + detail_height >= rows) details_y = rows > detail_height + 1 ? rows - detail_height - 1 : 0;
    int line = 0;
    snprintf(text, sizeof(text), "%s", song->title[0] ? song->title : "waiting for media playback");
    clean_text(text);
    prepare_marquee_row(&title_marquee, details_x, details_y + line, r, g, b, true);
    draw_marquee(&title_marquee, text, text_w, cols, details_x, details_y + line++, r, g, b, true, now);
    snprintf(text, sizeof(text), "%s", song->artist[0] ? song->artist : "unknown artist");
    clean_text(text);
    draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    if (!tiny && !cfg.minimal && cfg.show_album && song->album[0]) {
        snprintf(text, sizeof(text), "%s", song->album);
        clean_text(text);
        prepare_marquee_row(&album_marquee, details_x, details_y + line, r, g, b, false);
        draw_marquee(&album_marquee, text, text_w, cols, details_x, details_y + line++, r, g, b, false, now);
    } else kitty_delete_marquee(&album_marquee);
    if (!tiny && !cfg.minimal && cfg.show_track && song->track[0]) {
        snprintf(text, sizeof(text), "track %s", song->track);
        draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    }
    if (show_playlist) {
        snprintf(text, sizeof(text), "%s", playlist_display);
        clean_text(text);
        bool prefix = prepare_marquee_row(&playlist_marquee, details_x, details_y + line, r, g, b, false);
        if (prefix) { move_to(details_x, details_y + line); rgb(r, g, b); fputs("playlist: ", stdout); reset_color(); frame_changed = true; }
        draw_marquee(&playlist_marquee, text, text_w - 10, cols, details_x + 10, details_y + line++, r, g, b, false, now);
    } else kitty_delete_marquee(&playlist_marquee);
    line++;
    if (cfg.visualizer && cava_enabled) {
        int bars = cfg.bars < panel_w ? cfg.bars : panel_w;
        int filled = song->length > 0 ? (int)(song->position / song->length * bars) : 0;
        if (filled < 0) filled = 0;
        if (filled > bars) filled = bars;
        static const char *levels[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        char visualizer[512] = "";
        for (int i = 0; i < bars; i++) {
            int level = (cava_values[i] * 7 + 500) / 1000;
            if (level < 0) level = 0;
            if (level > 7) level = 7;
            if (cfg.dim_unplayed && i == filled) strcat(visualizer, "\033[2m");
            strcat(visualizer, levels[level]);
        }
        draw_detail_row(visualizer, details_x, details_y + line++, r, g, b, false);
    } else if (cfg.show_time) {
        int bars = panel_w - 1;
        int filled = song->length > 0 ? (int)(song->position / song->length * bars) : 0;
        if (filled < 0) filled = 0;
        if (filled > bars) filled = bars;
        char progress[512] = "";
        for (int i = 0; i < bars && i < 160; i++) strcat(progress, i < filled ? "━" : "─");
        draw_detail_row(progress, details_x, details_y + line++, r, g, b, false);
    }
    if (cfg.show_time) {
        char elapsed[32], duration[32];
        format_time(song->position, elapsed, sizeof(elapsed));
        format_time(song->length, duration, sizeof(duration));
        snprintf(text, sizeof(text), "%s / %s", elapsed, duration);
        draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    }
    if (!tiny && !cfg.minimal && cfg.show_source && song->player[0]) {
        const char *quality = lossless_label(song);
        char player_name[128]; set_string(player_name, sizeof(player_name), song->player); lowercase(player_name);
        snprintf(text, sizeof(text), "source: %s%s%s", player_name, quality ? " • " : "", quality ? quality : "");
        draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    }
    if (!tiny && !cfg.minimal && cfg.show_status) {
        char status[64];
        set_string(status, sizeof(status), song->status[0] ? song->status : "stopped");
        lowercase(status);
        snprintf(text, sizeof(text), "status: %s", status);
        draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    }
    if (!tiny && !cfg.minimal && cfg.show_volume) {
        snprintf(text, sizeof(text), "volume: %.0f%%", song->volume);
        draw_static_field(text, text_w, details_x, details_y + line++, r, g, b);
    }
    bool next_visible = false;
    if (!tiny && !cfg.minimal && cfg.show_next && song->player[0] && queued_track[0]) {
        snprintf(text, sizeof(text), "%s", queued_track);
        clean_text(text);
        if (details_y + line < rows - 1) {
            bool prefix = prepare_marquee_row(&next_marquee, details_x, details_y + line, r, g, b, false);
            if (prefix) { move_to(details_x, details_y + line); rgb(r, g, b); fputs("next: ", stdout); reset_color(); frame_changed = true; }
            draw_marquee(&next_marquee, text, text_w - 6, cols, details_x + 6, details_y + line++, r, g, b, false, now);
            next_visible = true;
        }
    }
    pthread_mutex_lock(&lyric_mutex);
    int lyric_space = show_lyrics ? lyric_budget : 0;
    if (next_visible && show_lyrics) {
        line++;
    }
    if (!tiny && !cfg.minimal && cfg.lyrics && lyrics_loaded && lyric_count && lyric_space > 0) {
        if (wrapped_generation != lyric_generation || wrapped_width != text_w || wrapped_clean != cfg.clean) {
            reset_wrapped_lyrics();
            wrapped_generation = lyric_generation;
            wrapped_width = text_w;
            wrapped_clean = cfg.clean;
        }
        double position = song->position + cfg.lyric_offset;
        int active = -1;
        for (int i = 0; i < lyric_count; i++) if (lyrics[i].timestamp <= position) active = i;
        if (active != active_lyric_line) { active_lyric_line = active; lyric_transition_start = now; }
        double fade = cfg.smooth_scroll ? fmin(1.0, fmax(0.0, (now - lyric_transition_start) / cfg.transition_duration)) : 1.0;
        int lyric_r = (int)(r * (0.65 + 0.35 * fade));
        int lyric_g = (int)(g * (0.65 + 0.35 * fade));
        int lyric_b = (int)(b * (0.65 + 0.35 * fade));
        int focus = active < 0 ? 0 : active;
        int lyric_top = details_y + line;
        int lyric_bottom = lyric_top + lyric_space;
        int anchor_offset = cfg.lyric_lines / 2;
        if (anchor_offset >= lyric_space) anchor_offset = lyric_space - 1;
        int anchor_y = lyric_top + anchor_offset;
        int previous_count = active < 0 ? 0 : active < cfg.lyric_lines / 2 ? active : cfg.lyric_lines / 2;
        int previous_space = anchor_y - lyric_top;
        int previous_y = anchor_y;
        for (int i = focus - 1, shown = 0; i >= 0 && shown < previous_count && previous_space > 0; i--, shown++) {
            if (!prepare_wrapped_lyric(i, text_w)) break;
            int height = wrapped_lyrics[i].height;
            int take = height < previous_space ? height : previous_space;
            previous_y -= take;
            draw_lyric_segments(wrapped_lyrics[i].text, text_w, height - take, take, details_x - 2, previous_y, false, r / 2, g / 2, b / 2, r, g, b);
            previous_space -= take;
        }
        int next_y = anchor_y;
        if (prepare_wrapped_lyric(focus, text_w)) {
            int next_count = cfg.lyric_lines - 1 - previous_count;
            int reserve_next = next_count > 0 && focus + 1 < lyric_count && lyric_bottom - next_y > 1 ? 1 : 0;
            next_y += draw_lyric_segments(wrapped_lyrics[focus].text, text_w, 0, lyric_bottom - next_y - reserve_next, details_x - 2, next_y, active >= 0, active >= 0 ? lyric_r : r / 2, active >= 0 ? lyric_g : g / 2, active >= 0 ? lyric_b : b / 2, r, g, b);
            for (int i = focus + 1, shown = 0; i < lyric_count && shown < next_count && next_y < lyric_bottom; i++, shown++) {
                if (!prepare_wrapped_lyric(i, text_w)) break;
                next_y += draw_lyric_segments(wrapped_lyrics[i].text, text_w, 0, lyric_bottom - next_y, details_x - 2, next_y, false, r / 2, g / 2, b / 2, r, g, b);
            }
        }
    }
    pthread_mutex_unlock(&lyric_mutex);
    if (!next_visible) kitty_delete_marquee(&next_marquee);
    finish_detail_rows();
    draw_notice(rows, cols, now, redraw_art);
    if (frame_changed) move_to(0, rows - 1);
    if (synchronized) fputs("\033[?2026l", stdout);
    fflush(stdout);
    (void)now;
}
