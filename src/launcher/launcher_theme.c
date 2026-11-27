/*
 * launcher_theme.c -- see launcher_theme.h.
 *
 * The background and the panels are written pixel by pixel into a 32-bit
 * buffer rather than drawn with the host's shapes. That is not perversity:
 * GDI has no antialiasing and no alpha on its primitives, and this look is
 * made of soft edges and washes. Arithmetic over a buffer gives both, costs
 * a few milliseconds on a window this size, and is the same on every host.
 *
 * The buffer itself, and text, which wants a font engine that knows how to
 * shape and hint, are the host's: launcher_theme_gdi.c and
 * launcher_theme_coretext.c, with launcher_theme_buf.h between them and this.
 *
 * Everything is laid out in window units and drawn in pixels, theme_scale
 * to the unit: each shape is worked out where a pixel falls in window units,
 * so a Retina display gets the same panel with four times the pixels in it
 * rather than the same pixels at four times the size. At a scale of 1 that
 * is the pixel itself, and nothing differs from drawing in pixels.
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint32_t *theme_px;
int       theme_w, theme_h;
double    theme_scale = 1.0;

/* ------------------------------------------------------------- pixels */

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void blend(int x, int y, int r, int g, int b, double a)
{
    uint32_t *p;
    int sr, sg, sb;

    if (x < 0 || y < 0 || x >= theme_w || y >= theme_h || a <= 0)
        return;
    if (a > 1) a = 1;
    p = &theme_px[(size_t)y * theme_w + x];
    sb = (int)(*p & 0xFF);
    sg = (int)((*p >> 8) & 0xFF);
    sr = (int)((*p >> 16) & 0xFF);
    sr = (int)(sr + (r - sr) * a);
    sg = (int)(sg + (g - sg) * a);
    sb = (int)(sb + (b - sb) * a);
    *p = ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
}

/* What of the background does not move: each pixel's distance from the
 * glow's centre, and how much vignette is over it. Worked out once per
 * buffer size, since the glow breathing changes neither -- two square roots
 * a pixel every frame is most of a frame on a Retina display. */
static float *g_bg_dist, *g_bg_vignette;
static int    g_bg_w, g_bg_h;

static int background_cache(void)
{
    const double cx = theme_w * 0.5, cy = theme_h * 0.52;
    size_t n = (size_t)theme_w * theme_h;
    int x, y;

    if (g_bg_dist && g_bg_w == theme_w && g_bg_h == theme_h)
        return 1;
    free(g_bg_dist);
    free(g_bg_vignette);
    g_bg_dist = malloc(n * sizeof *g_bg_dist);
    g_bg_vignette = malloc(n * sizeof *g_bg_vignette);
    if (!g_bg_dist || !g_bg_vignette) {
        free(g_bg_dist);
        free(g_bg_vignette);
        g_bg_dist = g_bg_vignette = NULL;
        return 0;
    }
    for (y = 0; y < theme_h; y++)
        for (x = 0; x < theme_w; x++) {
            double dx = x - cx, dy = (y - cy) * 1.25;
            double nx = (x - cx) / (theme_w * 0.5), ny = (y - cy) / (theme_h * 0.5);
            size_t i = (size_t)y * theme_w + x;

            g_bg_dist[i] = (float)sqrt(dx * dx + dy * dy);
            g_bg_vignette[i] =
                (float)(clamp01((sqrt(nx * nx + ny * ny) - 0.75) / 0.75) * 0.85);
        }
    g_bg_w = theme_w;
    g_bg_h = theme_h;
    return 1;
}

/* ------------------------------------------------------------ pictures */

static const uint32_t *g_bd;          /* the backdrop as given */
static int             g_bd_w, g_bd_h;
static uint32_t       *g_bd_fit;      /* it, cropped and darkened, at the buffer's size */
static int             g_bd_fit_w, g_bd_fit_h;
static const uint32_t *g_bd_fit_of;

void theme_backdrop(const uint32_t *argb, int w, int h)
{
    g_bd = argb && w > 0 && h > 0 ? argb : NULL;
    g_bd_w = w;
    g_bd_h = h;
}

/* One pass of a box blur, radius r, along rows (dx 1) or columns: three
 * passes are close to a Gaussian. In place, through a row's copy. */
static void box_pass(float *c, int w, int h, int r, int along_rows)
{
    int len = along_rows ? w : h, lines = along_rows ? h : w, line, i, ch;
    float *tmp = malloc((size_t)len * 3 * sizeof *tmp);

    if (!tmp || r < 1) {
        free(tmp);
        return;
    }
    for (line = 0; line < lines; line++) {
        for (i = 0; i < len; i++) {
            size_t at = along_rows ? (size_t)line * w + i : (size_t)i * w + line;

            for (ch = 0; ch < 3; ch++)
                tmp[i * 3 + ch] = c[at * 3 + ch];
        }
        for (ch = 0; ch < 3; ch++) {
            double sum = 0;
            int lo, hi;

            for (i = -r; i <= r; i++)
                sum += tmp[(i < 0 ? 0 : i >= len ? len - 1 : i) * 3 + ch];
            for (i = 0; i < len; i++) {
                size_t at = along_rows ? (size_t)line * w + i : (size_t)i * w + line;

                c[at * 3 + ch] = (float)(sum / (2 * r + 1));
                lo = i - r < 0 ? 0 : i - r;
                hi = i + r + 1 >= len ? len - 1 : i + r + 1;
                sum += tmp[hi * 3 + ch] - tmp[lo * 3 + ch];
            }
        }
    }
    free(tmp);
}

/* The backdrop scaled to cover the buffer (cropped to its shape, a little
 * above centre: key art keeps its faces high), averaged over each target
 * pixel's footprint, softened, then darkened -- lightly behind the logo at
 * the top, heavily behind the rows and buttons -- and drawn toward the
 * title's own background colour as it darkens, so text and panels over it
 * read. Worked out once per size and picture. */
