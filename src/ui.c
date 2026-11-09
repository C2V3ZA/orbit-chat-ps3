/* Orbit 0.7 renderer: software 2D on a 1280x720 virtual canvas, scaled to the output mode.
 * Everything is drawn here (no image assets): anti-aliased shapes and controller glyphs. */
#include "ui.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static FT_Library library;
static FT_Face face;
static uint32_t *screen;
static int sw, sh, timezone_minutes;
/* ---------- glyph cache: four-way, bounded to 2 MiB ---------- */
typedef struct {
    uint32_t cp, age;
    int size, advance, left, top, width, height;
    unsigned char *pixels;
} Glyph;
static Glyph glyphs[2048][4];
static size_t glyph_bytes;
static uint32_t glyph_age;
static int font_size;
static void clear_glyphs(void) {
    for (int i = 0; i < 2048; i++)
        for (int j = 0; j < 4; j++)
            free(glyphs[i][j].pixels);
    memset(glyphs, 0, sizeof(glyphs));
    glyph_bytes = 0;
}
static Glyph *glyph(uint32_t cp, int size) {
    if (size < 1)
        size = 1;
    Glyph *set = glyphs[(cp * 131u + (unsigned)size * 977u) & 2047], *slot = &set[0];
    for (int i = 0; i < 4; i++) {
        if (set[i].size == size && set[i].cp == cp) {
            set[i].age = ++glyph_age;
            return &set[i];
        }
        if (!set[i].size || (slot->size && set[i].age < slot->age))
            slot = &set[i];
    }
    if (font_size != size) {
        if (FT_Set_Pixel_Sizes(face, 0, size))
            return NULL;
        font_size = size;
    }
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
        return NULL;
    FT_GlyphSlot g = face->glyph;
    size_t bytes = (size_t)g->bitmap.width * g->bitmap.rows;
    if (bytes > 65536)
        return NULL;
    if (glyph_bytes + bytes > 6 * 1024 * 1024) {
        clear_glyphs();
        slot = &set[0];
    }
    glyph_bytes -= (size_t)slot->width * slot->height;
    free(slot->pixels);
    memset(slot, 0, sizeof(*slot));
    if (bytes) {
        slot->pixels = malloc(bytes);
        if (!slot->pixels)
            return NULL;
        for (int y = 0; y < (int)g->bitmap.rows; y++)
            memcpy(slot->pixels + y * g->bitmap.width, g->bitmap.buffer + y * g->bitmap.pitch,
                   g->bitmap.width);
    }
    slot->cp = cp;
    slot->size = size;
    slot->age = ++glyph_age;
    slot->advance = (int)(g->advance.x >> 6);
    slot->left = g->bitmap_left;
    slot->top = g->bitmap_top;
    slot->width = (int)g->bitmap.width;
    slot->height = (int)g->bitmap.rows;
    glyph_bytes += bytes;
    return slot;
}
/* ---------- palette ---------- */
static const uint32_t C_BG = 0x0d0f13, C_SIDE = 0x111318, C_SURF = 0x171a20, C_SURF2 = 0x1f232a,
                      C_LINE = 0x262a32, C_TEXT = 0xf3f5f8, C_MUTED = 0x9aa1ab, C_DIM = 0x636a74,
                      C_ACCENT = 0x4f8cff, C_OK = 0x39d98a, C_WARN = 0xf4b445, C_BAD = 0xff5d62,
                      G_CROSS = 0x8ab6ff, G_CIRCLE = 0xff6f6f, G_SQUARE = 0xf39ad9,
                      G_TRIANGLE = 0x43dcb7;
