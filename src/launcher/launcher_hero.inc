/*
 * launcher_hero.inc -- the hero layout (recomp_add_launcher HERO), included
 * by launcher.c in place of launcher_showcase.inc: the same settings, rows
 * and screens, laid out another way. 1280 by 720 window units.
 *
 * The picked game's key art fills the window (launcher_games.h's heroes,
 * from its art folder: a title's tools fetch them, Timeslip's
 * tools/fetch_art.py from the LaunchBox Games Database), drifting slowly and
 * handing over to the next picture every few seconds; picking the other
 * game hands over to its pictures with a slide. Across the bottom, under
 * the game's logo: Play, the settings, Saves, Extras, Updates and Quit in a
 * row, the focused one on a block of the game's colour, what it is set to
 * under the row, and the other game at the row's right. The settings open
 * in a column at the right, the picture dimmed behind them.
 * Extras are the manuals, videos, pictures and links the player (or an
 * install) puts beside the game (launcher_extras.h), shown as tiles with
 * their pictures, or the game's own when they have none.
 *
 * The button hints at the bottom name the controller's buttons when one is
 * in use -- with the game's own button pictures when its art folder has
 * them (art "glyphs": a footer sheet, B A X Y over START BACK and two
 * arrows) -- and the keys when not. Nothing here draws a picture of its own: without the art
 * the window is the ink colour with the game's logo, or its name.
 */
#include "launcher_extras.h"
#include "launcher_image.h"

#define SC_W LAUNCHER_WINDOW_W
#define SC_H LAUNCHER_WINDOW_H

#ifndef THEME_SC_SUBTITLE
#define THEME_SC_SUBTITLE ""
#endif
#define SC_DOT "  \xc2\xb7  "                   /* a middle dot, spaced */
#define SC_ELL "\xe2\x80\xa6"                   /* an ellipsis */
#if defined(_WIN32)
#define SC_TAGLINE "NATIVE FOR WINDOWS" SC_DOT "RECOMPILED FROM YOUR XBOX DISC"
#elif defined(__APPLE__)
#define SC_TAGLINE "NATIVE FOR APPLE SILICON" SC_DOT "RECOMPILED FROM YOUR XBOX DISC"
#else
#define SC_TAGLINE "NATIVE FOR LINUX" SC_DOT "RECOMPILED FROM YOUR XBOX DISC"
#endif

enum { SC_NONE = -1, SC_PLAY = 100, SC_QUIT = 101, SC_BACK = 102, SC_ADVANCED = 103,
       SC_SWITCH = 104, SC_FOLDER = 105, SC_OPEN = 106, SC_PRICES = 107, SC_HELP = 108,
       SC_ITEM = 200, SC_SECTION = 300, SC_TILE = 400 };

/* The home menu. Updates only when the install has an updater. */
enum { HX_PLAY, HX_GRAPHICS, HX_CONTROLS, HX_ONLINE, HX_SAVES, HX_EXTRAS, HX_UPDATES, HX_QUIT,
       HX_KINDS };

static int g_sc_hover = SC_NONE, g_sc_pressed = SC_NONE;
static int g_sc_open = -1;                /* a panel: the Tab it shows, or -1 */
static int g_sc_adv_focus;                /* the Graphics panel's ADVANCED has the keys */
static int g_hx_focus;                    /* the home menu's focused item: its place in hx_items */
static int g_hx_extras;                   /* the Extras are open */
static int g_hx_help;                     /* Play Together's page on setting up a server */
static uint64_t g_hx_help_copied;         /* "copied" shown until then */
static double g_hx_bar = -1;              /* where the focus wash is drawn, sliding to its row */

static COLORREF sc_c(uint32_t rgb) { return THEME_RGB_(rgb); }
static ThemeRect sc_rect(int x, int y, int w, int h)
{
    ThemeRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}
static int sc_in(ThemeRect r, int mx, int my)
{
    return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
}
static void sc_upper(const char *in, char *out, size_t n)
{
    size_t i;

    for (i = 0; in && in[i] && i + 1 < n; i++)
        out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
    out[i] = '\0';
}
static double hx_clamp(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static double hx_ease(double t) { t = hx_clamp(t); return t * t * (3 - 2 * t); }

/* The picked game's colours: its own in launcher_games.c, else the theme's. */
static uint32_t hx_accent(void)
{
#ifdef LAUNCHER_GAMES
    if (launcher_games[g_game].accent)
        return launcher_games[g_game].accent;
#endif
    return THEME_ACCENT_RGB;
}
static uint32_t hx_wash(void)
{
#ifdef LAUNCHER_GAMES
    if (launcher_games[g_game].accent_wash)
        return launcher_games[g_game].accent_wash;
#endif
    return THEME_ACCENT_DIM_RGB;
}
/* The accent, lighter: the focused row's edge. */
static uint32_t hx_bright(void)
{
    uint32_t c = hx_accent(), r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;

    r += (255 - r) / 2; g += (255 - g) / 2; b += (255 - b) / 2;
    return (r << 16) | (g << 8) | b;
}

/* Text: the spaced capitals of the menus, or as it is. */
static void hx_caps(ThemeRect r, const char *text, int pt, uint32_t rgb, ThemeAlign al, int track,
                    int face, int shadow)
{
    char up[256];

    sc_upper(text, up, sizeof up);
    theme_text_ex(r, up, pt, 700, sc_c(rgb), al, track, face, shadow);
}
static void hx_text(ThemeRect r, const char *text, int pt, int weight, uint32_t rgb, ThemeAlign al)
{
    theme_text_ex(r, text, pt, weight, sc_c(rgb), al, 0, THEME_FACE_BODY, 0);
}

/* A heading in the Online panel's box (launcher.c's draw_link). */
static void sc_link_heading(ThemeRect r, const char *text)
{
    hx_caps(r, text, 9, hx_accent(), THEME_LEFT, 3, THEME_FACE_DISPLAY, 0);
}

/* ---------------------------------------------------------------- facts */

static const uint32_t *sc_art(const char *name, int *w, int *h)
{
#ifdef LAUNCHER_GAMES
    return launcher_art_image_of(launcher_games[g_game].title_id, name, w, h);
#else
    return launcher_art_image(name, w, h);
#endif
}

static int sc_has_advanced(void)
{
#ifdef LAUNCHER_GAMES
    return g_adv_count > 0;
#else
    return 0;
#endif
}

#ifdef LAUNCHER_GAMES
static int sc_card_ok(int i)
{
    return i >= 0 && i < launcher_game_count && i < LAUNCHER_GAMES_MAX && g_game_home[i][0];
}
/* The next game that is installed, `dir` (+1 or -1) along the list, or -1. */
static int hx_other_game(int dir)
{
    int k, i;

    for (k = 1; k < launcher_game_count; k++) {
        i = ((g_game + dir * k) % launcher_game_count + launcher_game_count) % launcher_game_count;
        if (sc_card_ok(i))
            return i;
    }
    return -1;
}
#endif

/* The folder the game saves in: UDATA beside its default.xbe. */
static void sc_open_saves(void)
{
    char dir[LAUNCHER_PATH + 16];
    char *slash;

    if (!g_xbe_found[0])
        return;
    snprintf(dir, sizeof dir, "%s", g_xbe_found);
    slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';
    strncat(dir, "/UDATA", sizeof dir - strlen(dir) - 1);
    if (!lhost_open_folder(dir)) {
        if (slash) *slash = '\0';
        lhost_open_folder(dir);
    }
}

/* ------------------------------------------------------------ the extras */

static Extra      g_ex[EXTRAS_MAX];
static ThemeThumb g_ex_thumb[EXTRAS_MAX];
static char       g_ex_tried[EXTRAS_MAX];
static int        g_ex_count, g_ex_game = -1;
static char       g_ex_sections[8][48];
static int        g_ex_nsec;               /* sections, after ALL */
static int        g_ex_sec;                /* 0 ALL, else g_ex_sections[g_ex_sec - 1] */
static int        g_ex_sel, g_ex_top;      /* in the shown list; its first row of tiles */
static uint64_t   g_ex_click_at;

static void hx_extras_scan(void)
{
    int i, j;

    for (i = 0; i < EXTRAS_MAX; i++)
        theme_thumb_free(&g_ex_thumb[i]);
    memset(g_ex_tried, 0, sizeof g_ex_tried);
#ifdef LAUNCHER_GAMES
    g_ex_count = extras_scan(launcher_games[g_game].title_id, g_ex, EXTRAS_MAX);
#else
    g_ex_count = extras_scan(g_title_id, g_ex, EXTRAS_MAX);
#endif
    g_ex_game = g_game;
    g_ex_nsec = 0;
    for (i = 0; i < g_ex_count; i++) {
        for (j = 0; j < g_ex_nsec; j++)
            if (!strcmp(g_ex_sections[j], g_ex[i].section))
                break;
        if (j == g_ex_nsec && g_ex_nsec < 8)
            snprintf(g_ex_sections[g_ex_nsec++], sizeof g_ex_sections[0], "%s", g_ex[i].section);
    }
    if (g_ex_sec > g_ex_nsec)
        g_ex_sec = 0;
    g_ex_sel = g_ex_top = 0;
}

/* The items the chosen section shows, as indices into g_ex. */
static int hx_extras_shown(int *idx)
{
    int i, n = 0;

    for (i = 0; i < g_ex_count; i++)
        if (!g_ex_sec || !strcmp(g_ex[i].section, g_ex_sections[g_ex_sec - 1]))
            idx[n++] = i;
    return n;
}

enum { EX_COLS = 4, EX_TW = 268, EX_TH = 151, EX_GAP = 24, EX_X0 = 64, EX_Y0 = 200,
       EX_PITCH = EX_TH + 66, EX_ROWS = 2 };

static ThemeRect hx_tile_rect(int k)
{
    int row = k / EX_COLS - g_ex_top, col = k % EX_COLS;

    return sc_rect(EX_X0 + col * (EX_TW + EX_GAP), EX_Y0 + row * EX_PITCH, EX_TW, EX_TH);
}

static ThemeRect hx_section_rect(int s)
{
    /* ALL, then each section: as wide as its name, in the order found. */
    int x = EX_X0, i;

    for (i = 0; i < s; i++) {
        const char *name = i == 0 ? "ALL" : g_ex_sections[i - 1];
        x += (int)strlen(name) * 11 + 44;
    }
    return sc_rect(x - 8, 140, (int)strlen(s == 0 ? "ALL" : g_ex_sections[s - 1]) * 11 + 24, 34);
}

static int hx_hero_count(int game);
static const uint32_t *hx_hero_art(int game, int k, int *w, int *h);

/* A picture for an item without one of its own: one of the game's key art. */
static const uint32_t *hx_fallback_art(int k, int *w, int *h)
{
    int n = hx_hero_count(g_game), i;

    for (i = 0; i < n; i++) {
        const uint32_t *px = hx_hero_art(g_game, (k + i) % n, w, h);

        if (px)
            return px;
    }
    return NULL;
}

/* One missing thumbnail a frame, so a long list fills in without a stall. */
static void hx_extras_thumbs(const int *idx, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int k = idx[i], w = 0, h = 0;
        uint32_t *own = NULL;
        const uint32_t *px;

        if (g_ex_tried[k])
            continue;
        g_ex_tried[k] = 1;
        if (g_ex[k].picture[0])
            own = launcher_image_load(g_ex[k].picture, &w, &h);
        px = own ? own : hx_fallback_art(k, &w, &h);
        if (px)
            theme_thumb_make(&g_ex_thumb[k], px, w, h, EX_TW, EX_TH, 0.5, own ? 0.5 : 0.42);
        free(own);
        return;
    }
}