static int backdrop_draw(void)
{
    size_t n = (size_t)theme_w * theme_h;
    double k, ox, oy;
    float *c;
    int x, y, pass, radius;

    if (!g_bd)
        return 0;
    if (!(g_bd_fit && g_bd_fit_w == theme_w && g_bd_fit_h == theme_h && g_bd_fit_of == g_bd)) {
        free(g_bd_fit);
        g_bd_fit = malloc(n * sizeof *g_bd_fit);
        c = malloc(n * 3 * sizeof *c);
        if (!g_bd_fit || !c) {
            free(g_bd_fit);
            free(c);
            g_bd_fit = NULL;
            return 0;
        }
        k = (double)g_bd_w / theme_w > (double)g_bd_h / theme_h
            ? (double)g_bd_h / theme_h : (double)g_bd_w / theme_w;   /* source px per pixel */
        ox = (g_bd_w - theme_w * k) * 0.5;
        oy = (g_bd_h - theme_h * k) * 0.35;
        for (y = 0; y < theme_h; y++) {
            int sy0 = (int)(oy + y * k), sy1 = (int)(oy + (y + 1) * k);

            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > g_bd_h) sy1 = g_bd_h;
            for (x = 0; x < theme_w; x++) {
                int sx0 = (int)(ox + x * k), sx1 = (int)(ox + (x + 1) * k), sx, sy;
                double r = 0, g = 0, b = 0, cnt = 0;

                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > g_bd_w) sx1 = g_bd_w;
                for (sy = sy0; sy < sy1; sy++)
                    for (sx = sx0; sx < sx1; sx++) {
                        uint32_t p = g_bd[(size_t)sy * g_bd_w + sx];

                        r += (p >> 16) & 0xFF;
                        g += (p >> 8) & 0xFF;
                        b += p & 0xFF;
                        cnt++;
                    }
                if (cnt < 1) cnt = 1;
                c[((size_t)y * theme_w + x) * 3 + 0] = (float)(r / cnt);
                c[((size_t)y * theme_w + x) * 3 + 1] = (float)(g / cnt);
                c[((size_t)y * theme_w + x) * 3 + 2] = (float)(b / cnt);
            }
        }
        /* Soft, so it is a mood behind the settings and not a second thing
         * to read: about four window units. */
        radius = (int)(4 * theme_scale + 0.5);
        for (pass = 0; pass < 3; pass++) {
            box_pass(c, theme_w, theme_h, radius, 1);
            box_pass(c, theme_w, theme_h, radius, 0);
        }
        for (y = 0; y < theme_h; y++) {
            double fy = (double)y / theme_h;
            /* 0.50 behind the logo, 0.24 by the tabs, 0.12 at the bottom. */
            double shade = fy < 0.10 ? 0.50
                         : fy < 0.20 ? 0.50 - 0.26 * (fy - 0.10) / 0.10
                         : 0.24 - 0.12 * (fy - 0.20) / 0.80;

            for (x = 0; x < theme_w; x++) {
                double nx = (x - theme_w * 0.5) / (theme_w * 0.5);
                double v = shade * (1.0 - 0.30 * nx * nx);
                const float *q = &c[((size_t)y * theme_w + x) * 3];
                double r = q[0] * v + ((THEME_BG_RGB >> 16) & 0xFF) * (1 - v);
                double g = q[1] * v + ((THEME_BG_RGB >> 8) & 0xFF) * (1 - v);
                double b = q[2] * v + (THEME_BG_RGB & 0xFF) * (1 - v);

                g_bd_fit[(size_t)y * theme_w + x] =
                    ((uint32_t)(r > 255 ? 255 : r) << 16) | ((uint32_t)(g > 255 ? 255 : g) << 8) |
                    (uint32_t)(b > 255 ? 255 : b);
            }
        }
        free(c);
        g_bd_fit_w = theme_w;
        g_bd_fit_h = theme_h;
        g_bd_fit_of = g_bd;
    }
    memcpy(theme_px, g_bd_fit, n * sizeof *theme_px);
    return 1;
}

/* ----------------------------------------------------- showcase pieces */

static void px_rect(ThemeRect r, int *x0, int *y0, int *x1, int *y1)
{
    const double s = theme_scale;

    *x0 = (int)floor(r.x * s);
    *y0 = (int)floor(r.y * s);
    *x1 = (int)ceil((r.x + r.w) * s);
    *y1 = (int)ceil((r.y + r.h) * s);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > theme_w) *x1 = theme_w;
    if (*y1 > theme_h) *y1 = theme_h;
}

#define RGB_R(v) (int)(((v) >> 16) & 0xFF)
#define RGB_G(v) (int)(((v) >> 8) & 0xFF)
#define RGB_B(v) (int)((v) & 0xFF)

/* A run of n pixels washed toward rgb by a/256, two channels at a time: the
 * red and blue in one word's lanes, the green in another, each lane wide
 * enough for a byte times 256. A wash over the whole window is a few
 * milliseconds this way, where blend()'s doubles were most of a frame. */