static char text_rows[64][1024];
void ui_set_timezone(int minutes) {
    timezone_minutes = minutes;
}
int ui_init(const char *path) {
    return FT_Init_FreeType(&library) || FT_New_Face(library, path, 0, &face) ? -1 : 0;
}
static uint32_t *bg_cache;
static size_t bg_size;
void ui_close(void) {
    clear_glyphs();
    free(bg_cache);
    bg_cache = NULL;
    bg_size = 0;
    font_size = 0;
    if (face)
        FT_Done_Face(face);
    if (library)
        FT_Done_FreeType(library);
    face = NULL;
    library = NULL;
}
/* ---------- primitives (virtual 1280x720 coordinates) ---------- */
static int sx(int x) {
    return x * sw / 1280;
}
static int sy(int y) {
    return y * sh / 720;
}
static float fx(float x) {
    return x * (float)sw / 1280.0f;
}
static float fy(float y) {
    return y * (float)sh / 720.0f;
}
static inline uint32_t blend(uint32_t a, uint32_t b, unsigned t) {
    unsigned u = 255 - t;
    unsigned r = (((a >> 16) & 255) * u + ((b >> 16) & 255) * t + 128) * 257 >> 16;
    unsigned g = (((a >> 8) & 255) * u + ((b >> 8) & 255) * t + 128) * 257 >> 16;
    unsigned c = ((a & 255) * u + (b & 255) * t + 128) * 257 >> 16;
    return r << 16 | g << 8 | c;
}
static void fill_px(int x0, int y0, int x1, int y1, uint32_t c) {
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > sw)
        x1 = sw;
    if (y1 > sh)
        y1 = sh;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = screen + j * sw;
        for (int i = x0; i < x1; i++)
            row[i] = c;
    }
}
static void box(int x, int y, int w, int h, uint32_t c) {
    fill_px(sx(x), sy(y), sx(x + w), sy(y + h), c);
}
static inline void plot(int x, int y, uint32_t c, float cover) {
    if (cover <= 0.0f || x < 0 || y < 0 || x >= sw || y >= sh)
        return;
    uint32_t *p = &screen[y * sw + x];
    *p = cover >= 1.0f ? c : blend(*p, c, (unsigned)(cover * 255.0f));
}
/* Rounded rectangle with anti-aliased corners. */
static void rrect(int x, int y, int w, int h, int r, uint32_t c) {
    float X0 = fx((float)x), Y0 = fy((float)y), X1 = fx((float)(x + w)), Y1 = fy((float)(y + h));
    float R = fy((float)r);
    if (R * 2 > X1 - X0)
        R = (X1 - X0) / 2;
    if (R * 2 > Y1 - Y0)
        R = (Y1 - Y0) / 2;
    int ri = (int)ceilf(R);
    int ix0 = (int)X0, iy0 = (int)Y0, ix1 = (int)X1, iy1 = (int)Y1;
    fill_px(ix0 + ri, iy0, ix1 - ri, iy1, c);
    fill_px(ix0, iy0 + ri, ix0 + ri, iy1 - ri, c);
    fill_px(ix1 - ri, iy0 + ri, ix1, iy1 - ri, c);
    for (int j = 0; j < ri; j++)
        for (int i = 0; i < ri; i++) {
            float dx = R - (i + 0.5f), dy = R - (j + 0.5f);
            float cover = R - sqrtf(dx * dx + dy * dy) + 0.5f;
            if (cover <= 0)
                continue;
            if (cover > 1)
                cover = 1;
            plot(ix0 + i, iy0 + j, c, cover);
            plot(ix1 - 1 - i, iy0 + j, c, cover);
            plot(ix0 + i, iy1 - 1 - j, c, cover);
            plot(ix1 - 1 - i, iy1 - 1 - j, c, cover);
        }
}
static void disc(float cx, float cy, float r, uint32_t c) {
    float X = fx(cx), Y = fy(cy), R = fy(r);
    for (int j = (int)(Y - R - 1); j <= (int)(Y + R + 1); j++)
        for (int i = (int)(X - R - 1); i <= (int)(X + R + 1); i++) {
            float dx = i + 0.5f - X, dy = j + 0.5f - Y;
            float cover = R - sqrtf(dx * dx + dy * dy) + 0.5f;
            plot(i, j, c, cover > 1 ? 1 : cover);
        }
}
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}
/* ---------- controller glyphs ---------- */
enum { K_CROSS, K_CIRCLE, K_SQUARE, K_TRIANGLE };
static void face_button(float cx, float cy, float d, int kind) {
    disc(cx, cy, d / 2, 0x2a2f37);
    disc(cx, cy, d / 2 - 1.3f, 0x1a1d23);
    float X = fx(cx), Y = fy(cy), s = fy(d) * 0.26f, w = fy(d) * 0.05f + 0.45f;
    uint32_t color = kind == K_CROSS    ? G_CROSS
                     : kind == K_CIRCLE ? G_CIRCLE
                     : kind == K_SQUARE ? G_SQUARE
                                        : G_TRIANGLE;
    int r = (int)(fy(d) / 2) + 1;
    for (int j = (int)Y - r; j <= (int)Y + r; j++)
        for (int i = (int)X - r; i <= (int)X + r; i++) {
            float px = i + 0.5f, py = j + 0.5f, dist;
            if (kind == K_CROSS)
                dist = fminf(seg_dist(px, py, X - s, Y - s, X + s, Y + s),
                             seg_dist(px, py, X - s, Y + s, X + s, Y - s));
            else if (kind == K_CIRCLE)
                dist = fabsf(sqrtf((px - X) * (px - X) + (py - Y) * (py - Y)) - s * 1.02f);
            else if (kind == K_SQUARE) {
                float q = s * 0.86f, ax = fabsf(px - X) - q, ay = fabsf(py - Y) - q;
                float out = sqrtf(fmaxf(ax, 0) * fmaxf(ax, 0) + fmaxf(ay, 0) * fmaxf(ay, 0));
                dist = fabsf(out + fminf(fmaxf(ax, ay), 0));
            } else {
                float t = s * 1.12f, top = Y - t * 0.95f, base = Y + t * 0.62f;
                dist = fminf(fminf(seg_dist(px, py, X, top, X - t, base),
                                   seg_dist(px, py, X - t, base, X + t, base)),
                             seg_dist(px, py, X + t, base, X, top));
            }
            float cover = w - dist + 0.5f;
            if (cover > 0)
                plot(i, j, color, cover > 1 ? 1 : cover);
        }
}
static uint32_t codepoint(const unsigned char **p) {
    uint32_t c = *(*p)++;
    int n = c < 128 ? 0 : c < 224 ? 1 : c < 240 ? 2 : 3;
    if (!n)
        return c;
    c &= (1U << (6 - n)) - 1;
    while (n-- && **p)
        c = (c << 6) | (*(*p)++ & 63);
    return c;
}
static int text_raw(int x, int y, int size, uint32_t color, const char *str, int maxw) {
    if (!face)
        return x;
    int pen = sx(x), limit = sx(x + maxw), baseline = sy(y + size);
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        Glyph *g = glyph(codepoint(&p), sy(size));
        if (!g)
            continue;
        if (pen + g->advance > limit)
            break;
        for (int j = 0; j < g->height; j++) {
            int dy = baseline - g->top + j;
            if (dy < 0 || dy >= sh)
                continue;
            const unsigned char *src = g->pixels + j * g->width;
            uint32_t *row = screen + dy * sw;
            for (int i = 0; i < g->width; i++) {
                int dx = pen + g->left + i;
                unsigned alpha = src[i];
                if (alpha && dx >= 0 && dx < sw && dx < limit)
                    row[dx] = alpha == 255 ? color : blend(row[dx], color, alpha);
            }
        }
        pen += g->advance;
    }
    return pen * 1280 / sw;
}
static int measured(const char *str, int size) {
    const unsigned char *p = (const unsigned char *)str;
    int width = 0;
    while (*p) {
        Glyph *g = glyph(codepoint(&p), sy(size));
        if (g)
            width += g->advance;
    }
    return width;
}
static int text_width(const char *str, int size) {
    return measured(str, size) * 1280 / (sw ? sw : 1280);
}
static int text(int x, int y, int size, uint32_t color, const char *str, int maxw) {
    if (!face || maxw <= 0)
        return x;
    if (measured(str, size) <= sx(maxw))
        return text_raw(x, y, size, color, str, maxw);
    char clipped[1100];
    const unsigned char *p = (const unsigned char *)str;
    int pixels = 0, n = 0, limit = sx(maxw) - measured("...", size);
    while (*p && n < 1090) {
        const unsigned char *start = p;
        Glyph *g = glyph(codepoint(&p), sy(size));
        if (!g)
            continue;
        if (pixels + g->advance > limit)
            break;
        while (start < p)
            clipped[n++] = (char)*start++;
        pixels += g->advance;
    }
    memcpy(clipped + n, "...", 4);
    return text_raw(x, y, size, color, clipped, maxw);
}
/* Faux bold for headings: one font file, drawn twice with a subpixel-sized offset. */
static int bold(int x, int y, int size, uint32_t color, const char *str, int maxw) {
    text(x, y, size, color, str, maxw);
    if (sw >= 1000)
        text_raw(x + 1, y, size, color, str, maxw - 1);
    return x + text_width(str, size) + 1;
}
static void center(int cx, int y, int size, uint32_t color, const char *str, int heading) {
    int w = text_width(str, size);
    if (heading)
        bold(cx - w / 2, y, size, color, str, w + 4);
    else
        text(cx - w / 2, y, size, color, str, w + 4);
}
static int split_lines(const char *str, int size, int width, char lines[64][1024]) {
    const unsigned char *p = (const unsigned char *)str;
    int row = 0;
    for (; row < 64 && *p; row++) {
        while (*p == ' ')
            p++;
        int n = 0, pixels = 0, last_space = -1;
        const unsigned char *next_word = NULL;
        while (*p && n < 1000) {
            const unsigned char *start = p;
            uint32_t cp = codepoint(&p);
            Glyph *g = glyph(cp, sy(size));
            if (!g)
                continue;
            if (pixels + g->advance > sx(width) && n) {
                if (last_space >= 0) {
                    n = last_space;
                    p = next_word;
                } else
                    p = start;
                break;
            }
            if (cp == ' ') {
                last_space = n;
                next_word = p;
            }
            while (start < p)
                lines[row][n++] = (char)*start++;
            pixels += g->advance;
        }
        lines[row][n] = 0;
    }
    return row;
}
static int wrapped(int x, int y, int size, uint32_t color, const char *str, int width, int lines,
                   int gap) {
    int count = split_lines(str, size, width, text_rows);
    int i = 0;
    for (; i < count && i < lines; i++) {
        if (i == lines - 1 && count > lines) {
            size_t n = strlen(text_rows[i]);
            if (n + 4 < sizeof(text_rows[i]))
                memcpy(text_rows[i] + n, " ...", 5);
        }
        text(x, y + i * (size + gap), size, color, text_rows[i], width);
    }
    return i;
}
/* Small labelled key: L1, R1, START, SELECT, R3. Returns right edge. */
static int key_cap(int x, int y, const char *label) {
    int w = text_width(label, 13) + 16;
    rrect(x, y + 2, w, 24, 7, 0x2a2f37);
    rrect(x + 1, y + 3, w - 2, 22, 6, 0x1a1d23);
    text(x + 8, y + 5, 13, C_TEXT, label, w);
    return x + w;
}
static void dpad(int x, int y, int vertical) {
    int c = 0x2a2f37;
    rrect(x + 8, y + 1, 10, 26, 3, c);
    rrect(x - 1, y + 9, 28, 10, 3, c);
    if (vertical) {
        rrect(x + 10, y + 3, 6, 8, 2, C_TEXT);
        rrect(x + 10, y + 17, 6, 8, 2, C_TEXT);
    } else {
        rrect(x + 1, y + 11, 8, 6, 2, C_TEXT);
        rrect(x + 17, y + 11, 8, 6, 2, C_TEXT);
    }
}
/* Hint: glyph followed by its label. kind < 0 = labelled key text. Returns next x. */
static int hint(int x, int y, int kind, const char *key, const char *label) {
    int end;
    if (kind >= 0) {
        face_button((float)x + 14, (float)y + 14, 28, kind);
        end = x + 28;
    } else if (kind == -2) {
        dpad(x, y, 1);
        end = x + 28;
    } else if (kind == -3) {
        dpad(x, y, 0);
        end = x + 28;
    } else
        end = key_cap(x, y, key);
    int after = text(end + 9, y + 5, 17, C_MUTED, label, 300);
    return after + 30;
}
static void footer_bar(void) {
    box(0, 664, 1280, 56, C_BG);
    box(48, 664, 1184, 1, C_LINE);
}
/* ---------- brand mark: planet with orbit ---------- */
static void orbit_mark(float cx, float cy, float r) {
    float X = fx(cx), Y = fy(cy), R = fy(r);
    int ext = (int)(R * 1.7f) + 2;
    for (int j = (int)Y - ext; j <= (int)Y + ext; j++)
        for (int i = (int)X - ext; i <= (int)X + ext; i++) {
            float px = i + 0.5f - X, py = j + 0.5f - Y;
            /* rotated ellipse ring */
            float c = 0.866f, s = -0.5f;
            float ex = (px * c - py * s) / (R * 1.55f), ey = (px * s + py * c) / (R * 0.48f);
            float e = sqrtf(ex * ex + ey * ey);
            float ring = (1.0f - fabsf(e - 1.0f) * R * 0.48f / (R * 0.09f + 0.8f));
            float d = sqrtf(px * px + py * py);
            float planet = R * 0.62f - d + 0.5f;
            int front = (px * s + py * c) > 0;
            if (!front && ring > 0)
                plot(i, j, C_ACCENT, ring > 1 ? 1 : ring);
            if (planet > 0) {
                float shade = 0.75f + 0.25f * (-px - py) / (R * 0.9f);
                uint32_t col = blend(0x2c63d8, 0x8fb5ff, (unsigned)(fminf(fmaxf(shade, 0), 1) * 255));
                plot(i, j, col, planet > 1 ? 1 : planet);
            }
            if (front && ring > 0 && d > R * 0.5f)
                plot(i, j, C_ACCENT, ring > 1 ? 1 : ring);
        }
}
static void avatar(int x, int y, int d, const char *name, int mine) {
    static const uint32_t colors[] = {0x4f8cff, 0x39d98a, 0xf4b445, 0xff6f91, 0xa98bff, 0x2fc6d6};
    unsigned h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 33 + *p;
    uint32_t c = mine ? 0x4f8cff : colors[h % 6];
    disc((float)x + d / 2.0f, (float)y + d / 2.0f, d / 2.0f, blend(C_BG, c, 70));
    char initial[5] = "?";
    if (name && name[0]) {
        const unsigned char *p = (const unsigned char *)name, *start = p;
        codepoint(&p);
        size_t n = (size_t)(p - start);
        if (n <= 4) {
            memcpy(initial, start, n);
            initial[n] = 0;
            if (n == 1 && initial[0] >= 'a' && initial[0] <= 'z')
                initial[0] -= 32;
        }
    }
    int size = d * 45 / 100;
    int w = text_width(initial, size);
    text(x + (d - w) / 2, y + (d - size) / 2 - size / 8, size, c, initial, d);
}
static void status_dot(int x, int y, int state) {
    uint32_t c = state == LINK_ONLINE ? C_OK : state == LINK_CONNECTING ? C_WARN : C_DIM;
    disc((float)x, (float)y, 4.5f, c);
}
static void chip(int x, int y, uint32_t dot, const char *label) {
    int w = text_width(label, 14) + 34;
    rrect(x, y, w, 28, 14, C_SURF2);
    disc((float)x + 14, (float)y + 14, 4, dot);
    text(x + 25, y + 5, 14, C_MUTED, label, w);
}
static void clock_text(char out[24], uint32_t t) {
    out[0] = 0;
    if (!t)
        return;
    long local = (long)t + timezone_minutes * 60L;
    long today = ((long)time(NULL) + timezone_minutes * 60L) / 86400;
    int minute = (int)(local / 60 % 60), hour = (int)(local / 3600 % 24);
    if (local / 86400 == today)
        snprintf(out, 24, "%02d:%02d", hour, minute);
    else if (local / 86400 == today - 1)
        snprintf(out, 24, "Gestern %02d:%02d", hour, minute);
    else {
        time_t tt = (time_t)local;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(out, 24, "%02d.%02d. %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, hour, minute);
    }
}
/* ---------- toast ---------- */
static uint32_t toast_id;
static uint64_t toast_at;
int ui_toast_visible(const UiState *s, uint64_t now) {
    if (s->status_id != toast_id) {
        toast_id = s->status_id;
        toast_at = now;
    }
    return s->status[0] && s->status_id && now - toast_at < 4500;
}
static void toast(const UiState *s, const UiLocal *l) {
    if (!ui_toast_visible(s, l->now))
        return;
    int w = text_width(s->status, 17) + 48;
    if (w > 900)
        w = 900;
    int x = 802 - w / 2, y = 122;
    rrect(x - 1, y - 1, w + 2, 46, 23, C_LINE);
    rrect(x, y, w, 44, 22, 0x252a33);
    text(x + 24, y + 11, 17, C_TEXT, s->status, w - 44);
}
/* ---------- screens ---------- */
static const char *net_label(int net) {
    return net == NET_INTERNET ? "Internet bereit"
           : net == NET_HOME   ? "Nur Heimnetz"
           : net == NET_OFFLINE ? "Offline"
                                : "Prüfe Netz ...";
}
static uint32_t net_color(int net) {
    return net == NET_INTERNET ? C_OK : net == NET_HOME ? C_WARN : net == NET_OFFLINE ? C_BAD : C_DIM;
}
static void mic_icon(int x, int y, uint32_t c, int muted) {
    rrect(x + 4, y, 8, 13, 4, c);
    rrect(x + 7, y + 15, 2, 4, 1, c);
    rrect(x + 3, y + 19, 10, 2, 1, c);
    if (muted)
        for (int i = 0; i < 18; i++)
            rrect(x + i, y + 1 + i, 2, 2, 1, C_BAD);
}
/* Discord-style voice panel, top left: who is in the call, who is talking right now. */
static int call_card(const UiState *s, int y) {
    int shown = s->call_count > 5 ? 5 : s->call_count;
    int h = 92 + (shown ? shown * 32 + 6 : 26);
    rrect(40, y, 284, h, 14, 0x14241c);
    rrect(40, y, 284, h, 14, 0x14241c);
    uint32_t dot = s->call_state == CALL_ONLINE ? C_OK : C_WARN;
    disc(58, (float)y + 21, 4.5f, dot);
    text(70, y + 12, 13, dot, "SPRACHCHAT", 120);
    const char *state = s->call_state == CALL_ONLINE       ? "verbunden"
                        : s->call_state == CALL_CONNECTING ? "verbinde ..."
                                                           : "neuer Versuch ...";
    int w = text_width(state, 13);
    text(308 - w, y + 12, 13, C_MUTED, state, w + 4);
    bold(56, y + 34, 17, C_TEXT, s->call_name[0] ? s->call_name : "Gruppe", 250);
    int py = y + 64;
    for (int i = 0; i < shown; i++) {
        const UiCallPerson *p = &s->call_people[i];
        if (p->speaking)
            disc(70, (float)py + 13, 15, C_OK);
        disc(70, (float)py + 13, p->speaking ? 12.5f : 13, 0x14241c);
        avatar(57, py, 26, p->name, p->self);
        char label[80];
        snprintf(label, sizeof(label), "%s%s", p->name, p->self ? " (du)" : "");
        text(92, py + 4, 15, p->speaking ? C_TEXT : 0xc4c9d0, label, 180);
        if (p->self)
            mic_icon(290, py + 3, s->call_muted ? C_BAD : C_MUTED, s->call_muted);
        py += 32;
    }
    if (!shown)
        text(56, py, 14, C_MUTED, "Du bist gleich drin ...", 250), py += 26;
    else if (s->call_count > shown) {
        char more[32];
        snprintf(more, sizeof(more), "+%d weitere", s->call_count - shown);
        text(250, y + 36, 13, C_MUTED, more, 70);
    }
    py += 2;
    int x = key_cap(56, py, "SELECT");
    x = text(x + 6, py + 4, 13, C_MUTED, "Auflegen", 90);
    x = key_cap(x + 14, py, "R2");
    text(x + 6, py + 4, 13, C_MUTED, s->call_muted ? "Ton an" : "Stumm", 80);
    return h;
}
static void sidebar(const UiState *s) {
    box(0, 0, 340, 664, C_SIDE);
    box(340, 0, 1, 664, C_LINE);
    orbit_mark(78, 58, 15);
    bold(106, 42, 26, C_TEXT, "Orbit", 140);
    chip(186, 44, net_color(s->net), net_label(s->net));
    int top = 104;
    if (s->call_state != CALL_OFF)
        top += call_card(s, 92) + 4;
    text(56, top, 13, C_DIM, "GRUPPEN", 200);
    int rows = (590 - (top + 24)) / 58;
    if (rows < 1)
        rows = 1;
    int from = s->selected >= rows ? s->selected - rows + 1 : 0;
    for (int i = from; i < s->group_count && i < from + rows; i++) {
        const UiGroup *g = &s->groups[i];
        int y = top + 24 + (i - from) * 58, active = i == s->selected;
        if (active) {
            rrect(40, y, 284, 52, 12, C_SURF2);
            rrect(40, y + 14, 4, 24, 2, C_ACCENT);
        }
        uint32_t tc = g->state == LINK_ONLINE || g->own ? (active ? C_TEXT : 0xd6dae0) : C_DIM;
        text(62, y + 6, 18, tc, g->name, g->call ? 170 : 220);
        char sub[160];
        if (g->own)
            snprintf(sub, sizeof(sub), "Bei dir  ·  %d %s", g->members,
                     g->members == 1 ? "Person" : "Personen");
        else
            snprintf(sub, sizeof(sub), "%s  ·  bei %s",
                     g->state == LINK_ONLINE       ? "online"
                     : g->state == LINK_CONNECTING ? "verbinde"
                                                   : "offline",
                     g->host[0] ? g->host : "Freund");
        if (!g->own) {
            status_dot(66, y + 38, g->state);
            text(76, y + 30, 13, active ? C_MUTED : C_DIM, sub, 216);
        } else
            text(62, y + 30, 13, active ? C_MUTED : C_DIM, sub, 230);
        if (g->call) {
            char n[16];
            snprintf(n, sizeof(n), "%d", g->call);
            int w = text_width(n, 13) + 36;
            rrect(292 - w, y + 7, w, 22, 11, 0x163a28);
            mic_icon(300 - w, y + 8, C_OK, 0);
            text(318 - w, y + 9, 13, C_OK, n, w);
        }
        if (g->unread)
            disc(306, (float)y + 26, 5.5f, C_ACCENT);
    }
    if (s->group_count > rows) {
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", s->selected + 1, s->group_count);
        text(258, top, 13, C_DIM, pos, 70);
    }
    box(40, 600, 284, 1, C_LINE);
    avatar(44, 612, 36, s->username, 1);
    text(92, 612, 17, C_TEXT, s->username, 230);
    text(92, 634, 13, C_DIM, s->call_state != CALL_OFF ? "Im Sprachchat" : "Diese PS3", 230);
}
static void empty_state(const UiState *s) {
    orbit_mark(790, 196, 44);
    center(790, 268, 34, C_TEXT, "Noch keine Gruppe", 1);
    center(790, 318, 18, C_MUTED, "Erstelle eine Gruppe und lade Freunde mit einem Code ein -", 0);
    center(790, 344, 18, C_MUTED, "oder tippe die Einladung eines Freundes ein.", 0);
    int y = 400;
    rrect(470, y, 310, 112, 18, C_SURF);
    face_button(510, (float)y + 40, 40, K_CROSS);
    bold(544, y + 26, 20, C_TEXT, "Gruppe erstellen", 220);
    text(500, y + 72, 15, C_MUTED, "Sie läuft auf deiner PS3", 270);
    rrect(800, y, 310, 112, 18, C_SURF);
    face_button(840, (float)y + 40, 40, K_SQUARE);
    bold(874, y + 26, 20, C_TEXT, "Einladung eingeben", 220);
    text(830, y + 72, 15, C_MUTED, "20 Zeichen vom Freund", 270);
    if (s->net == NET_HOME) {
        char line[340];
        snprintf(line, sizeof(line), "Hinweis: %s", s->net_detail);
        wrapped(470, 536, 14, C_WARN, line, 640, 4, 5);
    }
}
static void chat_screen(const UiState *s, const UiLocal *l) {
    sidebar(s);
    if (!s->group_count || s->selected < 0) {
        empty_state(s);
        return;
    }
    const UiGroup *g = &s->groups[s->selected];
    text(380, 34, 28, C_DIM, "#", 30);
    bold(406, 34, 28, C_TEXT, g->name, 600);
    char sub[260];
    snprintf(sub, sizeof(sub), "%s", g->people[0] ? g->people : "Nur du");
    text(382, 76, 15, C_MUTED, sub, 640);
    const char *state = g->own                       ? "Läuft auf dieser PS3"
                        : g->state == LINK_ONLINE     ? "Verbunden"
                        : g->state == LINK_CONNECTING ? "Verbinde ..."
                                                      : "Gastgeber offline";
    uint32_t dot = g->own || g->state == LINK_ONLINE ? C_OK
                   : g->state == LINK_CONNECTING     ? C_WARN
                                                     : C_DIM;
    int cw = text_width(state, 14) + 34;
    chip(1232 - cw, 40, dot, state);
    if (g->call && !s->call_here) {
        char line[96];
        snprintf(line, sizeof(line), "%d im Sprachchat  -  SELECT zum Beitreten", g->call);
        int w = text_width(line, 14);
        text(1232 - w, 80, 14, C_OK, line, w + 4);
    }
    box(372, 110, 860, 1, C_LINE);
    int top = 124, bottom = 584;
    if (!s->message_count) {
        if (s->loading)
            center(802, 320, 18, C_MUTED, "Nachrichten werden geladen ...", 0);
        else if (!g->own && g->state != LINK_ONLINE) {
            center(802, 290, 22, C_TEXT, "Gastgeber gerade nicht erreichbar", 1);
            char who[160];
            snprintf(who, sizeof(who), "Die Gruppe liegt auf der PS3 von %s. Sobald dort Orbit "
                                       "läuft, geht es automatisch weiter.",
                     g->host[0] ? g->host : "deinem Freund");
            wrapped(552, 330, 16, C_MUTED, who, 500, 3, 8);
            if (s->link_error[0])
                wrapped(552, 410, 14, C_DIM, s->link_error, 500, 3, 6);
        } else {
            center(802, 290, 24, C_TEXT, "Hier beginnt euer Chat", 1);
            center(802, 330, 17, C_MUTED, "Drücke KREUZ und schreib die erste Nachricht.", 0);
        }
    } else {
        int y = bottom, last = s->message_count - 1 - l->history_offset;
        if (last < 0)
            last = 0;
        for (int i = last; i >= 0 && y > top; i--) {
            const UiMessage *m = &s->messages[i];
            int grouped = i > 0 && !strcmp(s->messages[i - 1].name, m->name) &&
                          m->time - s->messages[i - 1].time < 300;
            int count = split_lines(m->text, 19, 760, text_rows);
            if (count > 14)
                count = 14;
            int h = count * 27 + (grouped ? 6 : 40);
            int y0 = y - h;
            if (!grouped) {
                if (y0 + 2 >= top)
                    avatar(384, y0 + 4, 38, m->name, m->mine);
                if (y0 + 4 >= top) {
                    int e = bold(438, y0 + 4, 17, m->mine ? 0x9cc0ff : C_TEXT, m->name, 500);
                    char t[24];
                    clock_text(t, m->time);
                    text(e + 10, y0 + 7, 13, C_DIM, t, 200);
                }
            }
            int ty = y0 + (grouped ? 3 : 32);
            for (int r = 0; r < count; r++)
                if (ty + r * 27 >= top)
                    text(438, ty + r * 27, 19, 0xdfe3e8, text_rows[r], 780);
            y = y0 - 6;
        }
        if (l->history_offset) {
            char more[64];
            snprintf(more, sizeof(more), "%d neuere  -  R1", l->history_offset);
            int w = text_width(more, 14) + 28;
            rrect(1232 - w, 538, w, 30, 15, C_ACCENT);
            text(1246 - w, 543, 14, 0xffffff, more, w);
        }
    }
    /* composer */
    int online = g->own || g->state == LINK_ONLINE;
    rrect(372, 596, 860, 52, 14, C_SURF2);
    face_button(400, 622, 28, K_CROSS);
    text(426, 611, 18, l->draft[0] ? C_TEXT : C_DIM,
         !online      ? "Offline - Nachricht kann gerade nicht gesendet werden"
         : l->draft[0] ? l->draft
                       : "Nachricht schreiben ...",
         780);
}
static void hints_chat(const UiState *s) {
    footer_bar();
    int x = 56, y = 678;
    if (!s->group_count) {
        x = hint(x, y, K_CROSS, NULL, "Gruppe erstellen");
        x = hint(x, y, K_SQUARE, NULL, "Einladung eingeben");
        x = hint(x, y, K_CIRCLE, NULL, "Menü");
        hint(x, y, -1, "START", "Beenden");
        return;
    }
    x = hint(x, y, -2, NULL, "Gruppe");
    x = hint(x, y, K_CROSS, NULL, "Schreiben");
    x = hint(x, y, -1, "SELECT", s->call_state != CALL_OFF ? "Auflegen" : "Sprachchat");
    if (s->call_state != CALL_OFF)
        x = hint(x, y, -1, "R2", s->call_muted ? "Ton an" : "Stumm");
    x = hint(x, y, K_TRIANGLE, NULL, "Einladen");
    x = hint(x, y, K_SQUARE, NULL, "Code eingeben");
    hint(x, y, K_CIRCLE, NULL, "Menü");
}
static void dim_background(void) {
    for (int j = 0; j < sh; j++) {
        uint32_t *row = screen + j * sw;
        for (int i = 0; i < sw; i++) {
            uint32_t p = row[i];
            row[i] = (p >> 2 & 0x3f3f3f) + (p >> 3 & 0x1f1f1f);
        }
    }
}
static void modal(int x, int y, int w, int h) {
    rrect(x - 1, y - 1, w + 2, h + 2, 22, C_LINE);
    rrect(x, y, w, h, 21, C_SURF);
}
static void invite_page(const UiState *s) {
    modal(240, 80, 800, 520);
    bold(286, 118, 30, C_TEXT, "Freund einladen", 600);
    text(288, 162, 16, C_MUTED, s->invite_group[0] ? s->invite_group : "", 700);
    rrect(286, 206, 708, 120, 16, C_BG);
    if (s->invite_state == 2) {
        int w = text_width(s->invite_code, 46);
        bold(640 - w / 2, 236, 46, C_TEXT, s->invite_code, 700);
    } else if (s->invite_state == 3)
        center(640, 252, 20, C_BAD, "Einladung konnte nicht erstellt werden", 0);
    else
        center(640, 252, 20, C_MUTED, "Einladung wird erstellt ...", 0);
    if (s->invite_state == 2) {
        int internet = !strncmp(s->invite_scope, "Klappt", 6);
        disc(298, 357, 6, internet ? C_OK : C_WARN);
        wrapped(314, 346, 16, internet ? C_OK : C_WARN, s->invite_scope, 660, 2, 6);
    }
    int y = 410;
    const char *steps[] = {"Schick deinem Freund diesen Code - per Handy, Chat oder einfach vorlesen.",
                           "Er drückt in Orbit QUADRAT und tippt den Code ein.",
                           "Fertig: Ihr seid in derselben Gruppe. Kein Server, kein Router-Menü."};
    for (int i = 0; i < 3; i++) {
        disc(300, (float)y + i * 46 + 12, 13, C_SURF2);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        center(300, y + i * 46 + 3, 15, C_TEXT, n, 1);
        text(326, y + i * 46 + 2, 17, 0xd6dae0, steps[i], 660);
    }
    footer_bar();
    hint(56, 678, K_CIRCLE, NULL, "Zurück");
}
static void menu_page(const UiState *s, const UiLocal *l) {
    modal(760, 40, 480, 600);
    bold(796, 72, 26, C_TEXT, "Menü", 300);
    const UiGroup *g = s->group_count && s->selected >= 0 ? &s->groups[s->selected] : NULL;
    const char *items[] = {"Gruppe erstellen",
                           "Einladung eingeben",
                           "Freund einladen",
                           s->call_state != CALL_OFF ? "Sprachchat verlassen" : "Sprachchat beitreten",
                           "Name ändern",
                           "Verbindung & Status",
                           g && g->own ? "Gruppe löschen" : "Gruppe verlassen",
                           "Orbit beenden"};
    const char *subs[] = {"Neue Gruppe auf deiner PS3",
                          "Code eines Freundes eintippen",
                          g ? g->name : "Zuerst eine Gruppe erstellen",
                          s->call_state != CALL_OFF ? s->call_name : g ? g->name : "Zuerst eine Gruppe erstellen",
                          s->username,
                          net_label(s->net),
                          g ? g->name : "Keine Gruppe ausgewählt",
                          "Alles ist gespeichert"};
    for (int i = 0; i < 8; i++) {
        int y = 116 + i * 60, active = i == l->menu_focus;
        int disabled = (i == 2 || i == 3 || i == 6) && !g;
        if (active) {
            rrect(780, y, 440, 54, 12, C_SURF2);
            rrect(780, y + 15, 4, 24, 2, C_ACCENT);
        }
        uint32_t c = disabled ? C_DIM : i == 6 && active ? C_BAD : C_TEXT;
        text(804, y + 6, 19, c, items[i], 400);
        text(804, y + 31, 14, C_DIM, subs[i], 400);
    }
    footer_bar();
    int x = hint(56, 678, -2, NULL, "Auswählen");
    x = hint(x, 678, K_CROSS, NULL, "OK");
    hint(x, 678, K_CIRCLE, NULL, "Zurück");
}
static void leave_page(const UiState *s) {
    const UiGroup *g = s->group_count && s->selected >= 0 ? &s->groups[s->selected] : NULL;
    modal(300, 190, 680, 300);
    int own = g && g->own;
    bold(340, 226, 28, C_TEXT, own ? "Gruppe löschen?" : "Gruppe verlassen?", 600);
    text(342, 272, 18, C_MUTED, g ? g->name : "", 600);
    wrapped(342, 316, 17, 0xd6dae0,
            own ? "Die Gruppe und alle Nachrichten werden für alle Mitglieder gelöscht."
                : "Du kannst später mit einer neuen Einladung wieder beitreten.",
            600, 3, 8);
    footer_bar();
    int x = hint(56, 678, K_CROSS, NULL, own ? "Endgültig löschen" : "Verlassen");
    hint(x, 678, K_CIRCLE, NULL, "Abbrechen");
}
static void network_page(const UiState *s) {
    modal(200, 60, 880, 560);
    bold(240, 96, 28, C_TEXT, "Verbindung & Status", 600);
    int y = 158;
    rrect(240, y, 800, 128, 16, C_BG);
    disc(270, (float)y + 34, 8, net_color(s->net));
    bold(290, y + 21, 21, C_TEXT,
         s->net == NET_INTERNET ? "Erreichbar über das Internet"
         : s->net == NET_HOME   ? "Erreichbar im Heimnetz"
         : s->net == NET_OFFLINE ? "Keine Netzwerkverbindung"
                                 : "Wird geprüft ...",
         700);
    wrapped(290, y + 54, 15, C_MUTED, s->net_detail, 730, 3, 5);
    y = 310;
    char relay[160], tunnel[180];
    snprintf(relay, sizeof(relay), "%s%s", s->relay[0] ? s->relay : "nicht nötig",
             !s->relay[0] ? "" : s->relay_online ? "  -  verbunden" : "  -  nicht erreichbar");
    snprintf(tunnel, sizeof(tunnel), "%s%s", s->tunnel, s->tunnel_online ? "  -  verbunden" : "");
    const char *labels[] = {"Diese PS3 im Heimnetz", "Internet-Tunnel", "Vermittler (optional)",
                            "Treffpunkt-ID"};
    const char *values[] = {s->local_ip, tunnel, relay, s->server_id};
    for (int i = 0; i < 4; i++) {
        text(244, y + i * 44, 15, C_DIM, labels[i], 300);
        text(560, y + i * 44, 17, i == 1 && s->tunnel_online ? C_OK : C_TEXT, values[i], 460);
        box(240, y + i * 44 + 32, 800, 1, C_LINE);
    }
    wrapped(244, 500, 15, C_MUTED,
            "Im selben WLAN finden sich die Konsolen direkt. Übers Internet öffnet Orbit selbst einen "
            "Tunnel - kein PC, kein Router-Menü. Alles bleibt Ende-zu-Ende verschlüsselt.",
            790, 3, 7);
    footer_bar();
    int x = hint(56, 678, K_SQUARE, NULL, "Neu verbinden");
    x = hint(x, 678, K_TRIANGLE, NULL, "Vermittler ändern");
    hint(x, 678, K_CIRCLE, NULL, "Zurück");
}
static void welcome_page(const UiState *s, const UiLocal *l) {
    orbit_mark(640, 150, 46);
    center(640, 222, 40, C_TEXT, "Willkommen bei Orbit", 1);
    center(640, 280, 19, C_MUTED, "Chatte mit Freunden direkt von PS3 zu PS3.", 0);
    center(640, 306, 19, C_MUTED, "Kein Konto, kein Passwort, kein Server.", 0);
    const char *labels[] = {"So sehen dich deine Freunde", "Los geht's"};
    for (int i = 0; i < 2; i++) {
        int y = 366 + i * 92, active = l->focus == i;
        rrect(390, y, 500, 78, 16, active ? C_SURF2 : C_SURF);
        if (active)
            rrect(390, y + 22, 4, 34, 2, C_ACCENT);
        if (i == 0) {
            avatar(414, y + 17, 44, s->username, 1);
            text(472, y + 12, 14, C_DIM, labels[0], 380);
            bold(472, y + 34, 22, C_TEXT, s->username, 360);
        } else {
            bold(472, y + 25, 22, active ? C_TEXT : C_MUTED, labels[1], 360);
            face_button(430, (float)y + 39, 34, K_CROSS);
        }
    }
    footer_bar();
    int x = hint(56, 678, -2, NULL, "Auswählen");
    x = hint(x, 678, K_CROSS, NULL, l->focus == 0 ? "Name ändern" : "Starten");
    hint(x, 678, -1, "START", "Beenden");
}
/* ---------- in-app keyboard (German layout) ---------- */
static const char *kb_lower[4][11] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "ß"},
    {"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä"},
    {"y", "x", "c", "v", "b", "n", "m", ",", ".", "-", "?"}};