static void hx_extras_open_folder(void)
{
    char dir[1024];

#ifdef LAUNCHER_GAMES
    if (extras_dir(launcher_games[g_game].title_id, dir, sizeof dir, 1))
#else
    if (extras_dir(g_title_id, dir, sizeof dir, 1))
#endif
        lhost_open(dir);
}

static void hx_extras_open_item(void)
{
    int idx[EXTRAS_MAX], n = hx_extras_shown(idx);

    if (g_ex_sel >= 0 && g_ex_sel < n)
        extras_open(&g_ex[idx[g_ex_sel]]);
}

static void hx_extras_show(int open)
{
    g_hx_extras = open;
    if (open)
        hx_extras_scan();
}

/* ----------------------------------------------------- the moving picture */

#define HX_HOLD   11000                    /* ms a picture is up */
#define HX_FADE    1800                    /* the hand-over to the next */
#define HX_SWITCH   700                    /* another game's: quicker, with a slide */
#define HX_SLIDE     70.0                  /* how far, in window units */

/* No pictures named: none (the ink, and the logo). */
static const LauncherHero k_hx_fallback[] = { { NULL, 0, 0, 0, 0, 0 } };

static const LauncherHero *hx_heroes(int game)
{
#ifdef LAUNCHER_GAMES
    if (game >= 0 && game < launcher_game_count && launcher_games[game].heroes)
        return launcher_games[game].heroes;
#else
    (void)game;
#endif
    return k_hx_fallback;
}

static const uint32_t *hx_hero_art(int game, int k, int *w, int *h)
{
    const LauncherHero *hs = hx_heroes(game);

#ifdef LAUNCHER_GAMES
    return launcher_art_image_of(launcher_games[game].title_id, hs[k].art, w, h);
#else
    (void)game;
    return launcher_art_image(hs[k].art, w, h);
#endif
}

static int hx_hero_count(int game)
{
    const LauncherHero *hs = hx_heroes(game);
    int n = 0;

    while (hs[n].art && n < 4)
        n++;
    return n;
}

/* The next of the game's pictures that is there, after k (-1: the first). */
static int hx_hero_next(int game, int k)
{
    int n = hx_hero_count(game), i, w, h;

    for (i = 1; i <= n; i++) {
        int j = (k + i + n) % n;

        if (k < 0)
            j = i - 1;
        if (hx_hero_art(game, j, &w, &h))
            return j;
    }
    return -1;
}

typedef struct { int game, k; uint64_t since; } HxShown;
static HxShown  g_hx_now = { -1, -1, 0 }, g_hx_was = { -1, -1, 0 };
static uint64_t g_hx_fade_at, g_hx_fade_len;
static int      g_hx_slide;                /* the last hand-over was a game's: +1/-1 its way */
static int      g_hx_warm;                 /* the other game's first picture, scaled ahead */

static void hx_draw_one(const HxShown *s, uint64_t now, double alpha, double slide)
{
    const LauncherHero *h;
    const uint32_t *px;
    int w = 0, hh = 0;
    double d;

    if (s->game < 0 || s->k < 0 || !(px = hx_hero_art(s->game, s->k, &w, &hh)))
        return;
    h = &hx_heroes(s->game)[s->k];
    d = (double)(now - s->since) / (HX_HOLD + HX_FADE);
    theme_hero((s->game & 1) * 4 + (s->k & 3), px, w, hh, h->zoom, h->fx, h->fy,
               h->drift_x * (d - 0.5) + slide, h->drift_y * (d - 0.5), alpha);
}

/* The picture behind everything: the one up, the one before it over the
 * hand-over, and `dim` of the ink over both (a panel's). */
static void hx_background(double dim)
{
    uint64_t now = lhost_ticks();
    int n;

    if (g_hx_now.game != g_game) {
        int first = hx_hero_next(g_game, -1);

        g_hx_was = g_hx_now;
        g_hx_slide = g_hx_now.game < 0 ? 0 : (g_game > g_hx_now.game ? 1 : -1);
        g_hx_now.game = g_game;
        g_hx_now.k = first;
        g_hx_now.since = now;
        g_hx_fade_at = now;
        g_hx_fade_len = g_hx_was.game >= 0 ? HX_SWITCH : 0;
    }
    n = hx_hero_count(g_game);
    if (g_hx_now.k >= 0 && n > 1 && now - g_hx_now.since > HX_HOLD) {
        int next = hx_hero_next(g_game, g_hx_now.k);

        if (next >= 0 && next != g_hx_now.k) {
            g_hx_was = g_hx_now;
            g_hx_now.k = next;
            g_hx_now.since = now;
            g_hx_fade_at = now;
            g_hx_fade_len = HX_FADE;
            g_hx_slide = 0;
        }
    }
    if (g_hx_now.k < 0 && g_hx_now.game == g_game && now - g_hx_now.since > 500) {
        g_hx_now.k = hx_hero_next(g_game, -1);   /* the art arrived late */
        g_hx_now.since = now;
    }

    theme_fill(sc_rect(0, 0, SC_W, SC_H), THEME_INK_RGB, 1);
    if (g_hx_fade_len && now - g_hx_fade_at < g_hx_fade_len && g_hx_was.k >= 0) {
        double p = hx_ease((double)(now - g_hx_fade_at) / g_hx_fade_len);

        hx_draw_one(&g_hx_was, now, 1, -g_hx_slide * HX_SLIDE * p);
        hx_draw_one(&g_hx_now, now, p, g_hx_slide * HX_SLIDE * (1 - p));
    } else {
        hx_draw_one(&g_hx_now, now, 1, 0);
    }
    if (dim > 0)
        theme_fill(sc_rect(0, 0, SC_W, SC_H), THEME_INK_RGB, dim);

    /* Ahead of their turns: the next picture, three seconds before it is
     * due; the other game's first, once, a second after the window opens. */
    if (n > 1 && g_hx_now.k >= 0 && now - g_hx_now.since > HX_HOLD - 3000) {
        int next = hx_hero_next(g_game, g_hx_now.k), w, h;
        const uint32_t *px = next >= 0 ? hx_hero_art(g_game, next, &w, &h) : NULL;

        if (px)
            theme_hero_prepare((g_game & 1) * 4 + (next & 3), px, w, h, hx_heroes(g_game)[next].zoom);
    }
#ifdef LAUNCHER_GAMES
    if (!g_hx_warm && now - g_hx_now.since > 1000) {
        int other = hx_other_game(1), k, w, h;
        const uint32_t *px;

        g_hx_warm = 1;
        if (other >= 0 && (k = hx_hero_next(other, -1)) >= 0 && (px = hx_hero_art(other, k, &w, &h)))
            theme_hero_prepare((other & 1) * 4 + (k & 3), px, w, h, hx_heroes(other)[k].zoom);
    }
#endif
}

/* Ink washed in from the right (under the menu), down from the top and up
 * from the bottom, so the words read over any picture. `from` is where the
 * right-hand wash starts; `strength` its darkest. */
static void hx_scrims(int from, double strength)
{
    double at3[3] = { 0, 0.5, 1 }, a3[3] = { 0, strength * 0.78, strength };
    uint32_t c3[3] = { THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB };
    double at2[2] = { 0, 1 }, top[2] = { 0.55, 0 }, bot[2] = { 0, 0.86 };

    theme_hgrad(sc_rect(from, 0, SC_W - from, SC_H), 3, at3, c3, a3);
    theme_vgrad(sc_rect(0, 0, SC_W, 96), 2, at2, c3, top);
    theme_vgrad(sc_rect(0, SC_H - 150, SC_W, 150), 2, at2, c3, bot);
}

/* The game's logo in r, at the right; its name when it has none. Fades in
 * after a game is picked. */
static void hx_logo(ThemeRect r, ThemeAlign al, double alpha)
{
    int lw = 0, lh = 0;
    const uint32_t *logo = sc_art("logo_hd", &lw, &lh);    /* fetch_art.py's, else the disc's */

    if (!logo)
        logo = sc_art("logo", &lw, &lh);

    if (logo) {
        theme_picture_ex(r, logo, lw, lh, al, alpha, 6, NULL);
    } else {
#ifdef LAUNCHER_GAMES
        const char *name = launcher_games[g_game].name;
#else
        const char *name = g_title_name;
#endif
        hx_caps(r, name, 24, THEME_TEXT_RGB, al, 4, THEME_FACE_HEAVY, 1);
    }
}

/* -------------------------------------------------------- the button hints */

/* The game's own button pictures (art "glyphs": its footer's sheet, B A X Y
 * over START BACK and two arrows, 32 by 32 each), cut to what is drawn in
 * each, made once at the size they are shown. */
enum { HX_G_B, HX_G_A, HX_G_X, HX_G_Y, HX_GLYPHS };
static ThemeThumb g_hx_glyph[HX_GLYPHS];
static const uint32_t *g_hx_glyph_src;
static int g_hx_glyph_w[HX_GLYPHS];

static void hx_glyphs_make(void)
{
    const uint32_t *px = NULL;
    int w = 0, h = 0, i;

#ifdef LAUNCHER_GAMES
    px = sc_art("glyphs", &w, &h);
    for (i = 0; !px && i < launcher_game_count; i++)
        px = launcher_art_image_of(launcher_games[i].title_id, "glyphs", &w, &h);
#else
    px = sc_art("glyphs", &w, &h);
#endif
    if (px == g_hx_glyph_src)
        return;
    g_hx_glyph_src = px;
    for (i = 0; i < HX_GLYPHS; i++) {
        theme_thumb_free(&g_hx_glyph[i]);
        g_hx_glyph_w[i] = 0;
    }
    if (!px || w < 128 || h < 64)
        return;
    for (i = 0; i < HX_GLYPHS; i++) {
        int cx = (i % 4) * (w / 4), cy = 0, cw = w / 4, ch = h / 2, x, y;
        int x0 = cw, y0 = ch, x1 = -1, y1 = -1;
        uint32_t *cut;

        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++)
                if ((px[(size_t)(cy + y) * w + cx + x] >> 24) > 20) {
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x > x1) x1 = x;
                    if (y > y1) y1 = y;
                }
        if (x1 < x0 || y1 < y0)
            continue;
        cut = malloc((size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * sizeof *cut);
        if (!cut)
            continue;
        for (y = y0; y <= y1; y++)
            memcpy(&cut[(size_t)(y - y0) * (x1 - x0 + 1)], &px[(size_t)(cy + y) * w + cx + x0],
                   (size_t)(x1 - x0 + 1) * sizeof *cut);
        g_hx_glyph_w[i] = (int)(22.0 * (x1 - x0 + 1) / (y1 - y0 + 1) + 0.5);
        theme_thumb_make(&g_hx_glyph[i], cut, x1 - x0 + 1, y1 - y0 + 1, g_hx_glyph_w[i], 22, 0.5, 0.5);
        free(cut);
    }
}

/* A hint: the button (an HX_G_*, or -1 for none), the key without a
 * controller, what to write with one when there is no button picture
 * (NULL: the key), what it does, and what a click on it is (SC_NONE: nothing). */
typedef struct { int glyph; const char *key, *pad_key, *label; int code; } HxHint;
static ThemeRect g_hx_hint_rect[6];
static int       g_hx_hint_code[6], g_hx_hints;

/* The hints, right-aligned on the bottom line: with a controller, its
 * button's picture (or its letter, without the art); without, the key. */