static void span_wash(uint32_t *d, int n, uint32_t rgb, int a)
{
    const uint32_t crb = (rgb & 0xFF00FFu) * (uint32_t)a, cg = (rgb & 0x00FF00u) * (uint32_t)a;
    const uint32_t k = 256u - (uint32_t)a;
    int i;

    if (a <= 0)
        return;
    if (a >= 256) {
        for (i = 0; i < n; i++)
            d[i] = rgb & 0xFFFFFFu;
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t v = d[i];

        d[i] = (((crb + (v & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) |
               (((cg + (v & 0x00FF00u) * k) >> 8) & 0x00FF00u);
    }
}

static int alpha256(double a)
{
    return a <= 0 ? 0 : a >= 1 ? 256 : (int)(a * 256.0 + 0.5);
}

void theme_fill(ThemeRect r, uint32_t rgb, double alpha)
{
    int x0, y0, x1, y1, y, a = alpha256(alpha);

    if (!theme_px || a <= 0)
        return;
    px_rect(r, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1 && x1 > x0; y++)
        span_wash(&theme_px[(size_t)y * theme_w + x0], x1 - x0, rgb, a);
}

void theme_vgrad(ThemeRect r, int n, const double *at, const uint32_t *rgb, const double *alpha)
{
    int x0, y0, x1, y1, y, k;

    if (!theme_px || n < 1)
        return;
    px_rect(r, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        double t = r.h > 0 ? ((y + 0.5) / theme_scale - r.y) / r.h : 0, u, a;
        int cr, cg, cb;

        for (k = 0; k + 1 < n && t > at[k + 1]; k++)
            ;
        if (k + 1 >= n || t <= at[0]) {
            k = t <= at[0] ? 0 : n - 1;
            cr = RGB_R(rgb[k]); cg = RGB_G(rgb[k]); cb = RGB_B(rgb[k]); a = alpha[k];
        } else {
            u = (t - at[k]) / (at[k + 1] - at[k] > 1e-9 ? at[k + 1] - at[k] : 1e-9);
            cr = (int)(RGB_R(rgb[k]) + (RGB_R(rgb[k + 1]) - RGB_R(rgb[k])) * u);
            cg = (int)(RGB_G(rgb[k]) + (RGB_G(rgb[k + 1]) - RGB_G(rgb[k])) * u);
            cb = (int)(RGB_B(rgb[k]) + (RGB_B(rgb[k + 1]) - RGB_B(rgb[k])) * u);
            a = alpha[k] + (alpha[k + 1] - alpha[k]) * u;
        }
        if (a <= 0 || x1 <= x0)
            continue;
        span_wash(&theme_px[(size_t)y * theme_w + x0], x1 - x0,
                  ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb, alpha256(a));
    }
}

void theme_outline(ThemeRect r, uint32_t rgb, double alpha, int width)
{
    ThemeRect e;

    e = r; e.h = width; theme_fill(e, rgb, alpha);
    e = r; e.y = r.y + r.h - width; e.h = width; theme_fill(e, rgb, alpha);
    e = r; e.y += width; e.h -= 2 * width; e.w = width; theme_fill(e, rgb, alpha);
    e.x = r.x + r.w - width; theme_fill(e, rgb, alpha);
}

void theme_brackets(ThemeRect r, int len, int width, uint32_t rgb, double alpha)
{
    ThemeRect e;
    int x1 = r.x + r.w, y1 = r.y + r.h;

    e.w = len; e.h = width;
    e.x = r.x; e.y = r.y; theme_fill(e, rgb, alpha);
    e.x = x1 - len; theme_fill(e, rgb, alpha);
    e.y = y1 - width; theme_fill(e, rgb, alpha);
    e.x = r.x; theme_fill(e, rgb, alpha);
    e.w = width; e.h = len - width;
    e.x = r.x; e.y = r.y + width; theme_fill(e, rgb, alpha);
    e.x = x1 - width; theme_fill(e, rgb, alpha);
    e.y = y1 - len; theme_fill(e, rgb, alpha);
    e.x = r.x; theme_fill(e, rgb, alpha);
}

void theme_glow_rect(ThemeRect r, uint32_t rgb, double alpha, double radius)
{
    const double s = theme_scale;
    ThemeRect big = r;
    int x0, y0, x1, y1, x, y, pad = (int)ceil(radius * 2.5);

    if (!theme_px || alpha <= 0 || radius <= 0)
        return;
    big.x -= pad; big.y -= pad; big.w += 2 * pad; big.h += 2 * pad;
    px_rect(big, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        double py = (y + 0.5) / s, dy = py < r.y ? r.y - py : py > r.y + r.h ? py - (r.y + r.h) : 0;

        for (x = x0; x < x1; x++) {
            double px = (x + 0.5) / s, dx = px < r.x ? r.x - px : px > r.x + r.w ? px - (r.x + r.w) : 0;
            double d2 = dx * dx + dy * dy;

            if (d2 <= 0)
                continue;                       /* the inside is the caller's */
            blend(x, y, RGB_R(rgb), RGB_G(rgb), RGB_B(rgb), alpha * exp(-d2 / (radius * radius * 0.5)));
        }
    }
}

static float *g_vig;
static int    g_vig_w, g_vig_h;
static double g_vig_key[3];

void theme_vignette(double strength, double cx, double cy)
{
    size_t i, n = (size_t)theme_w * theme_h;
    int x, y;

    if (!theme_px || strength <= 0)
        return;
    if (!g_vig || g_vig_w != theme_w || g_vig_h != theme_h || g_vig_key[0] != strength ||
        g_vig_key[1] != cx || g_vig_key[2] != cy) {
        free(g_vig);
        g_vig = malloc(n * sizeof *g_vig);
        if (!g_vig)
            return;
        for (y = 0; y < theme_h; y++)
            for (x = 0; x < theme_w; x++) {
                double nx = (double)x / theme_w - cx, ny = (double)y / theme_h - cy;
                double d = sqrt(nx * nx + ny * ny) / 0.8;

                g_vig[(size_t)y * theme_w + x] = (float)(strength * (d > 1 ? 1 : d * d));
            }
        g_vig_w = theme_w; g_vig_h = theme_h;
        g_vig_key[0] = strength; g_vig_key[1] = cx; g_vig_key[2] = cy;
    }
    for (i = 0; i < n; i++) {
        uint32_t p = theme_px[i];
        double k = 1.0 - g_vig[i];

        theme_px[i] = ((uint32_t)(((p >> 16) & 0xFF) * k) << 16) |
                      ((uint32_t)(((p >> 8) & 0xFF) * k) << 8) | (uint32_t)((p & 0xFF) * k);
    }
}

/* Scaled copies kept between frames: a picture re-filtered from 4x every
 * 16 ms would crawl. Premultiplied, so drawing one is a blend a pixel. */
typedef struct {
    const uint32_t *src;
    int sw, sh, pw, ph;
    double key[6];
    uint32_t *px;           /* 0xAARRGGBB, premultiplied */
} Kept;
static Kept g_kept[8];

static uint32_t *scale_box(const uint32_t *src, int sw, int sh, double sx0, double sy0,
                           double sx1, double sy1, int pw, int ph)
{
    uint32_t *out = malloc((size_t)pw * ph * sizeof *out);
    double kx = (sx1 - sx0) / pw, ky = (sy1 - sy0) / ph;
    int x, y;

    if (!out)
        return NULL;
    for (y = 0; y < ph; y++) {
        int y0 = (int)(sy0 + y * ky), y1 = (int)(sy0 + (y + 1) * ky);

        if (y1 <= y0) y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > sh) y1 = sh;
        for (x = 0; x < pw; x++) {
            int x0 = (int)(sx0 + x * kx), x1 = (int)(sx0 + (x + 1) * kx), xx, yy;
            double r = 0, g = 0, b = 0, a = 0, c = 0;

            if (x1 <= x0) x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > sw) x1 = sw;
            for (yy = y0; yy < y1; yy++)
                for (xx = x0; xx < x1; xx++) {
                    uint32_t p = src[(size_t)yy * sw + xx];
                    double pa = ((p >> 24) & 0xFF) / 255.0;

                    r += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                    a += pa;
                    c++;
                }
            if (c < 1) c = 1;
            out[(size_t)y * pw + x] = ((uint32_t)(a / c * 255.0 + 0.5) << 24) |
                                      ((uint32_t)(r / c + 0.5) << 16) |
                                      ((uint32_t)(g / c + 0.5) << 8) | (uint32_t)(b / c + 0.5);
        }
    }
    return out;
}

static void draw_kept(const uint32_t *px, int pw, int ph, int ox, int oy, double alpha)
{
    int x, y;

    for (y = 0; y < ph; y++) {
        int ty = oy + y;

        if (ty < 0 || ty >= theme_h)
            continue;
        for (x = 0; x < pw; x++) {
            int tx = ox + x;
            uint32_t p = px[(size_t)y * pw + x], *d;
            double a = ((p >> 24) & 0xFF) / 255.0 * alpha, k = 1 - a;

            if (tx < 0 || tx >= theme_w || a <= 0)
                continue;
            d = &theme_px[(size_t)ty * theme_w + tx];
            *d = ((uint32_t)(((*d >> 16) & 0xFF) * k + ((p >> 16) & 0xFF) * alpha) << 16) |
                 ((uint32_t)(((*d >> 8) & 0xFF) * k + ((p >> 8) & 0xFF) * alpha) << 8) |
                 (uint32_t)((*d & 0xFF) * k + (p & 0xFF) * alpha);
        }
    }
}

static Kept *kept_for(int slot, const uint32_t *src, int sw, int sh, const double key[6], int pw, int ph)
{
    Kept *k;

    if (slot < 0 || slot >= 8)
        return NULL;
    k = &g_kept[slot];
    if (k->px && k->src == src && k->sw == sw && k->sh == sh && k->pw == pw && k->ph == ph &&
        !memcmp(k->key, key, sizeof k->key))
        return k;
    free(k->px);
    memset(k, 0, sizeof *k);
    k->src = src; k->sw = sw; k->sh = sh; k->pw = pw; k->ph = ph;
    memcpy(k->key, key, sizeof k->key);
    return k;
}

void theme_cover(ThemeRect r, const uint32_t *argb, int w, int h, double keep,
                 double fx, double fy, double alpha, int slot)
{
    const double s = theme_scale;
    int pw = (int)ceil(r.w * s), ph = (int)ceil(r.h * s);
    double ih = h * (keep > 0 && keep <= 1 ? keep : 1), sc, cw, ch, sx0, sy0;
    double key[6] = { keep, fx, fy, r.w, r.h, s };
    Kept *k;
    uint32_t *px;

    if (!theme_px || !argb || w <= 0 || h <= 0 || pw <= 0 || ph <= 0)
        return;
    sc = (double)pw / w > (double)ph / ih ? (double)pw / w : (double)ph / ih;
    cw = pw / sc; ch = ph / sc;
    sx0 = (w - cw) * fx;
    sy0 = (ih - ch) * fy;
    k = kept_for(slot, argb, w, h, key, pw, ph);
    if (k && k->px) {
        draw_kept(k->px, pw, ph, (int)floor(r.x * s), (int)floor(r.y * s), alpha);
        return;
    }
    px = scale_box(argb, w, h, sx0, sy0, sx0 + cw, sy0 + ch, pw, ph);
    if (!px)
        return;
    draw_kept(px, pw, ph, (int)floor(r.x * s), (int)floor(r.y * s), alpha);
    if (k) k->px = px; else free(px);
}

void theme_picture_ex(ThemeRect r, const uint32_t *argb, int w, int h, ThemeAlign align,
                      double alpha, int slot, ThemeRect *drawn)
{
    const double s = theme_scale;
    double fit, dw, dh, x0, y0;
    double key[6] = { (double)align, r.w, r.h, s, 0, 0 };
    int pw, ph;
    Kept *k;
    uint32_t *px;

    if (!theme_px || !argb || w <= 0 || h <= 0 || r.w <= 0 || r.h <= 0)
        return;
    fit = (double)r.w / w < (double)r.h / h ? (double)r.w / w : (double)r.h / h;
    dw = w * fit; dh = h * fit;
    x0 = align == THEME_LEFT ? r.x : align == THEME_RIGHT ? r.x + r.w - dw : r.x + (r.w - dw) * 0.5;
    y0 = r.y + (r.h - dh) * 0.5;
    if (drawn) {
        drawn->x = (int)x0; drawn->y = (int)y0; drawn->w = (int)dw; drawn->h = (int)dh;
    }
    pw = (int)(dw * s + 0.5); ph = (int)(dh * s + 0.5);
    if (pw <= 0 || ph <= 0)
        return;
    k = kept_for(slot, argb, w, h, key, pw, ph);
    if (k && k->px) {
        draw_kept(k->px, pw, ph, (int)(x0 * s), (int)(y0 * s), alpha);
        return;
    }
    px = scale_box(argb, w, h, 0, 0, w, h, pw, ph);
    if (!px)
        return;
    draw_kept(px, pw, ph, (int)(x0 * s), (int)(y0 * s), alpha);
    if (k) k->px = px; else free(px);
}

void theme_line(double x0, double y0, double x1, double y1, double width, uint32_t rgb, double alpha)
{
    const double s = theme_scale;
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, hw = width * 0.5;
    int px0 = (int)floor(((x0 < x1 ? x0 : x1) - width) * s), px1 = (int)ceil(((x0 > x1 ? x0 : x1) + width) * s);
    int py0 = (int)floor(((y0 < y1 ? y0 : y1) - width) * s), py1 = (int)ceil(((y0 > y1 ? y0 : y1) + width) * s);
    int x, y;

    if (!theme_px || alpha <= 0)
        return;
    for (y = py0 < 0 ? 0 : py0; y < py1 && y < theme_h; y++)
        for (x = px0 < 0 ? 0 : px0; x < px1 && x < theme_w; x++) {
            double px = (x + 0.5) / s, py = (y + 0.5) / s, t, ex, ey, d;

            t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            ex = px - (x0 + t * dx); ey = py - (y0 + t * dy);
            d = sqrt(ex * ex + ey * ey) - hw;           /* < 0 inside */
            if (d < 1.0 / s)
                blend(x, y, RGB_R(rgb), RGB_G(rgb), RGB_B(rgb), alpha * clamp01(0.5 - d * s));
        }
}

void theme_chevron(double cx, double cy, int right, double w, double h, uint32_t rgb, double alpha)
{
    double tip = right ? cx + w / 2 : cx - w / 2, back = right ? cx - w / 2 : cx + w / 2;

    theme_line(back, cy - h / 2, tip, cy, 2, rgb, alpha);
    theme_line(tip, cy, back, cy + h / 2, 2, rgb, alpha);
}

/* A softened copy of a picture's alpha, tinted: kept per slot as the others. */
void theme_picture_glow(ThemeRect r, const uint32_t *argb, int w, int h, ThemeAlign align,
                        uint32_t rgb, double alpha, int blur, int slot)
{
    const double s = theme_scale;
    double fit, dw, dh, x0, y0;
    double key[6] = { (double)align, r.w, r.h, s, (double)blur, (double)rgb };
    int pw, ph, pad = (int)ceil(blur * s * 2), gw, gh, x, y, pass;
    Kept *k;
    uint32_t *sc, *px;
    float *m, *t;

    if (!theme_px || !argb || w <= 0 || h <= 0 || r.w <= 0 || r.h <= 0)
        return;
    fit = (double)r.w / w < (double)r.h / h ? (double)r.w / w : (double)r.h / h;
    dw = w * fit; dh = h * fit;
    x0 = align == THEME_LEFT ? r.x : align == THEME_RIGHT ? r.x + r.w - dw : r.x + (r.w - dw) * 0.5;
    y0 = r.y + (r.h - dh) * 0.5;
    pw = (int)(dw * s + 0.5); ph = (int)(dh * s + 0.5);
    gw = pw + 2 * pad; gh = ph + 2 * pad;
    if (pw <= 0 || ph <= 0)
        return;
    k = kept_for(slot, argb, w, h, key, gw, gh);
    if (k && k->px) {
        draw_kept(k->px, gw, gh, (int)(x0 * s) - pad, (int)(y0 * s) - pad, alpha);
        return;
    }
    sc = scale_box(argb, w, h, 0, 0, w, h, pw, ph);
    m = calloc((size_t)gw * gh, sizeof *m);
    t = calloc((size_t)(gw > gh ? gw : gh), sizeof *t);
    px = malloc((size_t)gw * gh * sizeof *px);
    if (!sc || !m || !t || !px) {
        free(sc); free(m); free(t); free(px);
        return;
    }
    for (y = 0; y < ph; y++)
        for (x = 0; x < pw; x++)
            m[(size_t)(y + pad) * gw + x + pad] = ((sc[(size_t)y * pw + x] >> 24) & 0xFF) / 255.0f;
    for (pass = 0; pass < 3; pass++) {                /* three box passes: close to a Gaussian */
        int rad = (int)(blur * s / 2) > 0 ? (int)(blur * s / 2) : 1, i;

        for (y = 0; y < gh; y++) {                    /* rows */
            double sum = 0;
            for (x = 0; x < gw; x++) t[x] = m[(size_t)y * gw + x];
            for (i = -rad; i <= rad; i++) sum += t[i < 0 ? 0 : i >= gw ? gw - 1 : i];
            for (x = 0; x < gw; x++) {
                m[(size_t)y * gw + x] = (float)(sum / (2 * rad + 1));
                sum += t[x + rad + 1 >= gw ? gw - 1 : x + rad + 1] - t[x - rad < 0 ? 0 : x - rad];
            }
        }
        for (x = 0; x < gw; x++) {                    /* columns */
            double sum = 0;
            for (y = 0; y < gh; y++) t[y] = m[(size_t)y * gw + x];
            for (i = -rad; i <= rad; i++) sum += t[i < 0 ? 0 : i >= gh ? gh - 1 : i];
            for (y = 0; y < gh; y++) {
                m[(size_t)y * gw + x] = (float)(sum / (2 * rad + 1));
                sum += t[y + rad + 1 >= gh ? gh - 1 : y + rad + 1] - t[y - rad < 0 ? 0 : y - rad];
            }
        }
    }
    for (y = 0; y < gh; y++)
        for (x = 0; x < gw; x++) {
            double a = m[(size_t)y * gw + x];

            px[(size_t)y * gw + x] = ((uint32_t)(a * 255.0) << 24) |
                                     ((uint32_t)(RGB_R(rgb) * a) << 16) |
                                     ((uint32_t)(RGB_G(rgb) * a) << 8) | (uint32_t)(RGB_B(rgb) * a);
        }
    free(sc); free(m); free(t);
    draw_kept(px, gw, gh, (int)(x0 * s) - pad, (int)(y0 * s) - pad, alpha);
    if (k) k->px = px; else free(px);
}

/* A picture fitted into r, sampled with a box filter, its alpha honoured. */
void theme_picture(ThemeRect r, const uint32_t *argb, int w, int h, ThemeAlign align)
{
    const double s = theme_scale;
    double fit, dw, dh, x0, y0, k;
    int x, y, px0, py0, px1, py1;

    if (!theme_px || !argb || w <= 0 || h <= 0 || r.w <= 0 || r.h <= 0)
        return;
    fit = (double)r.w / w < (double)r.h / h ? (double)r.w / w : (double)r.h / h;
    dw = w * fit * s;
    dh = h * fit * s;
    x0 = align == THEME_LEFT ? r.x * s : align == THEME_RIGHT ? (r.x + r.w) * s - dw
                                                              : (r.x + r.w * 0.5) * s - dw * 0.5;
    y0 = (r.y + r.h * 0.5) * s - dh * 0.5;
    k = w / dw;                                    /* source px per pixel */
    px0 = (int)x0; py0 = (int)y0; px1 = (int)(x0 + dw); py1 = (int)(y0 + dh);
    for (y = py0; y < py1; y++) {
        int sy0 = (int)((y - y0) * k), sy1 = (int)((y + 1 - y0) * k);

        if (sy0 < 0) sy0 = 0;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (x = px0; x < px1; x++) {
            int sx0 = (int)((x - x0) * k), sx1 = (int)((x + 1 - x0) * k), sx, sy;
            double r_ = 0, g = 0, b = 0, a = 0, cnt = 0;

            if (sx0 < 0) sx0 = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            for (sy = sy0; sy < sy1; sy++)
                for (sx = sx0; sx < sx1; sx++) {
                    uint32_t p = argb[(size_t)sy * w + sx];
                    double pa = ((p >> 24) & 0xFF) / 255.0;

                    r_ += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                    a += pa;
                    cnt++;
                }
            if (a <= 0 || cnt < 1)
                continue;
            blend(x, y, (int)(r_ / a), (int)(g / a), (int)(b / a), a / cnt);
        }
    }
}

void theme_background(double pulse)
{
    /* Two glows behind the content, the larger one breathing. The
     * dashboard's light came from behind and below; this is that, not a
     * copy of it. */
    const double big = theme_w * (0.62 + 0.03 * sin(pulse * 6.2831853));
    const double small = theme_w * 0.26;
    size_t i, n;

    if (!theme_px || !background_cache())
        return;
    if (backdrop_draw())
        return;

    n = (size_t)theme_w * theme_h;
    for (i = 0; i < n; i++) {
        double d = g_bg_dist[i];
        double a = clamp01(1.0 - d / big);
        double b = clamp01(1.0 - d / small);
        double v = a * a * 0.22 + b * b * 0.28;
        double k = g_bg_vignette[i];
        int r = (int)((((THEME_BG_RGB) >> 16) & 0xFF) + v * (((THEME_GLOW_RGB) >> 16) & 0xFF));
        int g = (int)((((THEME_BG_RGB) >> 8) & 0xFF) + v * (((THEME_GLOW_RGB) >> 8) & 0xFF));
        int bl = (int)(((THEME_BG_RGB) & 0xFF) + v * ((THEME_GLOW_RGB) & 0xFF));

        /* A vignette, so the edges fall away and the eye goes to the
         * middle: black over it, as blend() would put it. */
        if (k > 0) {
            r = (int)(r - r * k);
            g = (int)(g - g * k);
            bl = (int)(bl - bl * k);
        }
        theme_px[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
    }
}

/* Distance from a point to a rounded rectangle: negative inside,
 * positive outside, and the magnitude is how far. Soft edges and the
 * glow around them both fall out of it. */
static double rrect_distance(double px, double py, ThemeRect r, double radius)
{
    double hx = r.w * 0.5, hy = r.h * 0.5;
    double cx = r.x + hx, cy = r.y + hy;
    double dx = fabs(px - cx) - (hx - radius);
    double dy = fabs(py - cy) - (hy - radius);
    double ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    double inside = (dx > dy ? dx : dy);

    if (inside > 0)
        inside = 0;
    return sqrt(ox * ox + oy * oy) + inside - radius;
}

void theme_panel(ThemeRect r, double lit)
{
#ifdef THEME_FLAT_PANELS
    /* The showcase's: a flat dark slab, a hairline edge that lights, and the
     * lit one tinted with a bar down its left side. */
    if (theme_px) {
        ThemeRect bar = r;

        theme_fill(r, THEME_SLAB_RGB, 0.86 + 0.10 * lit);
        if (lit > 0.01)
            theme_fill(r, THEME_ACCENT_RGB, 0.10 * lit);
        theme_outline(r, lit > 0.5 ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB,
                      0.55 + 0.45 * lit, 1);
        if (lit > 0.5) {
            bar.w = 3;
            theme_fill(bar, THEME_ACCENT_RGB, lit);
        }
    }
    return;
#endif
    const double radius = 10.0, s = theme_scale;
    double falloff, strength, reach;
    int pad, x0, y0, x1, y1, x, y;

    if (!theme_px)
        return;
    lit = clamp01(lit);
    falloff = 4.0 + 7.0 * lit;
    strength = 0.07 + 0.42 * lit;

    /* Room for the glow to reach nothing. Too little and it is cut off
     * square, which reads as a box drawn round the selected row rather
     * than light coming off it -- the falloff below has to have faded
     * to nothing by the time it reaches this edge. Nothing is where the
     * glow drops under 1/255, which blend() rounds to no change at all:
     * 52 for a panel fully lit, and far less for one at rest, which is
     * most of them. */
    reach = falloff * log(strength * 255.0) + 2.0;
    pad = reach < 52.0 ? (int)ceil(reach) : 52;

    x0 = (int)floor((r.x - pad) * s);
    y0 = (int)floor((r.y - pad) * s);
    x1 = (int)ceil((r.x + r.w + pad) * s);
    y1 = (int)ceil((r.y + r.h + pad) * s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > theme_w) x1 = theme_w;
    if (y1 > theme_h) y1 = theme_h;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            /* In window units, from the pixel's centre. */
            double d = rrect_distance((x + 0.5) / s, (y + 0.5) / s, r, radius);

            if (d < 0) {
                /* Inside: a dark slab that greens as it lights up. */
                double edge = clamp01(-d * s);        /* antialias the rim */
                double fill = 0.55 + 0.20 * lit;

                blend(x, y, (int)((((THEME_SLAB_RGB) >> 16) & 0xFF) + (((THEME_SLAB_LIT_RGB) >> 16) & 0xFF) * lit),
                            (int)((((THEME_SLAB_RGB) >> 8) & 0xFF) + (((THEME_SLAB_LIT_RGB) >> 8) & 0xFF) * lit),
                            (int)(((THEME_SLAB_RGB) & 0xFF) + ((THEME_SLAB_LIT_RGB) & 0xFF) * lit), fill * edge);
            } else {
                /* Outside: the glow, which only exists when lit. */
                double g = exp(-d / falloff) * strength;

                blend(x, y, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), g);
            }
            /* The rim itself, brightest right on the boundary. */
            if (d > -2.0 && d < 1.0) {
                double rim = 1.0 - fabs(d + 0.5) / 1.5;

                if (rim > 0)
                    blend(x, y, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), rim * (0.25 + 0.6 * lit));
            }
        }
    }
}

