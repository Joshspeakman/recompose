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