static void hx_hints(const HxHint *hint, int n)
{
    int x = SC_W - 64, i;

    hx_glyphs_make();
    g_hx_hints = 0;
    for (i = n - 1; i >= 0; i--) {
        const HxHint *h = &hint[i];
        static const char *const letter[] = { "B", "A", "X", "Y" };
        const char *keytxt = !g_pad_seen ? h->key
                           : h->glyph >= 0 ? letter[h->glyph] : h->pad_key ? h->pad_key : h->key;
        int tw = (int)strlen(h->label) * 10 + 8, kw, hot, plain = (unsigned char)keytxt[0] >= 0x80;
        ThemeRect lr, kr;

        lr = sc_rect(x - tw, SC_H - 50, tw, 28);
        if (g_pad_seen && h->glyph >= 0 && g_hx_glyph[h->glyph].px)
            kw = g_hx_glyph_w[h->glyph];
        else
            kw = plain ? 40 : (int)strlen(keytxt) * 9 + 4;
        kr = sc_rect(lr.x - 8 - kw, SC_H - 50, kw, 28);
        hot = g_sc_hover == h->code && h->code != SC_NONE;
        theme_text_ex(lr, h->label, 11, 600, hot ? RGB(255, 255, 255) : sc_c(0xD2DEF0), THEME_LEFT, 0,
                      THEME_FACE_DISPLAY, 1);
        if (g_pad_seen && h->glyph >= 0 && g_hx_glyph[h->glyph].px) {
            theme_thumb_draw(&g_hx_glyph[h->glyph], kr.x, kr.y + 3, 1);
        } else {
            /* Arrows are not in the display face: the body's has them. */
            hx_caps(kr, keytxt, plain ? 12 : 10, hot ? 0xFFFFFF : hx_accent(), THEME_RIGHT, 1,
                    plain ? THEME_FACE_BODY : THEME_FACE_DISPLAY, 1);
        }
        if (g_hx_hints < 6) {
            g_hx_hint_rect[g_hx_hints] = sc_rect(kr.x - 4, kr.y - 4, lr.x + lr.w - kr.x + 8, 36);
            g_hx_hint_code[g_hx_hints++] = h->code;
        }
        x = kr.x - 30;
    }
}

static int hx_hint_hit(int mx, int my)
{
    int i;

    for (i = 0; i < g_hx_hints; i++)
        if (g_hx_hint_code[i] != SC_NONE && sc_in(g_hx_hint_rect[i], mx, my))
            return g_hx_hint_code[i];
    return SC_NONE;
}

/* --------------------------------------------------------------- the menu */

static int hx_items(int *out)
{
    int n = 0;

    out[n++] = HX_PLAY;
    out[n++] = HX_GRAPHICS;
    out[n++] = HX_CONTROLS;
    out[n++] = HX_ONLINE;
    out[n++] = HX_SAVES;
    out[n++] = HX_EXTRAS;
    if (g_upd != UPD_OFF)
        out[n++] = HX_UPDATES;
    out[n++] = HX_QUIT;
    return n;
}

static const char *hx_item_name(int kind)
{
    static const char *const t[] = { "PLAY", "GRAPHICS", "CONTROLS", "PLAY TOGETHER", "SAVES",
                                     "EXTRAS", "UPDATES", "QUIT" };
    return t[kind];
}

/* What an item is set to, or what it does: under it while it is focused. */
static void hx_item_caption(int kind, char *out, size_t n)
{
    out[0] = '\0';
    switch (kind) {
    case HX_PLAY:
#ifdef LAUNCHER_GAMES
        sc_upper(launcher_games[g_game].card_line, out, n);
#endif
        break;
    case HX_GRAPHICS:
        snprintf(out, n, "%s" SC_DOT "%s" SC_DOT "%s",
                 g_settings.fullscreen ? "FULLSCREEN" : "WINDOW",
                 g_settings.resolution_scale == 1 ? "CONSOLE RESOLUTION" : "",
                 g_settings.widescreen ? "16:9" : "4:3");
        if (g_settings.resolution_scale != 1)
            snprintf(out, n, "%s" SC_DOT "%d\xc3\x97" SC_DOT "%s",
                     g_settings.fullscreen ? "FULLSCREEN" : "WINDOW", g_settings.resolution_scale,
                     g_settings.widescreen ? "16:9" : "4:3");
        break;
    case HX_CONTROLS:
        snprintf(out, n, "%s", g_settings.mouse_look ? "KEYS & MOUSE" : "CONTROLLER");
        break;
    case HX_ONLINE:
        if (!strcmp(g_settings.coop, "host"))         snprintf(out, n, "CO-OP" SC_DOT "HOSTING");
        else if (!strcmp(g_settings.coop, "join"))    snprintf(out, n, "CO-OP" SC_DOT "JOINING");
        else if (!strcmp(g_settings.syslink, "host")) snprintf(out, n, "SYSTEM LINK" SC_DOT "HOST");
        else if (!strcmp(g_settings.syslink, "join")) snprintf(out, n, "SYSTEM LINK" SC_DOT "JOIN");
        else                                          snprintf(out, n, "ONLINE CO-OP" SC_DOT "SYSTEM LINK");
        break;
    case HX_SAVES:
        snprintf(out, n, "OPENS THE GAME'S SAVES FOLDER");
        break;
    case HX_EXTRAS:
        if (g_ex_game != g_game)
            hx_extras_scan();
        if (g_ex_count)
            snprintf(out, n, "%d %s" SC_DOT "MANUALS, VIDEOS, PICTURES", g_ex_count,
                     g_ex_count == 1 ? "ITEM" : "ITEMS");
        else
            snprintf(out, n, "MANUALS, VIDEOS, PICTURES");
        break;
    case HX_UPDATES:
        switch (g_upd) {
        case UPD_CHECKING:  snprintf(out, n, "CHECKING" SC_ELL); break;
        case UPD_CURRENT:   snprintf(out, n, "UP TO DATE"); break;
        case UPD_AVAILABLE: snprintf(out, n, "%s IS READY", g_upd_tag); break;
        case UPD_NONE:      snprintf(out, n, "NO RELEASE YET"); break;
        default:            snprintf(out, n, "CAN'T CHECK"); break;
        }
        break;
    default:
        break;
    }
}

/* The menu: a row across the bottom, under the logo, the key art above it
 * clear. Each item's place along the row, from its measured width. */
enum { HX_ROW_X = 60, HX_ROW_Y = SC_H - 116, HX_ROW_H = 50, HX_GAP = 34, HX_R = SC_W - 64 };

static ThemeRect hx_item_rect(int i)
{
    int items[HX_KINDS], n = hx_items(items), x = HX_ROW_X, k;

    if (i >= n)
        i = n - 1;
    for (k = 0; k <= i; k++) {
        static int width[HX_KINDS];              /* each name measured once */
        int big = items[k] == HX_PLAY, w = width[items[k]];

        if (w <= 0) {
            w = big ? theme_text_width(hx_item_name(items[k]), 22, 700, 6, THEME_FACE_HEAVY)
                    : theme_text_width(hx_item_name(items[k]), 14, 700, 2, THEME_FACE_CONDENSED_BOLD);
            if (w > 0)
                width[items[k]] = w;
            else
                w = (int)strlen(hx_item_name(items[k])) * (big ? 24 : 12);
        }
        if (k == i)
            return sc_rect(x - 18, HX_ROW_Y, w + 36, HX_ROW_H);
        x += w + (big ? HX_GAP + 18 : HX_GAP);
    }
    return sc_rect(x, HX_ROW_Y, 10, HX_ROW_H);
}

static ThemeRect g_hx_switch_rect;

/* The other game, at the right of the row: its logo, and how to get to it. */
static void hx_switcher(void)
{
#ifdef LAUNCHER_GAMES
    int other = hx_other_game(1), lw = 0, lh = 0, hot = g_sc_hover == SC_SWITCH, tw;
    const uint32_t *ol;
    ThemeRect lr, drawn;
    char line[64];

    g_hx_switch_rect = sc_rect(0, 0, 0, 0);
    if (other < 0)
        return;
    snprintf(line, sizeof line, "%s  SWITCH GAME", g_pad_seen ? "RB" : "TAB");
    tw = theme_text_width(line, 9, 700, 3, THEME_FACE_DISPLAY);
    if (tw <= 0)
        tw = 150;
    ol = launcher_art_image_of(launcher_games[other].title_id, "logo_hd", &lw, &lh);
    if (!ol)
        ol = launcher_art_image_of(launcher_games[other].title_id, "logo", &lw, &lh);
    /* Over the row's right end, clear of the menu however long it is. */
    lr = sc_rect(SC_W - 56 - tw - 18 - 150, HX_ROW_Y - 74, 150, 56);
    drawn = lr;
    if (ol)
        theme_picture_ex(lr, ol, lw, lh, THEME_RIGHT, hot ? 1.0 : 0.75, 7, &drawn);
    else
        hx_caps(sc_rect(lr.x - 80, lr.y, lr.w + 80, lr.h), launcher_games[other].card_name, 10,
                hot ? 0xFFFFFF : 0xB4C0D4, THEME_RIGHT, 2, THEME_FACE_HEAVY, 1);
    hx_caps(sc_rect(SC_W - 56 - tw, lr.y, tw, lr.h), line, 9, hot ? 0xFFFFFF : 0xA4B2CA,
            THEME_RIGHT, 3, THEME_FACE_DISPLAY, 1);
    g_hx_switch_rect = sc_rect(drawn.x - 8, lr.y - 6, SC_W - 48 - drawn.x, lr.h + 12);
#endif
}

static void hx_home(void)
{
    int items[HX_KINDS], n = hx_items(items), i, lw = 0, lh = 0;
    uint64_t now = lhost_ticks();
    double la = g_hx_fade_len == HX_SWITCH ? hx_ease((double)(now - g_hx_fade_at) / 500.0) : 1;
    char cap[200];
    ThemeRect r;
    static const HxHint hints[] = {
        { HX_G_A, "ENTER", NULL, "Select", SC_OPEN },
        { HX_G_B, "ESC", NULL, "Quit", SC_QUIT },
    };

    if (g_hx_focus >= n) g_hx_focus = n - 1;
    if (g_hx_focus < 0) g_hx_focus = 0;
    hx_background(0);
    {   /* Ink up from the bottom for the menu, darker at its left; a little at the top. */
        double at3[3] = { 0, 0.45, 1 }, a3[3] = { 0, 0.72, 0.95 };
        double at2[2] = { 0, 1 }, l2[2] = { 0.55, 0 }, top[2] = { 0.5, 0 };
        uint32_t ink[3] = { THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB };

        int k;

        theme_vgrad(sc_rect(0, SC_H - 320, SC_W, 320), 3, at3, ink, a3);
        /* The left's extra ink fades in downward too, in bands: no edge. */
        for (k = 0; k < 20; k++) {
            double t = (k + 0.5) / 20.0;

            l2[0] = 0.55 * t * t;
            theme_hgrad(sc_rect(0, SC_H - 320 + k * 16, 780, 16), 2, at2, ink, l2);
        }
        theme_vgrad(sc_rect(0, 0, SC_W, 70), 2, at2, ink, top);
    }
    /* The logo, sitting on the menu. */
    if (sc_art("logo_hd", &lw, &lh) || sc_art("logo", &lw, &lh)) {
        int h = lw > 0 ? 430 * lh / lw : 150;

        if (h > 170)
            h = 170;
        hx_logo(sc_rect(HX_ROW_X - 6, HX_ROW_Y - 14 - h, 430, h), THEME_LEFT, la);
    } else {
        hx_logo(sc_rect(HX_ROW_X, HX_ROW_Y - 70, 1000, 56), THEME_LEFT, la);
    }
    hx_caps(sc_rect(24, 16, 900, 18), THEME_SC_SUBTITLE SC_DOT SC_TAGLINE, 8, 0xA4B2CA, THEME_LEFT, 3,
            THEME_FACE_DISPLAY, 1);

    /* The focus: a block of the game's colour that slides to its item. */
    r = hx_item_rect(g_hx_focus);
    if (g_hx_bar < 0)
        g_hx_bar = r.x;
    g_hx_bar += (r.x - g_hx_bar) * 0.34;
    if (g_hx_bar > r.x - 0.5 && g_hx_bar < r.x + 0.5)
        g_hx_bar = r.x;
    {
        static double bw = -1;
        double at[2] = { 0, 1 }, a[2] = { 0.92, 0.78 };
        uint32_t c[2] = { hx_wash(), hx_wash() };
        int bx = (int)(g_hx_bar + 0.5), w;

        if (bw < 0)
            bw = r.w;
        bw += (r.w - bw) * 0.34;
        w = (int)(bw + 0.5);
        theme_vgrad(sc_rect(bx, r.y, w, r.h), 2, at, c, a);
        theme_fill(sc_rect(bx, r.y + r.h - 3, w, 3), hx_bright(), 1);
    }
    for (i = 0; i < n; i++) {
        int on = i == g_hx_focus, big = items[i] == HX_PLAY;
        ThemeRect ir = hx_item_rect(i);

        if (big && !on)
            theme_fill(ir, hx_wash(), 0.34);          /* Play stands out unfocused too */
        if (big)
            hx_caps(ir, hx_item_name(items[i]), 22, 0xFFFFFF, THEME_CENTRE, 6, THEME_FACE_HEAVY, 1);
        else
            hx_caps(ir, hx_item_name(items[i]), 14, on ? 0xFFFFFF : 0xC6D0E2, THEME_CENTRE, 2,
                    THEME_FACE_CONDENSED_BOLD, 1);
    }
    hx_item_caption(items[g_hx_focus], cap, sizeof cap);
    if (cap[0])
        hx_caps(sc_rect(HX_ROW_X, SC_H - 54, 760, 28), cap, 10, hx_bright(), THEME_LEFT, 2,
                THEME_FACE_CONDENSED, 1);
    hx_switcher();
    hx_hints(hints, 2);
}