void theme_bar(ThemeRect r, double fraction, double phase)
{
    const double s = theme_scale, radius = r.h / 2.0;
    double fill_to = r.x + r.w * clamp01(fraction);
    /* The highlight: a soft band a fifth of the bar wide, entering at the
     * left edge and leaving past the right one. */
    const double band = r.w * 0.2;
    double centre = r.x - band + (r.w + 2 * band) * (phase < 0 ? 0 : phase - floor(phase));
    int x0, y0, x1, y1, x, y;

    if (!theme_px)
        return;
    x0 = (int)floor(r.x * s);
    y0 = (int)floor(r.y * s);
    x1 = (int)ceil((r.x + r.w) * s);
    y1 = (int)ceil((r.y + r.h) * s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > theme_w) x1 = theme_w;
    if (y1 > theme_h) y1 = theme_h;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            double px = (x + 0.5) / s, py = (y + 0.5) / s;
            double d = rrect_distance(px, py, r, radius);
            double edge = clamp01(-d * s);

            if (edge <= 0)
                continue;
            double u = (px - centre) / band;
            double glow = phase < 0 ? 0 : exp(-u * u * 4.0);

            if (px < fill_to) {
                /* Brighter along the top, as the panels' rims are. */
                double t = (py - r.y) / (double)r.h;

                blend(x, y, (int)((((THEME_ACCENT_RGB) >> 16) & 0xFF) - (((THEME_BAR_DARKEN_RGB) >> 16) & 0xFF) * t),
                            (int)((((THEME_ACCENT_RGB) >> 8) & 0xFF) - (((THEME_BAR_DARKEN_RGB) >> 8) & 0xFF) * t),
                            (int)(((THEME_ACCENT_RGB) & 0xFF) - ((THEME_BAR_DARKEN_RGB) & 0xFF) * t), edge);
                blend(x, y, (((THEME_HILITE_RGB) >> 16) & 0xFF), (((THEME_HILITE_RGB) >> 8) & 0xFF), ((THEME_HILITE_RGB) & 0xFF),
                      0.45 * glow * edge);
            } else {
                blend(x, y, (((THEME_TRACK_RGB) >> 16) & 0xFF), (((THEME_TRACK_RGB) >> 8) & 0xFF), ((THEME_TRACK_RGB) & 0xFF), 0.85 * edge);
                blend(x, y, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), 0.12 * glow * edge);
            }
        }
    }
}