static const char *kb_upper[4][11] = {
    {"!", "\"", "@", "#", "%", "&", "/", "(", ")", "=", "+"},
    {"Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "Ü"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", "Ö", "Ä"},
    {"Y", "X", "C", "V", "B", "N", "M", ";", ":", "_", "'"}};
int ui_kb_cols(int row) {
    return row < 4 ? 11 : 4;
}
const char *ui_kb_key(int row, int col, int shift) {
    if (row < 0 || row > 3 || col < 0 || col > 10)
        return NULL;
    return shift ? kb_upper[row][col] : kb_lower[row][col];
}
int ui_kb_special(int row, int col) {
    static const int specials[4] = {KB_SHIFT, KB_SPACE, KB_BACK, KB_DONE};
    return row == 4 && col >= 0 && col < 4 ? specials[col] : 0;
}
static void kb_key(int x, int y, int w, const char *label, int active, int on) {
    rrect(x, y, w, 48, 10, active ? C_ACCENT : on ? 0x2b3b5c : C_SURF2);
    int tw = text_width(label, 20);
    text(x + (w - tw) / 2, y + 11, 20, active ? 0xffffff : C_TEXT, label, w);
}
static void keyboard_overlay(const UiLocal *l) {
    rrect(119, 229, 1042, 432, 22, C_LINE);
    rrect(120, 230, 1040, 430, 22, C_SURF);
    text(150, 248, 14, C_DIM, l->kb_title, 800);
    char count[24];
    snprintf(count, sizeof(count), "%d / %d", (int)strlen(l->kb_text), l->kb_max);
    int cw = text_width(count, 13);
    text(1130 - cw, 249, 13, C_DIM, count, cw + 4);
    rrect(150, 274, 980, 60, 12, C_BG);
    /* show the end of the text so the cursor is always visible */
    const char *t = l->kb_text;
    while (*t && text_width(t, 22) > 930) {
        t++;
        while ((*t & 0xc0) == 0x80)
            t++;
    }
    int end = l->kb_text[0] ? text(170, 289, 22, C_TEXT, t, 940) : 170;
    if (!l->kb_text[0])
        text(170, 289, 22, C_DIM, l->kb_mode ? "z. B. ABCDE-FGHJK-LMNPQ-RSTUV" : "Tippe hier ...",
             900);
    rrect(end + 2, 286, 3, 36, 1, C_ACCENT);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 11; c++)
            kb_key(150 + c * 90, 352 + r * 56, 82, ui_kb_key(r, c, l->kb_shift),
                   l->kb_row == r && l->kb_col == c, 0);
    int y = 352 + 4 * 56;
    const int xs[4] = {150, 330, 780, 960}, ws[4] = {172, 442, 172, 172};
    const char *labels[4] = {"Groß / klein", "Leerzeichen", "Löschen",
                             l->kb_mode == 2 ? "Senden" : "Fertig"};
    for (int i = 0; i < 4; i++)
        kb_key(xs[i], y, ws[i], labels[i], l->kb_row == 4 && l->kb_col == i,
               (i == 0 && l->kb_shift) || i == 3);
    footer_bar();
    int x = hint(56, 678, K_CROSS, NULL, "Taste");
    x = hint(x, 678, K_SQUARE, NULL, "Löschen");
    x = hint(x, 678, K_TRIANGLE, NULL, "Leerzeichen");
    x = hint(x, 678, -1, "L1", "Groß");
    x = hint(x, 678, -1, "START", l->kb_mode == 2 ? "Senden" : "Fertig");
    hint(x, 678, K_CIRCLE, NULL, "Abbrechen");
}
/* Overlays (menu, keyboard, dialogs) reuse a cached, dimmed copy of the screen below:
 * moving the cursor redraws only the overlay instead of the whole chat. */