/* ------------------------------------------- setting up an online server */

/* Fly.io's own commands (fly.io/docs/flyctl/install), this system's. */
#if defined(_WIN32)
#define HX_FLY_INSTALL "pwsh -Command \"iwr https://fly.io/install.ps1 -useb | iex\""
#define HX_FLY_SETUP   "powershell -ExecutionPolicy Bypass -File deploy.ps1"
#elif defined(__APPLE__)
#define HX_FLY_INSTALL "brew install flyctl"
#define HX_FLY_SETUP   "sh deploy.sh"
#else
#define HX_FLY_INSTALL "curl -L https://fly.io/install.sh | sh"
#define HX_FLY_SETUP   "sh deploy.sh"
#endif
#define HX_FLY_PRICING "https://fly.io/docs/about/pricing/"

/* The server's files (recompose's tools/online_server): beside the launcher
 * in an install (Timeslip's installer puts them there), else in the source
 * tree a build runs from. 1 with the folder in out. */
static int hx_server_folder(char *out, size_t n)
{
    static const char *const rel[] = { "online_server", "../../recompose/tools/online_server",
                                       "../../../recompose/tools/online_server" };
    char here[1024], probe[1200];
    int i;

    if (!host_exe_path(here, sizeof here))
        return 0;
    host_path_dirname(here);
    for (i = 0; i < 3; i++) {
        FILE *f;

        snprintf(probe, sizeof probe, "%s/%s/server.py", here, rel[i]);
        if ((f = fopen(probe, "rb")) != NULL) {
            fclose(f);
            snprintf(out, n, "%s/%s", here, rel[i]);
            return 1;
        }
    }
    return 0;
}

static void hx_help_folder(void)
{
    char dir[1100];

    if (hx_server_folder(dir, sizeof dir))
        lhost_open(dir);
}

static void hx_help_copy(void)
{
    if (lhost_copy(HX_FLY_INSTALL))
        g_hx_help_copied = lhost_ticks() + 1500;
}

static void hx_help_draw(void)
{
    /* Each step: what it is, and the line to type (or none), set apart. */
    static const char *const head[] = {
        "WHAT IT IS", "MAKE A FLY.IO ACCOUNT, AND INSTALL FLYCTL", "RUN THE SETUP IN THE SERVER FOLDER",
        "CONNECT", "LATER" };
    char body[5][600], dir[1100], shown[1100];
    const char *cmd[5] = { NULL, HX_FLY_INSTALL, HX_FLY_SETUP, NULL, "flyctl apps destroy <its name>" };
    int y = 116, i;
    HxHint hints[4];
    int nh = 0;

    hx_background(0.5);
    theme_fill(sc_rect(0, 0, SC_W, SC_H), THEME_INK_RGB, 0.45);
    hx_caps(sc_rect(EX_X0, 30, 900, 50), "SET UP AN ONLINE SERVER", 26, 0xFFFFFF, THEME_LEFT, 6,
            THEME_FACE_HEAVY, 1);
    hx_caps(sc_rect(EX_X0, 80, 900, 20), "ON FLY.IO" SC_DOT "A FEW MINUTES, ONCE, BY ONE OF YOU", 9,
            hx_accent(), THEME_LEFT, 4, THEME_FACE_DISPLAY, 1);
    hx_logo(sc_rect(HX_R - 300 + 8, 22, 300, 96), THEME_RIGHT, 0.9);
    if (hx_server_folder(dir, sizeof dir)) {
        size_t k;

        lhost_full_path(dir, shown, sizeof shown);
#ifdef _WIN32
        for (k = 0; shown[k]; k++)
            if (shown[k] == '/')
                shown[k] = '\\';
#else
        (void)k;
#endif
    } else {
        snprintf(shown, sizeof shown, "the online_server folder of the game's source");
    }

    snprintf(body[0], sizeof body[0],
        "One server every copy connects to, for System Link and online co-op: nobody opens a"
        " port on their router. One of you sets it up and shares its address. On Fly.io it costs"
        " about $4.20 a month at their prices (a small machine, $2.19, and the dedicated IPv4"
        " address UDP needs, $2); Fly's free plan gives $5 of use a month once a card is added.");
    snprintf(body[1], sizeof body[1],
        "Sign up at fly.io. Then, in a terminal, install flyctl, Fly's command line (A copies it):");
    snprintf(body[2], sizeof body[2],
        "The folder is %s (X opens it). There, in a terminal, run the line below. It signs you in,"
        " asks a name and the region nearest you, and makes and starts the server. At the end it"
        " prints the address, like name.fly.dev.", shown);
    snprintf(body[3], sizeof body[3],
        "Back here, in Play Together: Connect, Through an online server; Server address, what it"
        " printed; Session code, any word of four letters or more. Everyone playing sets the same"
        " two, then hosts or joins as usual.");
    snprintf(body[4], sizeof body[4],
        "The server runs until it is removed, which ends its cost too; the README in the server"
        " folder has the steps by hand:");
    for (i = 0; i < 5; i++) {
        char num[4];
        static const int line_count[5] = { 3, 1, 2, 2, 1 };
        int lines = line_count[i];

        snprintf(num, sizeof num, "%d", i + 1);
        theme_text_ex(sc_rect(EX_X0, y, 40, 30), num, 20, 700, sc_c(hx_bright()), THEME_LEFT, 0,
                      THEME_FACE_HEAVY, 1);
        hx_caps(sc_rect(EX_X0 + 44, y + 2, 900, 22), head[i], 12, 0xFFFFFF, THEME_LEFT, 3,
                THEME_FACE_CONDENSED_BOLD, 1);
        theme_text_wrapped(sc_rect(EX_X0 + 44, y + 28, SC_W - EX_X0 * 2 - 44, 20 * lines + 4),
                           body[i], 11, sc_c(0xC6D0E2));
        y += 28 + 20 * lines + 8;
        if (cmd[i]) {
            ThemeRect c = sc_rect(EX_X0 + 44, y, SC_W - EX_X0 * 2 - 44, 30);

            theme_fill(c, 0x000000, 0.55);
            theme_fill(sc_rect(c.x, c.y, 3, c.h), hx_bright(), 1);
            theme_text_ex(sc_rect(c.x + 14, c.y, c.w - 20, c.h), cmd[i], 12, 600, sc_c(0xEAF2FF),
                          THEME_LEFT, 0, THEME_FACE_BODY, 0);
            y += 30 + 6;
        }
        y += 10;
    }
    if (lhost_ticks() < g_hx_help_copied)
        hx_text(sc_rect(EX_X0, SC_H - 50, 400, 28), "Copied.", 12, 600, hx_bright(), THEME_LEFT);
    hints[nh].glyph = HX_G_A; hints[nh].key = "ENTER"; hints[nh].pad_key = NULL;
    hints[nh].label = "Copy the install line"; hints[nh++].code = SC_OPEN;
    hints[nh].glyph = HX_G_X; hints[nh].key = "F"; hints[nh].pad_key = NULL;
    hints[nh].label = "Server folder"; hints[nh++].code = SC_FOLDER;
    hints[nh].glyph = HX_G_Y; hints[nh].key = "P"; hints[nh].pad_key = NULL;
    hints[nh].label = "Fly's prices"; hints[nh++].code = SC_PRICES;
    hints[nh].glyph = HX_G_B; hints[nh].key = "ESC"; hints[nh].pad_key = NULL;
    hints[nh].label = "Back"; hints[nh++].code = SC_BACK;
    hx_hints(hints, nh);
}

/* --------------------------------------------------------------- a panel */

enum { HX_PANEL_X = 610, HX_PANEL_W = SC_W - 64 - 610, HX_ROWS_Y = 134, HX_PITCH = 46,
       SC_BIND_GAP = 22, SC_BIND_COL_W = 131 };

static int sc_panel_rows(void)
{
    return g_sc_open == TAB_INPUT ? BIND_ROWS_VISIBLE
         : g_sc_open == TAB_LINK  ? g_link_count
#ifdef LAUNCHER_GAMES
         : g_sc_open == TAB_ADVANCED ? g_adv_count
#endif
         : g_video_count;
}

static int sc_panel_extra(void)
{
    return g_sc_open == TAB_LINK  ? (g_link_count > 1 ? 52 : 12) + link_box_h() + 30
         : g_sc_open == TAB_INPUT ? 48 + SC_BIND_GAP
         : 48;
}

static int sc_pitch(void)
{
    int rows = sc_panel_rows(), room = SC_H - 96 - HX_ROWS_Y - sc_panel_extra();
    int pitch = rows > 0 ? room / rows : HX_PITCH;

    return pitch >= HX_PITCH ? HX_PITCH : pitch < 36 ? 36 : pitch;
}

static void sc_rows_in_panel(void)
{
    g_row_x = HX_PANEL_X; g_row_y = HX_ROWS_Y; g_row_w = HX_PANEL_W;
    g_row_pitch = sc_pitch(); g_row_h = g_row_pitch - 6;
    g_content_bottom = SC_H - 96;
    g_row_gap = g_row_gap_at = 0;
    if (g_sc_open == TAB_INPUT) {
        g_row_gap = SC_BIND_GAP;
        g_row_gap_at = BIND_HEAD_ROWS - g_bind_top > 0 ? BIND_HEAD_ROWS - g_bind_top : 0;
    }
}

/* The Graphics panel's ADVANCED, and Advanced's way back: over the rows at
 * their left. */
static ThemeRect sc_advanced_rect(void) { return sc_rect(HX_PANEL_X - 8, 70, 190, 34); }