/* One window-unit row of the underline, `row` below its top: every pixel
 * row in it, faded at the ends. */
static void underline_row(int x0, int x1, int y, int row, double alpha)
{
    const double s = theme_scale;
    int py0 = (int)floor((y + row) * s), py1 = (int)floor((y + row + 1) * s);
    int px0 = (int)floor(x0 * s), px1 = (int)floor(x1 * s);
    int px, py;

    for (px = px0; px < px1; px++) {
        double u = px / s;                      /* window units */
        double edge = 1.0, run = u - x0, left = x1 - u;

        if (run < 10) edge = run / 10.0;        /* fade the ends */
        if (left < 10) edge = left / 10.0;
        for (py = py0; py < py1; py++)
            blend(px, py, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), alpha * edge);
    }
}

void theme_tab_underline(ThemeRect from, ThemeRect to, double t)
{
    double e = clamp01(t);
    int x0, x1;

    /* Ease, so it settles rather than stopping dead. */
    e = e * e * (3.0 - 2.0 * e);
    x0 = (int)(from.x + (to.x - from.x) * e);
    x1 = x0 + (int)(from.w + (to.w - from.w) * e);

    /* Drawn rather than panelled: a panel brings a panel's glow, and a
     * three pixel bar wearing a twenty-six pixel halo sits under the
     * whole strip instead of under one tab. */
    underline_row(x0, x1, from.y,  0, 0.95);
    underline_row(x0, x1, from.y,  1, 0.95);
    underline_row(x0, x1, from.y,  2, 0.45);
    underline_row(x0, x1, from.y,  3, 0.18);
    underline_row(x0, x1, from.y, -1, 0.25);
}