static int bg_rev = -1, bg_w, bg_h, bg_off, bg_base;
static unsigned bg_draft;
static unsigned hash_text(const char *t) {
    unsigned h = 2166136261u;
    for (; *t; t++)
        h = (h ^ (unsigned char)*t) * 16777619u;
    return h;
}
static void base_screen(const UiState *s, const UiLocal *l) {
    fill_px(0, 0, sw, sh, C_BG);
    if (l->page == PAGE_WELCOME)
        welcome_page(s, l);
    else {
        chat_screen(s, l);
        if (l->page == PAGE_CHAT)
            hints_chat(s);
    }
}
void ui_render(uint32_t *pixels, int width, int height, const UiState *s, const UiLocal *l) {
    screen = pixels;
    sw = width;
    sh = height;
    int overlay = l->kb_active || (l->page != PAGE_CHAT && l->page != PAGE_WELCOME);
    if (!overlay) {
        base_screen(s, l);
        toast(s, l);
        return;
    }
    size_t need = (size_t)sw * (size_t)sh;
    int base = l->page == PAGE_WELCOME ? 1 : l->page == PAGE_CHAT ? 2 : 3;
    unsigned draft = hash_text(l->draft);
    if (bg_size < need) {
        free(bg_cache);
        bg_cache = malloc(need * sizeof(uint32_t));
        bg_size = bg_cache ? need : 0;
        bg_rev = -1;
    }
    if (!bg_cache || bg_rev != s->revision || bg_w != sw || bg_h != sh || bg_off != l->history_offset ||
        bg_base != base || bg_draft != draft || base == 1) {
        UiLocal under = *l;
        under.kb_active = 0;
        if (base == 3)
            under.page = PAGE_CHAT;
        base_screen(s, &under);
        dim_background();
        if (bg_cache) {
            memcpy(bg_cache, screen, need * sizeof(uint32_t));
            bg_rev = s->revision;
            bg_w = sw;
            bg_h = sh;
            bg_off = l->history_offset;
            bg_base = base;
            bg_draft = draft;
        }
    } else
        memcpy(screen, bg_cache, need * sizeof(uint32_t));
    if (l->kb_active)
        keyboard_overlay(l);
    else if (l->page == PAGE_INVITE)
        invite_page(s);
    else if (l->page == PAGE_MENU)
        menu_page(s, l);
    else if (l->page == PAGE_LEAVE)
        leave_page(s);
    else if (l->page == PAGE_NETWORK)
        network_page(s);
    toast(s, l);
}
/* ---------- animated intro (about 3 seconds, skippable) ---------- */
static float clamp01(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
static float ease_out(float t) {
    t = clamp01(t);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
static float ease_back(float t) {
    t = clamp01(t);
    const float c = 1.6f;
    return 1 + (c + 1) * (t - 1) * (t - 1) * (t - 1) + c * (t - 1) * (t - 1);
}
static inline void plot_a(int x, int y, uint32_t c, float cover, float alpha) {
    plot(x, y, c, (cover > 1 ? 1 : cover) * alpha);
}
/* Planet with a tilted ring that draws itself in, a glow and an orbiting moon. */
static void orbit_mark_anim(float cx, float cy, float r, float scale, float reveal, float moon,
                            float alpha) {
    if (scale <= 0.01f || alpha <= 0)
        return;
    float X = fx(cx), Y = fy(cy), R = fy(r);
    float P = R * 0.62f * scale; /* planet radius */
    const float c = 0.866f, s = -0.5f, A = R * 1.55f, B = R * 0.48f, two_pi = 6.2831853f;
    int ext = (int)(R * 1.9f) + 3;
    int y0 = (int)Y - ext, y1 = (int)Y + ext, x0 = (int)X - ext, x1 = (int)X + ext;
    if (y0 < 0)
        y0 = 0;
    if (x0 < 0)
        x0 = 0;
    if (y1 >= sh)
        y1 = sh - 1;
    if (x1 >= sw)
        x1 = sw - 1;
    float glow_r = R * 1.9f * scale;
    for (int j = y0; j <= y1; j++)
        for (int i = x0; i <= x1; i++) {
            float px = i + 0.5f - X, py = j + 0.5f - Y;
            float d = sqrtf(px * px + py * py);
            /* soft glow */
            if (d < glow_r) {
                float g = 1 - d / glow_r;
                plot_a(i, j, 0x2a5fd0, g * g * 0.55f, alpha);
            }
            float lx = px * c - py * s, ly = px * s + py * c;
            float ex = lx / A, ey = ly / B, e = sqrtf(ex * ex + ey * ey);
            float ring = 1.0f - fabsf(e - 1.0f) * B / (R * 0.09f + 0.8f);
            int front = ly > 0;
            if (ring > 0 && reveal < 1) {
                float ang = atan2f(ey, ex) + 1.2f; /* start drawing at the upper left */
                if (ang < 0)
                    ang += two_pi;
                if (ang > two_pi)
                    ang -= two_pi;
                if (ang > reveal * two_pi)
                    ring = 0;
            }
            if (!front && ring > 0)
                plot_a(i, j, C_ACCENT, ring, alpha);
            float planet = P - d + 0.5f;
            if (planet > 0) {
                float shade = 0.72f + 0.28f * (-px - py) / (P * 1.4f + 1);
                uint32_t col = blend(0x2350c4, 0x9cc0ff, (unsigned)(clamp01(shade) * 255));
                plot_a(i, j, col, planet, alpha);
            }
            if (front && ring > 0 && d > P * 0.8f)
                plot_a(i, j, C_ACCENT, ring, alpha);
        }
    /* moon on the ring */
    if (moon >= 0 && reveal > 0.98f) {
        float th = moon * two_pi;
        float lx = cosf(th) * A, ly = sinf(th) * B;
        float mx = lx * c + ly * s, my = -lx * s + ly * c;
        int behind = ly < 0 && sqrtf(mx * mx + my * my) < P;
        if (!behind) {
            float mr = R * 0.11f + 1.5f, MX = X + mx, MY = Y + my;
            for (int j = (int)(MY - mr - 2); j <= (int)(MY + mr + 2); j++)
                for (int i = (int)(MX - mr - 2); i <= (int)(MX + mr + 2); i++) {
                    float dx = i + 0.5f - MX, dy = j + 0.5f - MY, dd = sqrtf(dx * dx + dy * dy);
                    plot_a(i, j, 0xe8f0ff, mr - dd + 0.5f, alpha);
                }
        }
    }
}
static uint32_t *intro_bg;
static int intro_w, intro_h;
static void intro_background(void) {
    if (intro_bg && intro_w == sw && intro_h == sh) {
        memcpy(screen, intro_bg, (size_t)sw * sh * 4);
        return;
    }
    /* deep space: vertical gradient with a soft vignette, computed once */
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++) {
            float u = (i + 0.5f) / sw - 0.5f, v = (j + 0.5f) / sh - 0.45f;
            float glow = clamp01(1 - sqrtf(u * u * 1.2f + v * v * 2.2f) * 1.6f);
            /* ordered dithering hides 8-bit banding in the dark gradient */
            static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9},
                                              {15, 7, 13, 5}};
            float g = glow * glow * 255 + (bayer[j & 3][i & 3] / 16.0f - 0.5f) * 2.5f;
            uint32_t base = blend(0x07080b, 0x0f1626, (unsigned)(g < 0 ? 0 : g > 255 ? 255 : g));
            screen[j * sw + i] = base;
        }
    free(intro_bg);
    intro_bg = malloc((size_t)sw * sh * 4);
    if (intro_bg) {
        memcpy(intro_bg, screen, (size_t)sw * sh * 4);
        intro_w = sw;
        intro_h = sh;
    }
}
static void stars(float t, float alpha) {
    unsigned seed = 1234567;
    for (int k = 0; k < 140; k++) {
        seed = seed * 1103515245u + 12345u;
        float x = (seed >> 8) % 1280;
        seed = seed * 1103515245u + 12345u;
        float y = (seed >> 8) % 640;
        seed = seed * 1103515245u + 12345u;
        float tw = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * (1.5f + (seed % 7) * 0.4f) + k));
        float size = (seed >> 4) % 9 == 0 ? 1.3f : 0.75f;
        float drift = t * (4 + k % 5); /* slow parallax drift */
        float sxp = fx(x - drift), syp = fy(y);
        uint32_t col = k % 9 == 0 ? 0xbcd2ff : 0xffffff;
        for (int j = (int)(syp - 2); j <= (int)(syp + 2); j++)
            for (int i = (int)(sxp - 2); i <= (int)(sxp + 2); i++) {
                float dx = i + 0.5f - sxp, dy = j + 0.5f - syp;
                float cover = size - sqrtf(dx * dx + dy * dy) + 0.5f;
                if (cover > 0)
                    plot_a(i, j, col, cover, alpha * tw * 0.8f);
            }
    }
}
static void text_alpha(int x, int y, int size, uint32_t color, const char *str, int maxw,
                       float alpha, int heading) {
    if (alpha <= 0.01f)
        return;
    uint32_t c = blend(0x0b0d12, color, (unsigned)(clamp01(alpha) * 255));
    if (heading)
        bold(x, y, size, c, str, maxw);
    else
        text(x, y, size, c, str, maxw);
}
#define INTRO_LENGTH 3.2f
int ui_intro(uint32_t *pixels, int width, int height, float t, const char *version) {
    screen = pixels;
    sw = width;
    sh = height;
    intro_background();
    float out = clamp01((t - (INTRO_LENGTH - 0.5f)) / 0.5f); /* final fade into the app */
    float scene = 1 - out;
    stars(t, clamp01(t / 0.8f) * scene);
    /* planet pops in, ring draws itself, moon starts to orbit */
    float grow = ease_back((t - 0.15f) / 0.75f);
    float reveal = ease_out((t - 0.45f) / 0.85f);
    float moon = t > 1.3f ? fmodf(0.62f + (t - 1.3f) * 0.32f, 1.0f) : -1;
    float lift = ease_out((t - 1.0f) / 0.7f) * 46; /* logo moves up to make room for text */
    orbit_mark_anim(640, 300 - lift, 74, grow, reveal, moon, scene);
    /* wordmark slides up and fades in */
    float wa = ease_out((t - 1.05f) / 0.6f) * scene;
    int wy = 410 + (int)((1 - wa) * 24);
    int ww = text_width("Orbit", 74);
    text_alpha(640 - ww / 2, wy, 74, 0xf3f5f8, "Orbit", ww + 6, wa, 1);
    float ta = ease_out((t - 1.45f) / 0.6f) * scene;
    const char *tag = "Chat & Sprachchat für PS3";
    int tw = text_width(tag, 22);
    text_alpha(640 - tw / 2, 510, 22, 0x9aa1ab, tag, tw + 4, ta, 0);
    /* thin progress line and version */
    float pa = clamp01((t - 0.3f) / 0.4f) * scene;
    if (pa > 0) {
        int full = 220, done = (int)(full * clamp01(t / (INTRO_LENGTH - 0.5f)));
        rrect(640 - full / 2, 600, full, 3, 1, blend(0x07080b, 0x1f232a, (unsigned)(pa * 255)));
        if (done > 2)
            rrect(640 - full / 2, 600, done, 3, 1, blend(0x07080b, C_ACCENT, (unsigned)(pa * 255)));
        char v[32];
        snprintf(v, sizeof(v), "Version %s", version);
        int vw = text_width(v, 13);
        text_alpha(640 - vw / 2, 620, 13, 0x636a74, v, vw + 4, pa, 0);
    }
    if (out > 0) {
        unsigned k = (unsigned)(out * 255);
        for (int j = 0; j < sh; j++) {
            uint32_t *row = screen + j * sw;
            for (int i = 0; i < sw; i++)
                row[i] = blend(row[i], C_BG, k);
        }
    }
    return t >= INTRO_LENGTH;
}
/* GitHub social preview (render at 1280x720, the top/bottom 40 px are cropped to 1280x640). */
void ui_social(uint32_t *pixels, int width, int height, const char *version) {
    screen = pixels;
    sw = width;
    sh = height;
    intro_background();
    stars(1.7f, 1.0f);
    orbit_mark_anim(300, 340, 118, 1.0f, 1.0f, 0.70f, 1.0f);
    bold(560, 158, 120, C_TEXT, "ORBIT", 700);
    text(566, 300, 32, 0xc9d3e6, "Text & voice chat for PS3", 680);
    text(566, 344, 22, C_MUTED, "Groups · voice chat · one invite code · end-to-end encrypted", 680);
    const char *chips[] = {"No PSN", "No account", "No router setup", "PS4 coming"};
    const uint32_t dots[] = {C_OK, C_OK, C_OK, C_WARN};
    int x = 566;
    for (int i = 0; i < 4; i++) {
        int w = text_width(chips[i], 18) + 44;
        rrect(x, 412, w, 40, 20, 0x1a2233);
        disc((float)x + 20, 432, 5, dots[i]);
        text(x + 32, 420, 18, 0xdfe6f2, chips[i], w);
        x += w + 12;
    }
    char v[48];
    snprintf(v, sizeof(v), "v%s  ·  homebrew for CFW/HEN", version);
    text(566, 490, 16, 0x636a74, v, 600);
    int bx = 566;
    for (int k = 0; k < 4; k++) /* the four face buttons as a small signature */
        face_button((float)bx + 18 + k * 44, 560, 32, k);
}
void ui_intro_done(void) {
    free(intro_bg);
    intro_bg = NULL;
    intro_w = intro_h = 0;
}
void ui_icon(uint32_t *pixels, int width, int height) {
    screen = pixels;
    sw = width;
    sh = height;
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++) {
            float t = (float)(i + j) / (float)(sw + sh);
            screen[j * sw + i] = blend(0x0d0f13, 0x16223a, (unsigned)(t * 255));
        }
    orbit_mark(360, 360, 150);
    bold(640, 250, 170, C_TEXT, "orbit", 640);
    text(650, 440, 50, C_MUTED, "Chat für PS3", 600);
}
void ui_audio_probe(uint32_t *pixels, int width, int height, const char *status,
                    const char rows[][100], int count, int player, int remaining, int level) {
    screen = pixels;
    sw = width;
    sh = height;
    fill_px(0, 0, sw, sh, C_BG);
    orbit_mark(80, 60, 18);
    bold(110, 44, 24, C_TEXT, "Orbit Audiotest", 600);
    rrect(64, 120, 730, 440, 18, C_SURF);
    if (!count)
        wrapped(90, 150, 20, C_TEXT,
                "Wähle dein Headset in den PS3-Einstellungen. Der Test liest 12 Sekunden lang "
                "echte Mikrofon-Daten.",
                670, 5, 8);
    for (int i = 0; i < count && i < 12; i++)
        text(90, 146 + i * 32, 17, C_TEXT, rows[i], 676);
    rrect(820, 120, 396, 440, 18, C_SURF);
    char value[80];
    snprintf(value, sizeof(value), "Gerät %d", player);
    bold(846, 150, 22, C_TEXT, value, 340);
    if (level >= 0) {
        if (level > 100)
            level = 100;
        text(846, 220, 14, C_DIM, "PEGEL", 338);
        rrect(846, 246, 340, 14, 7, C_SURF2);
        if (level > 1)
            rrect(846, 246, 340 * level / 100, 14, 7, C_ACCENT);
    }
    snprintf(value, sizeof(value), remaining ? "Noch %d s" : "Bereit", remaining);
    text(846, 300, 20, C_ACCENT, value, 338);
    wrapped(64, 584, 18, C_MUTED, status, 1150, 2, 6);
    footer_bar();
    int x = hint(56, 678, K_CROSS, NULL, "Testen");
    x = hint(x, 678, -3, NULL, "Gerät");
    x = hint(x, 678, K_CIRCLE, NULL, "Stoppen");
    hint(x, 678, -1, "START", "Beenden");
}
void ui_performance(int fps, int frame_us, int render_us) {
    char label[80];
    snprintf(label, sizeof(label), "%d FPS  %d.%d ms  UI %d.%d ms", fps, frame_us / 1000,
             frame_us / 100 % 10, render_us / 1000, render_us / 100 % 10);
    int w = text_width(label, 14) + 24;
    rrect(1232 - w, 678, w, 28, 14, 0x000000);
    text(1244 - w, 683, 14, fps >= 58 ? C_OK : fps >= 45 ? C_WARN : C_BAD, label, w);
}