static void sc_row_bg(ThemeRect rr, int foc, int last)
{
    if (foc) {
        double at[2] = { 0, 1 }, a[2] = { 0, 0.66 };
        uint32_t c[2] = { hx_wash(), hx_wash() };

        theme_hgrad(sc_rect(rr.x - 40, rr.y - 2, rr.w + 80, rr.h + 4), 2, at, c, a);
        theme_fill(sc_rect(rr.x + rr.w + 36, rr.y - 2, 4, rr.h + 4), hx_bright(), 1);
    }
    if (!last)
        theme_fill(sc_rect(rr.x, rr.y + rr.h + 2, rr.w, 1), 0xFFFFFF, 0.07);
}

/* The chevrons a value sits between, where launcher.c's hit_arrow looks. */
static void sc_row_arrows(ThemeRect rr, int can_left, int can_right, int foc)
{
    double cy = rr.y + rr.h / 2.0;

    theme_chevron(rr.x + rr.w - 315, cy, 0, 7, 13,
                  can_left ? (foc ? 0xFFFFFF : hx_accent()) : 0x56647C, can_left ? 1.0 : 0.5);
    theme_chevron(rr.x + rr.w - 19, cy, 1, 7, 13,
                  can_right ? (foc ? 0xFFFFFF : hx_accent()) : 0x56647C, can_right ? 1.0 : 0.5);
}

static void sc_row_label(ThemeRect rr, const char *label, int foc)
{
    hx_caps(sc_rect(rr.x + 2, rr.y, 300, rr.h), label, 13, foc ? 0xFFFFFF : 0xBFCADC, THEME_LEFT, 2,
            THEME_FACE_CONDENSED_BOLD, 1);
}

static void sc_row_value(ThemeRect rr, const char *value, int dim)
{
    theme_text_ex(sc_rect(rr.x + rr.w - 300, rr.y, 262, rr.h), value, 13, 600,
                  dim ? sc_c(0x7C8AA4) : sc_c(0xE6ECF6), THEME_CENTRE, 0, THEME_FACE_CONDENSED, 1);
}

static int sc_draw_rows(const Row *rows, int count, int wrap_help)
{
    char value[160];
    int i;
    ThemeRect r;

    for (i = 0; i < count; i++) {
        const Row *row = &rows[i];
        ThemeRect rr = row_rect(i);
        int foc = i == g_sel && !g_sc_adv_focus, dim_value = 0;

        sc_row_bg(rr, foc, i + 1 == count);
        sc_row_label(rr, row->label, foc);
        row_value(row, value, sizeof value);
        if (row->kind == ROW_TEXT) {
            if (foc) {
                if ((int)(g_pulse * 6.0) % 2 == 0)
                    strncat(value, "_", sizeof value - strlen(value) - 1);
            } else if (!value[0] && row->empty) {
                snprintf(value, sizeof value, "%s", row->empty);
                dim_value = 1;
            }
        }
        sc_row_value(rr, value, dim_value);
        if (row_can(row, -1) || row_can(row, 1))
            sc_row_arrows(rr, row_can(row, -1), row_can(row, 1), foc);
    }
    r = sc_rect(g_row_x + 2, g_row_y + count * g_row_pitch + 8, g_row_w - 4, 44);
    if (wrap_help && count == 1)
        return r.y;
    if (g_sel < count && !g_sc_adv_focus)
        theme_text_wrapped(r, row_help(&rows[g_sel]), 10, sc_c(0x9AA8C0));
    return r.y + r.h;
}

static int sc_bind_col_x(ThemeRect rr, int col) { return rr.x + rr.w - (col ? 169 : 300); }

static void sc_draw_input(void)
{
    BindPort *bp = &g_bind.port[g_bind_port];
    char line[160];
    int i;

    for (i = 0; i < BIND_ROWS_VISIBLE; i++) {
        int k = g_bind_top + i, foc = i == g_sel;
        ThemeRect rr = row_rect(i), cv;
        const char *label = NULL, *val = NULL;

        if (k >= BIND_TOTAL_ROWS)
            break;
        sc_row_bg(rr, foc, i + 1 == BIND_ROWS_VISIBLE);
        if (k == BIND_HEAD_ROWS || (i == 0 && k > BIND_HEAD_ROWS)) {
            hx_caps(sc_rect(sc_bind_col_x(rr, 0), rr.y - 22, SC_BIND_COL_W, 16), "CONTROLLER", 8,
                    g_bind_col == 0 ? hx_accent() : 0x8494AE, THEME_CENTRE, 2, THEME_FACE_DISPLAY, 0);
            hx_caps(sc_rect(sc_bind_col_x(rr, 1), rr.y - 22, SC_BIND_COL_W, 16), "KEYS & MOUSE", 8,
                    g_bind_col == 1 ? hx_accent() : 0x8494AE, THEME_CENTRE, 2, THEME_FACE_DISPLAY, 0);
        }
        if (k == BIND_ROW_PORT) {
            snprintf(line, sizeof line, "Controller %d", g_bind_port + 1);
            label = "Editing"; val = line;
        } else if (k == BIND_ROW_DEVICE) {
            if (bp->device == BIND_DEV_PAD) {
                char name[64];

                recomp_pad_name(bp->pad, name, sizeof name);
                snprintf(line, sizeof line, name[0] ? "Gamepad %d (%s)" : "Gamepad %d (not plugged in)",
                         bp->pad + 1, name);
            } else {
                snprintf(line, sizeof line, "%s", bp->device == BIND_DEV_KEYBOARD ? "Keyboard" : "Nothing");
            }
            label = "Read from"; val = line;
        } else if (k == BIND_ROW_API) {
            label = "Controllers";
            val = g_bind.pad_api == BIND_PAD_API_XINPUT ? "Xbox only (XInput)" : "All controllers (SDL3)";
        } else if (k == BIND_ROW_MOUSE) {
            label = "Mouse look"; val = g_settings.mouse_look ? "On" : "Off";
        } else if (k == BIND_ROW_SENS) {
            snprintf(line, sizeof line, "%d%s", g_settings.mouse_sensitivity,
                     g_settings.mouse_sensitivity == RECOMP_MOUSE_SENSITIVITY_DEFAULT ? "  (default)" : "");
            label = "Mouse sensitivity"; val = line;
        } else if (k == BIND_ROW_RESET) {
            label = "Reset this controller"; val = "Back to the defaults";
        }
        if (k == BIND_ROW_RESET) {
            sc_row_label(rr, label, foc);
            hx_caps(sc_rect(rr.x + rr.w - 300, rr.y, 292, rr.h), "BACK TO THE DEFAULTS", 10,
                    foc ? 0xFFFFFF : hx_accent(), THEME_RIGHT, 2, THEME_FACE_CONDENSED, 1);
            continue;
        }
        if (label) {
            sc_row_label(rr, label, foc);
            sc_row_value(rr, val, 0);
            if (k == BIND_ROW_SENS)
                sc_row_arrows(rr, g_settings.mouse_sensitivity > RECOMP_MOUSE_SENSITIVITY_MIN,
                              g_settings.mouse_sensitivity < RECOMP_MOUSE_SENSITIVITY_MAX, foc);
            else if (k < BIND_ROW_RESET)
                sc_row_arrows(rr, 1, 1, foc);
            continue;
        }
        {
            int c = k - BIND_HEAD_ROWS;

            sc_row_label(rr, bind_control_labels[c], foc);
            cv = sc_rect(sc_bind_col_x(rr, 0) + 4, rr.y, SC_BIND_COL_W - 8, rr.h);
            theme_text_ex(cv, (g_capturing && foc && g_bind_col == 0) ? "press" SC_ELL
                                                                     : bind_source_label(bp->pad_src[c]),
                          12, 600, foc && g_bind_col == 0 ? sc_c(hx_bright()) : sc_c(0xE6ECF6),
                          THEME_CENTRE, 0, THEME_FACE_CONDENSED, 1);
            cv.x = sc_bind_col_x(rr, 1) + 4;
            theme_text_ex(cv, (g_capturing && foc && g_bind_col == 1) ? "press" SC_ELL
                                                                     : bind_source_label(bp->key_src[c]),
                          12, 600, foc && g_bind_col == 1 ? sc_c(hx_bright()) : sc_c(0xE6ECF6),
                          THEME_CENTRE, 0, THEME_FACE_CONDENSED, 1);
        }
    }
    {   /* Where we are in the list: a track and a thumb. */
        ThemeRect t = row_rect(0), th;
        int span = row_rect(BIND_ROWS_VISIBLE - 1).y + g_row_h - t.y;

        t.x = g_row_x + g_row_w + 22; t.w = 2; t.h = span;
        theme_fill(t, 0xFFFFFF, 0.12);
        th = t;
        th.h = span * BIND_ROWS_VISIBLE / BIND_TOTAL_ROWS;
        th.y = t.y + (span - th.h) * g_bind_top / (BIND_TOTAL_ROWS - BIND_ROWS_VISIBLE);
        theme_fill(th, hx_accent(), 1);
    }
    {
        int k = g_bind_top + g_sel;
        ThemeRect r = sc_rect(g_row_x + 2, row_rect(BIND_ROWS_VISIBLE - 1).y + g_row_pitch + 6,
                              g_row_w - 4, 44);

        theme_text_wrapped(r,
            g_capturing ? (g_bind_col == 1 ? "Press a key, or click any mouse button. Escape cancels."
                                           : "Press what you want this to be. Escape cancels.")
            : k == BIND_ROW_PORT   ? "Which of the four controllers these bindings are for."
            : k == BIND_ROW_DEVICE ? "What this controller reads: a gamepad, the keyboard, or nothing."
            : k == BIND_ROW_API    ? "All controllers reads Xbox, PlayStation, Switch and most other pads; "
                                     "Xbox only is the older XInput path, for a pad that misbehaves."
            : k == BIND_ROW_MOUSE  ? "Moving the mouse turns your view. Click in the game to give it the pointer."
            : k == BIND_ROW_SENS   ? "How far a movement of the mouse turns you. Enter goes back to the default."
            : k == BIND_ROW_RESET  ? "Put this controller back to the built-in mapping."
            : "Left and right choose the column; Enter rebinds. Bindings are shared by every game.",
            10, sc_c(0x9AA8C0));
    }
}

static void draw_tab_body(void);          /* launcher.c: a tab's rows */

/* The panel has a link over its rows: Graphics' Advanced (and its way back),
 * Play Together's page on setting up a server. */
static int hx_panel_link(void)
{
    return (g_sc_open == TAB_VIDEO && sc_has_advanced()) || g_sc_open == TAB_LINK
#ifdef LAUNCHER_GAMES
        || g_sc_open == TAB_ADVANCED
#endif
        ;
}