void theme_arrows(ThemeRect r, int can_left, int can_right, double lit)
{
    const double s = theme_scale;
    const int size = (int)(7 * s);
    const double a = 0.25 + 0.65 * lit;
    int cy = (int)((r.y + r.h / 2) * s);
    int left = (int)(r.x * s), right = (int)((r.x + r.w) * s) - 1;
    int i;

    for (i = 0; i < size; i++) {
        int run = i + 1;                 /* tip at the outside edge */
        int j;

        for (j = -run / 2; j <= run / 2; j++) {
            if (can_left)
                blend(left + i, cy + j, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), a);
            if (can_right)
                blend(right - i, cy + j, (((THEME_ACCENT_RGB) >> 16) & 0xFF), (((THEME_ACCENT_RGB) >> 8) & 0xFF), ((THEME_ACCENT_RGB) & 0xFF), a);
        }
    }
}

/* ----------------------------------------------------- the hero layout's */

void theme_hgrad(ThemeRect r, int n, const double *at, const uint32_t *rgb, const double *alpha)
{
    int x0, y0, x1, y1, x, y, k;
    uint32_t *col;
    int *ca;

    if (!theme_px || n < 1)
        return;
    px_rect(r, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0)
        return;
    col = malloc((size_t)(x1 - x0) * sizeof *col);
    ca = malloc((size_t)(x1 - x0) * sizeof *ca);
    if (!col || !ca) {
        free(col);
        free(ca);
        return;
    }
    /* Each column's colour and alpha, once; then every row is the same run. */
    for (x = x0; x < x1; x++) {
        double t = r.w > 0 ? ((x + 0.5) / theme_scale - r.x) / r.w : 0, u, a;
        int cr, cg, cb;

        for (k = 0; k + 1 < n && t > at[k + 1]; k++)
            ;
        if (k + 1 >= n || t <= at[0]) {
            k = t <= at[0] ? 0 : n - 1;
            cr = RGB_R(rgb[k]); cg = RGB_G(rgb[k]); cb = RGB_B(rgb[k]); a = alpha[k];
        } else {
            u = (t - at[k]) / (at[k + 1] - at[k] > 1e-9 ? at[k + 1] - at[k] : 1e-9);
            cr = (int)(RGB_R(rgb[k]) + (RGB_R(rgb[k + 1]) - RGB_R(rgb[k])) * u);
            cg = (int)(RGB_G(rgb[k]) + (RGB_G(rgb[k + 1]) - RGB_G(rgb[k])) * u);
            cb = (int)(RGB_B(rgb[k]) + (RGB_B(rgb[k + 1]) - RGB_B(rgb[k])) * u);
            a = alpha[k] + (alpha[k + 1] - alpha[k]) * u;
        }
        col[x - x0] = ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
        ca[x - x0] = alpha256(a);
    }
    for (y = y0; y < y1; y++) {
        uint32_t *d = &theme_px[(size_t)y * theme_w];

        for (x = x0; x < x1; x++)
            span_wash(&d[x], 1, col[x - x0], ca[x - x0]);
    }
    free(col);
    free(ca);
}