static void hx_panel(void)
{
    static const char *const titles[] = { "GRAPHICS", "CONTROLS", "PLAY TOGETHER", "ABOUT", "ADVANCED" };
    char kicker[96];
    HxHint hints[3];
    int nh = 0;

    hx_background(0.22);
    hx_scrims(260, 0.95);
    hx_caps(sc_rect(HX_R - 600, 26, 600, 44), titles[g_sc_open < 5 ? g_sc_open : 3], 28, 0xFFFFFF,
            THEME_RIGHT, 7, THEME_FACE_HEAVY, 1);
#ifdef LAUNCHER_GAMES
    snprintf(kicker, sizeof kicker, "FOR %s", launcher_games[g_game].card_name);
#else
    snprintf(kicker, sizeof kicker, "FOR %s", g_title_name);
#endif
    hx_caps(sc_rect(HX_R - 600, 74, 600, 20), kicker, 9, hx_accent(), THEME_RIGHT, 4,
            THEME_FACE_DISPLAY, 1);
    if (hx_panel_link()) {
        ThemeRect a = sc_advanced_rect();
        int hot = g_sc_hover == SC_ADVANCED || g_sc_hover == SC_BACK || g_sc_hover == SC_HELP;
        int foc = g_sc_adv_focus;
        int back = g_sc_open != TAB_VIDEO && g_sc_open != TAB_LINK;

        if (foc) {
            double at[2] = { 0, 1 }, al[2] = { 0.66, 0 };
            uint32_t c[2] = { hx_wash(), hx_wash() };

            theme_hgrad(a, 2, at, c, al);
            theme_fill(sc_rect(a.x - 4, a.y, 4, a.h), hx_bright(), 1);
        }
        theme_chevron(back ? a.x + 12 : a.x + a.w - 16, a.y + a.h / 2.0, !back, 5, 10,
                      hot || foc ? 0xFFFFFF : hx_accent(), 1.0);
        hx_caps(sc_rect(back ? a.x + 26 : a.x + 8, a.y, a.w - 34, a.h),
                back ? "GRAPHICS" : g_sc_open == TAB_LINK ? "SET UP A SERVER" : "ADVANCED",
                10, hot || foc ? 0xFFFFFF : 0xBFCADC, THEME_LEFT, 3, THEME_FACE_CONDENSED_BOLD, 1);
    }
    sc_rows_in_panel();
    draw_tab_body();
    hints[nh].glyph = -1; hints[nh].key = "\xe2\x86\x90 \xe2\x86\x92"; hints[nh].pad_key = "D-PAD";
    hints[nh].label = "Change"; hints[nh++].code = SC_NONE;
    if (g_sc_open == TAB_INPUT) {
        hints[nh].glyph = HX_G_A; hints[nh].key = "ENTER"; hints[nh].pad_key = NULL;
        hints[nh].label = "Rebind"; hints[nh++].code = SC_NONE;
    }
    hints[nh].glyph = HX_G_B; hints[nh].key = "ESC"; hints[nh].pad_key = NULL;
    hints[nh].label = "Back"; hints[nh++].code = SC_BACK;
    hx_hints(hints, nh);
}

/* ------------------------------------------------------------- the extras */

static void hx_extras_draw(void)
{
    int idx[EXTRAS_MAX], n, i, s;
    char line[160];
    HxHint hints[4];
    int nh = 0;
    uint64_t now = lhost_ticks();

    if (g_ex_game != g_game)
        hx_extras_scan();
    n = hx_extras_shown(idx);
    if (g_ex_sel >= n) g_ex_sel = n ? n - 1 : 0;
    hx_background(0.5);
    theme_fill(sc_rect(0, 0, SC_W, SC_H), THEME_INK_RGB, 0.3);
    hx_scrims(0, 0.5);
    hx_caps(sc_rect(EX_X0, 30, 600, 50), "EXTRAS", 28, 0xFFFFFF, THEME_LEFT, 7, THEME_FACE_HEAVY, 1);
#ifdef LAUNCHER_GAMES
    snprintf(line, sizeof line, "%s" SC_DOT "MANUALS, VIDEOS AND PICTURES", launcher_games[g_game].card_name);
#else
    snprintf(line, sizeof line, "%s" SC_DOT "MANUALS, VIDEOS AND PICTURES", g_title_name);
#endif
    hx_caps(sc_rect(EX_X0, 82, 900, 20), line, 9, hx_accent(), THEME_LEFT, 4, THEME_FACE_DISPLAY, 1);
    hx_logo(sc_rect(HX_R - 300 + 8, 22, 300, 96), THEME_RIGHT, 0.9);

    if (!g_ex_count) {
        char dir[1024];

#ifdef LAUNCHER_GAMES
        if (!extras_dir(launcher_games[g_game].title_id, dir, sizeof dir, 0))
#else
        if (!extras_dir(g_title_id, dir, sizeof dir, 0))
#endif
            dir[0] = '\0';
        theme_text_ex(sc_rect(EX_X0, 190, 1000, 40), "Nothing here yet.", 20, 600, sc_c(0xE6ECF6),
                      THEME_LEFT, 0, THEME_FACE_DISPLAY, 1);
        hx_text(sc_rect(EX_X0, 240, 1100, 26),
                "Manuals (PDF), videos, pictures and links (.url) put in this game's Extras folder show up here:",
                12, 400, 0xAEBBD0, THEME_LEFT);
        theme_text_ex(sc_rect(EX_X0, 274, 1150, 28), dir, 13, 600, sc_c(hx_bright()), THEME_LEFT, 0,
                      THEME_FACE_BODY, 1);
        hx_text(sc_rect(EX_X0, 312, 1100, 26),
                "An extras.json beside them can name, order and sort them, give each a picture, and add links.",
                12, 400, 0xAEBBD0, THEME_LEFT);
        hints[nh].glyph = HX_G_Y; hints[nh].key = "O"; hints[nh].pad_key = NULL;
        hints[nh].label = "Open folder"; hints[nh++].code = SC_FOLDER;
        hints[nh].glyph = HX_G_B; hints[nh].key = "ESC"; hints[nh].pad_key = NULL;
        hints[nh].label = "Back"; hints[nh++].code = SC_BACK;
        hx_hints(hints, nh);
        return;
    }

    /* The sections: ALL, then each one found. */
    for (s = 0; s <= g_ex_nsec; s++) {
        ThemeRect sr = hx_section_rect(s);
        int on = s == g_ex_sec, hot = g_sc_hover == SC_SECTION + s;

        hx_caps(sc_rect(sr.x + 8, sr.y, sr.w, sr.h), s ? g_ex_sections[s - 1] : "ALL", 11,
                on || hot ? 0xFFFFFF : 0x96A6C0, THEME_LEFT, 3, THEME_FACE_CONDENSED_BOLD, 1);
        if (on)
            theme_fill(sc_rect(sr.x + 8, sr.y + sr.h - 4, sr.w - 24, 3), hx_bright(), 1);
    }

    /* The tiles, a row scrolled into view as the focus reaches it. */
    if (g_ex_sel / EX_COLS < g_ex_top)
        g_ex_top = g_ex_sel / EX_COLS;
    if (g_ex_sel / EX_COLS >= g_ex_top + EX_ROWS)
        g_ex_top = g_ex_sel / EX_COLS - EX_ROWS + 1;
    hx_extras_thumbs(idx, n);
    for (i = g_ex_top * EX_COLS; i < n && i < (g_ex_top + EX_ROWS) * EX_COLS; i++) {
        const Extra *e = &g_ex[idx[i]];
        ThemeRect tr = hx_tile_rect(i);
        int on = i == g_ex_sel, hot = g_sc_hover == SC_TILE + i;

        theme_fill(tr, 0x000000, 0.6);
        if (g_ex_thumb[idx[i]].px)
            theme_thumb_draw(&g_ex_thumb[idx[i]], tr.x, tr.y, on ? 1.0 : hot ? 0.9 : 0.62);
        if (on) {
            theme_outline(sc_rect(tr.x - 3, tr.y - 3, tr.w + 6, tr.h + 6), hx_bright(), 1, 3);
        }
        hx_caps(sc_rect(tr.x, tr.y + tr.h + 8, tr.w, 24), e->title, 13, on ? 0xFFFFFF : 0xBFCADC,
                THEME_LEFT, 2, THEME_FACE_CONDENSED_BOLD, 1);
        snprintf(line, sizeof line, "%s" SC_DOT "%s", e->section, extras_kind_name(e->kind));
        hx_caps(sc_rect(tr.x, tr.y + tr.h + 32, tr.w, 18), line, 8, on ? hx_bright() : 0x8494AE,
                THEME_LEFT, 2, THEME_FACE_DISPLAY, 1);
    }
    if ((g_ex_top + EX_ROWS) * EX_COLS < n)
        theme_chevron(SC_W / 2.0, EX_Y0 + EX_ROWS * EX_PITCH - 8, 1, 6, 12, hx_accent(), 0.8 + 0.2 * (now % 1000 < 500));
    if (g_ex_sel < n) {
        const Extra *e = &g_ex[idx[g_ex_sel]];
        const char *about = e->about[0] ? e->about
            : e->kind == EXTRA_MANUAL  ? "Opens in your PDF reader."
            : e->kind == EXTRA_VIDEO   ? "Plays in your video player."
            : e->kind == EXTRA_PICTURE ? "Opens in your picture viewer."
            : e->kind == EXTRA_LINK    ? "Opens in your web browser."
            : e->kind == EXTRA_FOLDER  ? "Opens the folder."
            : "Opens with the program your system has for it.";

        hx_text(sc_rect(EX_X0, SC_H - 50, 580, 28), about, 12, 400, 0xC6D0E2, THEME_LEFT);
    }
    hints[nh].glyph = HX_G_A; hints[nh].key = "ENTER"; hints[nh].pad_key = NULL;
    hints[nh].label = "Open"; hints[nh++].code = SC_OPEN;
    hints[nh].glyph = HX_G_Y; hints[nh].key = "O"; hints[nh].pad_key = NULL;
    hints[nh].label = "Open folder"; hints[nh++].code = SC_FOLDER;
    if (g_ex_nsec > 1) {
        hints[nh].glyph = -1; hints[nh].key = "TAB"; hints[nh].pad_key = "LB RB";
        hints[nh].label = "Section"; hints[nh++].code = SC_NONE;
    }
    hints[nh].glyph = HX_G_B; hints[nh].key = "ESC"; hints[nh].pad_key = NULL;
    hints[nh].label = "Back"; hints[nh++].code = SC_BACK;
    hx_hints(hints, nh);
}

/* ------------------------------------------------- the first-run screens */

static ThemeRect sc_play_rect(void) { return sc_rect(SC_W - 60 - 200, SC_H - 68, 200, 46); }
static ThemeRect sc_quit_rect(void) { return sc_rect(SC_W - 60 - 200 - 20 - 150, SC_H - 68, 150, 46); }

/* A button: the main one washed in the game's colour, the other dark. */
static void sc_button_ex(ThemeRect r, const char *title, int primary, int hot, int down, int off)
{
    if (primary && !off) {
        double at[2] = { 0, 1 }, a[2] = { down ? 0.95 : hot ? 0.9 : 0.75, down ? 0.95 : hot ? 0.9 : 0.75 };
        uint32_t c[2] = { hx_wash(), hx_accent() };

        theme_fill(r, THEME_INK_RGB, 0.9);
        theme_hgrad(r, 2, at, c, a);
    } else {
        theme_fill(r, THEME_INK_RGB, down ? 0.95 : 0.82);
    }
    theme_outline(r, hot && !off ? hx_bright() : primary && !off ? hx_accent() : 0x3A4660, off ? 0.5 : 1, 1);
    hx_caps(r, title, primary ? 13 : 11, off ? 0x6E7A90 : 0xFFFFFF, THEME_CENTRE, 5,
            THEME_FACE_CONDENSED_BOLD, 1);
}

static void sc_box(ThemeRect r, double fill, int marks)
{
    (void)marks;
    theme_fill(r, THEME_INK_RGB, fill);
    theme_outline(r, 0x2C3650, 0.9, 1);
}

static void sc_footer_line(const char *hint)
{
    theme_fill(sc_rect(60, SC_H - 92, SC_W - 120, 1), 0xFFFFFF, 0.1);
    theme_text_ex(sc_rect(60, SC_H - 66, 600, 40), hint, 9, 400, sc_c(0x8494AE), THEME_LEFT, 0,
                  THEME_FACE_BODY, 0);
}

/* Over the first-run and update screens: the picture, darker, the logo
 * across the top, the project's name and what it is under it. */
static void hx_setup_header(void)
{
    hx_background(0.62);
    hx_logo(sc_rect(SC_W / 2 - 230, 12, 460, 108), THEME_CENTRE, 1);
    theme_text_ex(sc_rect(0, 124, SC_W, 28), THEME_SC_SUBTITLE, 15, 700, sc_c(0xEBF0FA), THEME_CENTRE,
                  14, THEME_FACE_HEAVY, 1);
    hx_caps(sc_rect(0, 152, SC_W, 18), SC_TAGLINE, 8, hx_accent(), THEME_CENTRE, 4, THEME_FACE_DISPLAY, 1);
}

static ThemeRect sc_setup_low(void) { return sc_rect(60, SETUP_LOW_Y, SC_W - 120, SETUP_LOW_H); }

static void sc_setup(void)
{
    ThemeRect p = sc_rect(60, SETUP_DESC_Y, SC_W - 120, SETUP_DESC_H), r;
    int i;

    sc_box(p, 0.9, 0);
    hx_caps(sc_rect(p.x + 24, p.y + 16, p.w - 48, 22),
            g_rel_moved ? "The game's files have moved" : g_rel_stale ? "Build the game again"
                                                        : "Your copy of the game", 11,
            hx_accent(), THEME_LEFT, 4, THEME_FACE_DISPLAY, 0);
    theme_text_wrapped(sc_rect(p.x + 24, p.y + 46, p.w - 48, p.h - 56), g_rel_moved
        ? "The game is built, but the folder it was built from is no longer where it was. "
          "Choose where it is now, or the disc image: it is linked again in a few seconds, "
          "nothing rebuilt, your saves kept."
        : g_rel_stale
        ? "This release is newer than the game it built for you. Build it again from the same "
          "disc to bring it up to date; your saves and settings stay."
        : "The game is built once from your own copy, here on this computer, in a minute or two. "
          "A disc image is extracted to your home folder; a game folder is linked, never changed.",
        11, THEME_TEXT);

    r = setup_rect(0);
    sc_box(r, 0.9, 0);
    if (g_setup_sel == 0)
        theme_outline(r, hx_accent(), 1, 1);
    r.x += 24; r.w -= 48;
    if (g_source[0]) {
        char shown[LAUNCHER_PATH + 4];

        snprintf(shown, sizeof shown, "%s%s", g_source,
                 g_setup_sel == 0 && ((int)(g_pulse * 4) & 1) ? "_" : "");
        theme_text_ex(r, shown, 11, 600, sc_c(THEME_TEXT_RGB), THEME_LEFT, 0, THEME_FACE_BODY, 0);
    } else {
        theme_text_ex(r, "Type or paste a path", 11, 400, sc_c(THEME_TEXT_DIM_RGB), THEME_LEFT, 0,
                      THEME_FACE_BODY, 0);
    }
    for (i = 1; i <= 3; i++) {
        static const char *const lb[] = { "", "DISC IMAGE", "FOLDER", "BUILD" };   /* no ellipsis in the face */
        const char *label = (i == 3 && g_rel_moved) ? "LINK AGAIN"
                          : (i == 3 && g_rel_stale) ? "BUILD AGAIN" : lb[i];

        sc_button_ex(setup_rect(i), label, i == 3,
                     g_setup_sel == i || (i == 3 && g_sc_hover == SC_PLAY), 0, i == 3 && !g_source[0]);
    }
    sc_button_ex(sc_quit_rect(), "QUIT", 0, g_sc_hover == SC_QUIT, 0, 0);

    r = sc_setup_low();
    if (g_rel_error[0]) {
        sc_box(r, 0.9, 1);
        hx_caps(sc_rect(r.x + 24, r.y + 14, r.w - 48, 22),
                g_rel_unsupported ? "This disc can't be used"
                : g_rel_relinking ? "The game's files were not linked" : "The build did not finish",
                11, hx_accent(), THEME_LEFT, 4, THEME_FACE_DISPLAY, 0);
        theme_text_wrapped(sc_rect(r.x + 24, r.y + 42, r.w - 48, r.h - 48), g_rel_error, 10, THEME_TEXT);
    } else if (g_rel_note[0]) {
        theme_text_wrapped(sc_rect(r.x, r.y, r.w, 40), g_rel_note, 11, THEME_TEXT_DIM);
    } else {
        theme_fill(r, THEME_INK_RGB, 0.82);
        theme_outline(r, hx_accent(), 0.7, 1);
        hx_caps(sc_rect(r.x, r.y + r.h / 2 - 30, r.w, 30), "DROP THE GAME HERE", 15, THEME_TEXT_RGB,
                THEME_CENTRE, 6, THEME_FACE_HEAVY, 1);
        theme_text_ex(sc_rect(r.x, r.y + r.h / 2 + 4, r.w, 22),
                      "A disc image (.iso, .xiso), or the folder with default.xbe in it" SC_DOT
                      "USA or PAL", 10, 400, sc_c(THEME_TEXT_DIM_RGB), THEME_CENTRE, 0,
                      THEME_FACE_BODY, 0);
    }
    sc_footer_line(g_pad_seen ? "A chooses" SC_DOT "B quits"
                              : "Enter chooses" SC_DOT "Esc quits" SC_DOT "drop the game anywhere on the window");
}

static void sc_progress(int done)
{
    ThemeRect p = sc_rect(60, SETUP_PANEL_Y, SC_W - 120, 300), r, b;
    char pct[16], text[LAUNCHER_PATH + 400];

    sc_box(p, 0.9, 1);
    hx_caps(sc_rect(p.x + 30, p.y + 24, p.w - 60, 24),
            done ? "The game is ready" : g_rel_relinking ? "Finding the game's files" : "Building the game",
            14, done ? hx_accent() : THEME_TEXT_RGB, THEME_LEFT, 5, THEME_FACE_DISPLAY, 0);
    r = sc_rect(p.x + 30, p.y + 72, p.w - 60, 24);
    theme_text_ex(r, done ? (g_rel_relinking ? "Files found" : "Build complete") : g_build_label,
                  12, 600, sc_c(done ? THEME_TEXT_RGB : hx_accent()), THEME_LEFT, 0, THEME_FACE_BODY, 0);
    snprintf(pct, sizeof pct, "%d%%", done ? 100 : (int)(g_build_progress * 100.0 + 0.5));
    theme_text_ex(r, pct, 12, 600, sc_c(THEME_TEXT_RGB), THEME_RIGHT, 0, THEME_FACE_BODY, 0);
    b = sc_rect(r.x, r.y + 38, r.w, 14);
    theme_bar(b, done ? 1.0 : g_build_progress, done ? -1.0 : g_pulse * 2.0);
    if (done) {
        unsigned secs = (unsigned)((g_build_ms + 500) / 1000);

        if (g_rel_relinking)
            snprintf(text, sizeof text, "%s is linked to its files again. Nothing was rebuilt, and "
                     "your saves are where they were.", g_title_name);
        else
            snprintf(text, sizeof text, "%s was built from your copy in %u min %u s. Next time "
                     "the launcher opens straight to Play.", g_title_name, secs / 60, secs % 60);
        theme_text_wrapped(sc_rect(r.x, b.y + 34, r.w, 90), text, 11, THEME_TEXT);
    } else {
        theme_text_ex(sc_rect(r.x, b.y + 30, r.w, 20), g_build_detail, 10, 400,
                      sc_c(THEME_TEXT_DIM_RGB), THEME_LEFT, 0, THEME_FACE_BODY, 0);
        theme_text_wrapped(sc_rect(r.x, b.y + 64, r.w, 70), g_rel_relinking
            ? "Linking the game to its files again. Nothing is rebuilt."
            : "This happens once. Later starts go straight to the game. It is building from "
              "your own copy, on this computer.", 11, THEME_TEXT_DIM);
    }
    sc_footer_line(done ? (g_pad_seen ? "A carries on" : "Enter carries on")
                        : (g_pad_seen ? "B cancels" : "Esc cancels"));
    sc_button_ex(sc_play_rect(), done ? "OK" : "CANCEL", done, 0, 0, 0);
}

static int sc_update_shown(void)
{
    return g_upd == UPD_RUNNING || g_upd == UPD_DONE || g_upd == UPD_FAILED;
}

static void sc_update_screen(void)
{
    ThemeRect p = sc_rect(60, SETUP_PANEL_Y, SC_W - 120, 300), r, b;
    int done = g_upd == UPD_DONE, failed = g_upd == UPD_FAILED;
    char title[120], pct[16], text[600];

    sc_box(p, 0.9, 1);
    snprintf(title, sizeof title, done ? "Updated to %s" : failed ? "The update did not finish"
                                                                  : "Updating to %s", g_upd_tag);
    hx_caps(sc_rect(p.x + 30, p.y + 24, p.w - 60, 24), title, 14,
            done ? hx_accent() : THEME_TEXT_RGB, THEME_LEFT, 5, THEME_FACE_DISPLAY, 0);
    r = sc_rect(p.x + 30, p.y + 72, p.w - 60, 24);
    theme_text_ex(r, failed ? "Stopped" : g_upd_phase, 12, 600,
                  sc_c(failed ? THEME_TEXT_RGB : hx_accent()), THEME_LEFT, 0, THEME_FACE_BODY, 0);
    if (!failed) {
        snprintf(pct, sizeof pct, "%d%%", (int)(g_upd_progress * 100.0 + 0.5));
        theme_text_ex(r, pct, 12, 600, sc_c(THEME_TEXT_RGB), THEME_RIGHT, 0, THEME_FACE_BODY, 0);
    }
    b = sc_rect(r.x, r.y + 38, r.w, 14);
    theme_bar(b, g_upd_progress, done || failed ? -1.0 : g_pulse * 2.0);
    if (done) {
        snprintf(text, sizeof text, "%s %s is installed, built from your own disc. Restart the "
                 "launcher to use it; your saves and settings are where they were.",
                 launcher_title(), g_upd_tag);
        theme_text_wrapped(sc_rect(r.x, b.y + 34, r.w, 90), text, 11, THEME_TEXT);
    } else if (failed) {
        size_t e = strlen(g_upd_error);
        snprintf(text, sizeof text, "%s%s Nothing of the installed game was replaced unless it "
                 "says it was installing. The log is update.log beside the launcher.", g_upd_error,
                 e && g_upd_error[e - 1] != '.' ? "." : "");
        theme_text_wrapped(sc_rect(r.x, b.y + 34, r.w, 110), text, 11, THEME_TEXT);
    } else {
        theme_text_ex(sc_rect(r.x, b.y + 30, r.w, 20), g_upd_detail, 10, 400,
                      sc_c(THEME_TEXT_DIM_RGB), THEME_LEFT, 0, THEME_FACE_BODY, 0);
        theme_text_wrapped(sc_rect(r.x, b.y + 64, r.w, 70),
            "It moves to the new release and builds the game again from your own copy, on "
            "this computer. The game you have stays until the last step.", 11, THEME_TEXT_DIM);
    }
    sc_footer_line(done ? (g_pad_seen ? "A restarts" : "Enter restarts")
                 : failed ? (g_pad_seen ? "A goes back" : "Enter goes back")
                 : (g_pad_seen ? "B cancels" : "Esc cancels"));
    sc_button_ex(sc_play_rect(), done ? "RESTART" : failed ? "OK" : "CANCEL", done, 0, 0, 0);
}