/* A picture over the whole window: scaled once into a slot (box filtered,
 * as theme_cover's), then each frame a window of it is taken at a fraction
 * of a pixel's offset -- a drift is a translation, so the four weights of
 * the bilinear sample are the same for every pixel, in sixteenths of a
 * pixel each way, and a frame costs a few milliseconds. */
#define HERO_SLOTS 8
typedef struct {
    const uint32_t *src;
    int sw, sh, tw, th;
    double zoom;
    uint32_t *px;
    int pw, ph;
    uint64_t used;
} Hero;
static Hero g_hero[HERO_SLOTS];
static uint64_t g_hero_clock;

static Hero *hero_ready(int slot, const uint32_t *argb, int w, int h, double zoom)
{
    Hero *k;
    double sc;
    int pw, ph;

    if (slot < 0 || slot >= HERO_SLOTS || !argb || w <= 0 || h <= 0 || !theme_px)
        return NULL;
    k = &g_hero[slot];
    if (zoom < 1)
        zoom = 1;
    if (k->px && k->src == argb && k->sw == w && k->sh == h && k->tw == theme_w &&
        k->th == theme_h && k->zoom == zoom) {
        k->used = ++g_hero_clock;
        return k;
    }
    sc = ((double)theme_w / w > (double)theme_h / h ? (double)theme_w / w : (double)theme_h / h) * zoom;
    pw = (int)ceil(w * sc);
    ph = (int)ceil(h * sc);
    if (pw < theme_w + 1) pw = theme_w + 1;     /* a pixel to spare for the sample's right tap */
    if (ph < theme_h + 1) ph = theme_h + 1;
    free(k->px);
    memset(k, 0, sizeof *k);
    k->px = scale_box(argb, w, h, 0, 0, w, h, pw, ph);
    if (!k->px)
        return NULL;
    k->src = argb; k->sw = w; k->sh = h; k->tw = theme_w; k->th = theme_h; k->zoom = zoom;
    k->pw = pw; k->ph = ph;
    k->used = ++g_hero_clock;
    return k;
}