static void sc_update_input(int accept, int cancel)
{
    if (g_upd == UPD_RUNNING) {
        if (cancel || accept) {
            g_upd_cancelled = 1;
            lhost_job_cancel();
        }
    } else if (g_upd == UPD_DONE) {
        if (accept)
            upd_restart();
        else if (cancel)
            g_upd = UPD_CURRENT;
    } else if (accept || cancel) {
        upd_check();
    }
}

/* The whole window. 1 when drawn (it always is in this layout). */
static int sc_draw(void)
{
    if (sc_update_shown()) {
        hx_setup_header();
        sc_update_screen();
        return 1;
    }
    if (g_release && g_rel != REL_READY) {
        hx_setup_header();
        if (g_rel == REL_BUILDING)
            sc_progress(0);
        else if (g_rel == REL_DONE)
            sc_progress(1);
        else
            sc_setup();
        return 1;
    }
    if (g_hx_help)
        hx_help_draw();
    else if (g_hx_extras)
        hx_extras_draw();
    else if (g_sc_open >= 0)
        hx_panel();
    else
        hx_home();
    return 1;
}

/* ----------------------------------------------------------------- input */

static void hx_open_panel(Tab t)
{
    g_sc_open = t;
    set_tab(t);
    g_sel = 0;
    g_sc_adv_focus = 0;
    sc_rows_in_panel();
}

static void sc_open_advanced(int open)
{
#ifdef LAUNCHER_GAMES
    g_sc_open = open ? TAB_ADVANCED : TAB_VIDEO;
    set_tab(open ? TAB_ADVANCED : TAB_VIDEO);
    g_sel = 0;
    g_sc_adv_focus = !open;
    sc_rows_in_panel();
#else
    (void)open;
#endif
}

static void sc_close(void)
{
#ifdef LAUNCHER_GAMES
    if (g_sc_open == TAB_ADVANCED) {
        sc_open_advanced(0);
        return;
    }
#endif
    g_sc_adv_focus = 0;
    g_sc_open = -1;
    g_capturing = 0;
    settings_save();
}

static void hx_switch_game(int dir)
{
#ifdef LAUNCHER_GAMES
    int i = hx_other_game(dir);

    if (i >= 0) {
        if (g_hx_extras)
            g_ex_game = -1;                     /* the other game's extras, at the next draw */
        game_pick(i);
    }
#else
    (void)dir;
#endif
}

static void hx_activate(int kind)
{
    switch (kind) {
    case HX_PLAY:     do_play(); break;
    case HX_GRAPHICS: hx_open_panel(TAB_VIDEO); break;
    case HX_CONTROLS: hx_open_panel(TAB_INPUT); break;
    case HX_ONLINE:   hx_open_panel(TAB_LINK); break;
    case HX_SAVES:    sc_open_saves(); break;
    case HX_EXTRAS:   hx_extras_show(1); break;
    case HX_UPDATES:  upd_activate(); break;
    case HX_QUIT:     lhost_close(); break;
    default: break;
    }
}

static int hx_extras_nav(int dx, int dy, int accept, int cancel, int tabdelta)
{
    int idx[EXTRAS_MAX], n = hx_extras_shown(idx);

    if (cancel) {
        hx_extras_show(0);
        return 1;
    }
    if (tabdelta && g_ex_nsec > 0) {
        g_ex_sec = (g_ex_sec + tabdelta + g_ex_nsec + 1) % (g_ex_nsec + 1);
        g_ex_sel = g_ex_top = 0;
        return 1;
    }
    if (n) {
        int s = g_ex_sel + dx + dy * EX_COLS;

        if (dy && (s < 0 || s >= n))
            s = dy < 0 ? g_ex_sel % EX_COLS : (s >= n ? n - 1 : s);
        if (s < 0) s = 0;
        if (s >= n) s = n - 1;
        g_ex_sel = s;
    }
    if (accept)
        hx_extras_open_item();
    return 1;
}

/* 1 when the layout handled it; 0 sends it on to the plain launcher's (a
 * panel's rows are its rows). */
static int sc_nav(int dx, int dy, int accept, int cancel, int tabdelta)
{
    int items[HX_KINDS], n;

    if (sc_update_shown()) {
        sc_update_input(accept, cancel);
        return 1;
    }
    if (g_hx_help) {
        if (cancel)
            g_hx_help = 0;
        else if (accept)
            hx_help_copy();
        return 1;
    }
    if (g_hx_extras)
        return hx_extras_nav(dx, dy, accept, cancel, tabdelta);
    if (g_sc_open >= 0) {
        if (g_capturing)
            return 0;
        if (hx_panel_link()) {
            if (g_sc_adv_focus) {
                if (accept && g_sc_open == TAB_LINK)
                    g_hx_help = 1;
                else if (accept)
                    sc_open_advanced(g_sc_open == TAB_VIDEO);
                else if (cancel)
                    sc_close();
                else if (dy > 0) {
                    g_sc_adv_focus = 0;
                    g_sel = 0;
                }
                if (accept || cancel || dy || dx)
                    return 1;
            } else if (dy < 0 && g_sel == 0) {
                g_sc_adv_focus = 1;
                return 1;
            }
        }
        if (cancel || (accept && g_tab != TAB_INPUT)) {
            sc_close();
            return 1;
        }
        if (tabdelta) {
            static const Tab order[] = { TAB_VIDEO, TAB_INPUT, TAB_LINK };
            int i, at = 0;

            for (i = 0; i < 3; i++)
                if ((int)order[i] == g_sc_open)
                    at = i;
            hx_open_panel(order[(at + tabdelta + 3) % 3]);
            return 1;
        }
        sc_rows_in_panel();
        return 0;
    }
    n = hx_items(items);
    if (dx || dy)                                   /* along the row, either way */
        g_hx_focus = (g_hx_focus + (dx ? dx : dy) + n) % n;
    if (tabdelta)
        hx_switch_game(tabdelta);
    if (accept)
        hx_activate(items[g_hx_focus < n ? g_hx_focus : 0]);
    if (cancel)
        lhost_close();
    return 1;
}

static int sc_hit(int mx, int my)
{
    int items[HX_KINDS], n, i, h = hx_hint_hit(mx, my);

    if (h != SC_NONE)
        return h;
    if (g_hx_extras) {
        int idx[EXTRAS_MAX], m = hx_extras_shown(idx);

        if (g_ex_count)
            for (i = 0; i <= g_ex_nsec; i++)
                if (sc_in(hx_section_rect(i), mx, my))
                    return SC_SECTION + i;
        for (i = g_ex_top * EX_COLS; i < m && i < (g_ex_top + EX_ROWS) * EX_COLS; i++)
            if (sc_in(hx_tile_rect(i), mx, my))
                return SC_TILE + i;
        return SC_NONE;
    }
    if (g_hx_help)
        return SC_NONE;
    if (g_sc_open >= 0) {
        if (hx_panel_link() && sc_in(sc_advanced_rect(), mx, my))
            return g_sc_open == TAB_VIDEO ? SC_ADVANCED : g_sc_open == TAB_LINK ? SC_HELP : SC_BACK;
        return SC_NONE;
    }
    if (sc_in(g_hx_switch_rect, mx, my))
        return SC_SWITCH;
    n = hx_items(items);
    for (i = 0; i < n; i++)
        if (sc_in(hx_item_rect(i), mx, my))
            return SC_ITEM + i;
    return SC_NONE;
}

static int sc_click(int mx, int my, int dbl)
{
    int h = sc_hit(mx, my), items[HX_KINDS];

    if (sc_update_shown()) {
        if (sc_in(sc_play_rect(), mx, my))
            sc_update_input(1, 0);
        return 1;
    }
    if (g_hx_help) {
        if (h == SC_BACK) g_hx_help = 0;
        else if (h == SC_OPEN) hx_help_copy();
        else if (h == SC_FOLDER) hx_help_folder();
        else if (h == SC_PRICES) lhost_open(HX_FLY_PRICING);
        return 1;
    }
    if (h == SC_HELP) {
        g_hx_help = 1;
        return 1;
    }
    if (h == SC_BACK) {
        if (g_hx_extras)
            hx_extras_show(0);
        else
            sc_close();
        return 1;
    }
    if (h == SC_QUIT)    { lhost_close(); return 1; }
    if (h == SC_FOLDER)  { hx_extras_open_folder(); return 1; }
    if (h == SC_SWITCH)  { hx_switch_game(1); return 1; }
    if (h == SC_ADVANCED) { sc_open_advanced(1); return 1; }
    if (g_hx_extras) {
        uint64_t now = lhost_ticks();

        if (h == SC_OPEN) {
            hx_extras_open_item();
        } else if (h >= SC_SECTION && h <= SC_SECTION + g_ex_nsec) {
            g_ex_sec = h - SC_SECTION;
            g_ex_sel = g_ex_top = 0;
        } else if (h >= SC_TILE) {
            /* A click picks a tile; a second on it opens it. */
            if (g_ex_sel == h - SC_TILE && (dbl || now - g_ex_click_at < 1500))
                hx_extras_open_item();
            g_ex_sel = h - SC_TILE;
            g_ex_click_at = now;
        }
        return 1;
    }
    if (g_sc_open >= 0) {
        if (mx < HX_PANEL_X - 120 && my < SC_H - 96) {     /* outside the column: back */
            sc_close();
            return 1;
        }
        sc_rows_in_panel();
        return 0;                                   /* a panel's rows: the plain launcher's */
    }
    hx_items(items);
    if (h == SC_OPEN) {
        hx_activate(items[g_hx_focus]);
        return 1;
    }
    if (h >= SC_ITEM && h < SC_ITEM + HX_KINDS) {
        g_hx_focus = h - SC_ITEM;
        hx_activate(items[g_hx_focus]);
        return 1;
    }
    return 1;
}

static void sc_hover_at(int mx, int my)
{
    static int first_x = -1, first_y = -1, moved;

    /* The menu follows the pointer once it moves: a pointer that happens
     * to rest over the window as it opens does not take the focus (an
     * Enter then would do what is under it, Quit as likely as Play). */
    if (first_x < 0) {
        first_x = mx;
        first_y = my;
    } else if (abs(mx - first_x) > 3 || abs(my - first_y) > 3) {
        moved = 1;
    }
    g_sc_hover = sc_hit(mx, my);
    if (moved && g_sc_open < 0 && !g_hx_extras && g_sc_hover >= SC_ITEM && g_sc_hover < SC_ITEM + HX_KINDS)
        g_hx_focus = g_sc_hover - SC_ITEM;          /* the menu follows the pointer */
    if (g_sc_open >= 0)
        sc_rows_in_panel();
}

/* Each tick: the controller's Y (Extras' Open folder), which the plain
 * launcher's pad reading does not report. */
static void hx_tick(void)
{
    static uint16_t last;
    RecompPadState st;
    uint16_t b = 0;
    int i;

    for (i = 0; i < RECOMP_PAD_SLOTS; i++)
        if (recomp_pad_read(i, &st)) {
            b = st.buttons;
            break;
        }
    if ((b & RECOMP_PAD_Y) && !(last & RECOMP_PAD_Y) && g_hx_help)
        lhost_open(HX_FLY_PRICING);
    else if ((b & RECOMP_PAD_Y) && !(last & RECOMP_PAD_Y) && g_hx_extras)
        hx_extras_open_folder();
    if ((b & RECOMP_PAD_X) && !(last & RECOMP_PAD_X) && g_hx_help)
        hx_help_folder();
    last = b;
}

/* A key the plain launcher has no use for: O opens the Extras folder. */
static void hx_key(int key)
{
    if (key == 'O' && g_hx_extras)
        hx_extras_open_folder();
    if (key == 'F' && g_hx_help)
        hx_help_folder();
    if (key == 'P' && g_hx_help)
        lhost_open(HX_FLY_PRICING);
}