int theme_hero_prepare(int slot, const uint32_t *argb, int w, int h, double zoom)
{
    return hero_ready(slot, argb, w, h, zoom) != NULL;
}

void theme_hero(int slot, const uint32_t *argb, int w, int h, double zoom, double fx, double fy,
                double dx, double dy, double alpha)
{
    Hero *k = hero_ready(slot, argb, w, h, zoom);
    double ox, oy, sx, sy;
    int ix, iy, wx, wy, w00, w10, w01, w11, a = alpha256(alpha), x, y;

    if (!k || a <= 0)
        return;
    sx = k->pw - 1 - theme_w;                    /* what is spare each way */
    sy = k->ph - 1 - theme_h;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    ox = sx * (fx < 0 ? 0 : fx > 1 ? 1 : fx) + dx * theme_scale;
    oy = sy * (fy < 0 ? 0 : fy > 1 ? 1 : fy) + dy * theme_scale;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (ox > sx) ox = sx;
    if (oy > sy) oy = sy;
    ix = (int)ox;
    iy = (int)oy;
    wx = (int)((ox - ix) * 16.0 + 0.5);
    wy = (int)((oy - iy) * 16.0 + 0.5);
    if (wx > 15) wx = 15;
    if (wy > 15) wy = 15;
    w00 = (16 - wx) * (16 - wy); w10 = wx * (16 - wy); w01 = (16 - wx) * wy; w11 = wx * wy;
    for (y = 0; y < theme_h; y++) {
        const uint32_t *s0 = &k->px[(size_t)(iy + y) * k->pw + ix];
        const uint32_t *s1 = s0 + k->pw;
        uint32_t *d = &theme_px[(size_t)y * theme_w];

        for (x = 0; x < theme_w; x++) {
            uint32_t p00 = s0[x], p10 = s0[x + 1], p01 = s1[x], p11 = s1[x + 1];
            uint32_t rb = (((p00 & 0xFF00FFu) * w00 + (p10 & 0xFF00FFu) * w10 +
                            (p01 & 0xFF00FFu) * w01 + (p11 & 0xFF00FFu) * w11) >> 8) & 0xFF00FFu;
            uint32_t g = (((p00 & 0x00FF00u) * w00 + (p10 & 0x00FF00u) * w10 +
                           (p01 & 0x00FF00u) * w01 + (p11 & 0x00FF00u) * w11) >> 8) & 0x00FF00u;

            if (a >= 256)
                d[x] = rb | g;
            else
                span_wash(&d[x], 1, rb | g, a);
        }
    }
}

void theme_hero_forget(void)
{
    int i;

    for (i = 0; i < HERO_SLOTS; i++) {
        free(g_hero[i].px);
        memset(&g_hero[i], 0, sizeof g_hero[i]);
    }
}

int theme_thumb_make(ThemeThumb *t, const uint32_t *argb, int w, int h, int uw, int uh,
                     double fx, double fy)
{
    const double s = theme_scale;
    int pw = (int)ceil(uw * s), ph = (int)ceil(uh * s);
    double sc, cw, ch;

    if (!t)
        return 0;
    t->px = NULL;
    t->pw = t->ph = 0;
    if (!argb || w <= 0 || h <= 0 || pw <= 0 || ph <= 0)
        return 0;
    sc = (double)pw / w > (double)ph / h ? (double)pw / w : (double)ph / h;
    cw = pw / sc;
    ch = ph / sc;
    t->px = scale_box(argb, w, h, (w - cw) * fx, (h - ch) * fy, (w - cw) * fx + cw,
                      (h - ch) * fy + ch, pw, ph);
    if (!t->px)
        return 0;
    t->pw = pw;
    t->ph = ph;
    return 1;
}

void theme_thumb_draw(const ThemeThumb *t, double x, double y, double alpha)
{
    if (t && t->px && theme_px)
        draw_kept(t->px, t->pw, t->ph, (int)floor(x * theme_scale), (int)floor(y * theme_scale), alpha);
}

void theme_thumb_free(ThemeThumb *t)
{
    if (t) {
        free(t->px);
        t->px = NULL;
        t->pw = t->ph = 0;
    }
}
