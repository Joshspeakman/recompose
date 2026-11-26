/*
 * launcher_showcase.inc -- the showcase layout (recomp_add_launcher SHOWCASE),
 * included by launcher.c, whose rows, settings and screens it draws.
 *
 * The other native ports' launch window (their Mac apps: Homeworld's, Ghost
 * Recon's, TimeSplitters 2's own mac/ts2_launcher.m, whose numbers these
 * are): the game's logo over its art, the project's name under it, a card
 * for each renderer, a row of tiles that say what is set, and Quit / Play.
 * The tabs of the plain launcher become panels the tiles open; their rows
 * are drawn in the Mac's panel style (spaced capitals, the value centred
 * between chevrons, a hairline between rows, the focused one tinted with a
 * bar), and so are the first-run screens. 960 by 660 window units.
 *
 * The pictures are the title's (launcher_art.c): backdrop, logo, glow and
 * two card pictures, by the names its theme header gives; the strings are
 * its theme header's too (THEME_SC_*). Without the pictures the window
 * writes the game's name over a glow of its own.
 */

#define SC_W LAUNCHER_WINDOW_W
#define SC_H LAUNCHER_WINDOW_H

#ifndef THEME_SC_SUBTITLE
#define THEME_SC_SUBTITLE ""
#endif
#ifndef THEME_SC_NAME
#define THEME_SC_NAME g_title_name
#endif
#ifndef THEME_SC_PLACE
#define THEME_SC_PLACE ""
#endif
#ifndef THEME_SC_CARD0_ART
#define THEME_SC_CARD0_ART "card_0"
#endif
#ifndef THEME_SC_CARD1_ART
#define THEME_SC_CARD1_ART "card_1"
#endif
#ifndef THEME_INK_RGB
#define THEME_INK_RGB THEME_SLAB_RGB
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
       SC_CARD = 200, SC_TILE = 300 };
enum { SC_TILE_GRAPHICS, SC_TILE_CONTROLS, SC_TILE_ONLINE, SC_TILE_SAVES, SC_TILE_UPDATES, SC_TILES };
/* The tiles shown: Updates only when the install has an updater (launcher.c). */
static int sc_tiles(void) { return g_upd != UPD_OFF ? SC_TILES : SC_TILES - 1; }

static int g_sc_hover = SC_NONE, g_sc_pressed = SC_NONE;
static int g_sc_focus = -1;               /* -1 the cards, else a tile */
static int g_sc_open = -1;                /* a panel: the Tab it shows, or -1 */
/* The Graphics panel's ADVANCED button has the keys (up from its first row). */
static int g_sc_adv_focus;

static COLORREF sc_c(uint32_t rgb) { return THEME_RGB_(rgb); }

/* A picture of the game's: in a launcher of several, the picked one's, from
 * its own art folder (launcher_games.h). */
static const uint32_t *sc_art(const char *name, int *w, int *h)
{
#ifdef LAUNCHER_GAMES
    return launcher_art_image_of(launcher_games[g_game].title_id, name, w, h);
#else
    return launcher_art_image(name, w, h);
#endif
}

/* The panel's Advanced part is there: something to put in it. */
static int sc_has_advanced(void)
{
#ifdef LAUNCHER_GAMES
    return g_adv_count > 0;
#else
    return 0;
#endif
}
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

/* The spaced capitals every label is set in: a section, a tile, a kicker. */
static void sc_caps(ThemeRect r, const char *text, int size, uint32_t rgb, ThemeAlign al, int tracking)
{
    char up[256];

    sc_upper(text, up, sizeof up);
    theme_text_ex(r, up, size, 700, sc_c(rgb), al, tracking, THEME_FACE_DISPLAY, 0);
}

/* A heading in the Online panel's box (draw_link). */
static void sc_link_heading(ThemeRect r, const char *text)
{
    sc_caps(r, text, 8, THEME_ACCENT_RGB, THEME_LEFT, 3);
}

/* ---------------------------------------------------------------- facts */

#ifdef LAUNCHER_GAMES
/* A launcher of several games: a card for each, the picked one selected; one
 * that is not installed is shown, dimmed, and cannot be picked. */
static int sc_card_ok(int i)
{
    return i >= 0 && i < launcher_game_count && i < LAUNCHER_GAMES_MAX && g_game_home[i][0];
}

static int sc_card(void)
{
    return g_game;
}

static void sc_set_card(int i)
{
    if (sc_card_ok(i))
        game_pick(i);
}
#else
/* The two renderers a card can choose: Direct3D 11 and Vulkan on Windows;
 * elsewhere Vulkan alone (the second card says why it cannot be had). */
static int sc_card_ok(int i)
{
#if defined(_WIN32)
    (void)i;
    return 1;
#else
    return i == 0;
#endif
}

#if defined(_WIN32)
/* The card a game plays with when the player has not picked (`renderer =
 * auto`): the title's own renderer if it names Vulkan (Timeslip does), else
 * Direct3D 11, as rhi_select_backend chooses. */
static int sc_default_card(void)
{
    const char *t = recomp_title_setting_default("renderer");
    return t && !strcmp(t, "vulkan") ? 1 : 0;
}
#endif

static int sc_card(void)
{
#if defined(_WIN32)
    if (!g_settings.renderer[0] || !strcmp(g_settings.renderer, "auto"))
        return sc_default_card();
    return !strcmp(g_settings.renderer, "vulkan") ? 1 : 0;
#else
    return 0;
#endif
}

static void sc_set_card(int i)
{
    if (!sc_card_ok(i))
        return;
#if defined(_WIN32)
    snprintf(g_settings.renderer, sizeof g_settings.renderer, "%s", i ? "vulkan" : "d3d11");
#endif
}
#endif

static const char *sc_tile_title(int k)
{
    static const char *const t[] = { "GRAPHICS OPTIONS", "CONTROLS", "ONLINE", "SAVES", "UPDATES" };
    return t[k];
}

static void sc_tile_summary(int k, char *out, size_t n)
{
    switch (k) {
    case SC_TILE_GRAPHICS:
        if (sc_tiles() > 4)                   /* a narrower tile: no room for the filtering */
            snprintf(out, n, "%s" SC_DOT "%dx" SC_DOT "%s",
                     g_settings.fullscreen ? "Fullscreen" : "Window", g_settings.resolution_scale,
                     g_settings.widescreen ? "16:9" : "4:3");
        else
            snprintf(out, n, "%s" SC_DOT "%dx" SC_DOT "%s" SC_DOT "%dx AF",
                     g_settings.fullscreen ? "Fullscreen" : "Window", g_settings.resolution_scale,
                     g_settings.widescreen ? "16:9" : "4:3", g_settings.anisotropy);
        break;
    case SC_TILE_CONTROLS:
        snprintf(out, n, "%s", g_settings.mouse_look ? "Keys & Mouse" : "Controller");
        break;
    case SC_TILE_ONLINE:
        if (!strcmp(g_settings.coop, "host"))         snprintf(out, n, "Co-op: Hosting");
        else if (!strcmp(g_settings.coop, "join"))    snprintf(out, n, "Co-op: Joining");
        else if (!strcmp(g_settings.syslink, "host")) snprintf(out, n, "System Link: Host");
        else if (!strcmp(g_settings.syslink, "join")) snprintf(out, n, "System Link: Join");
        else                                          snprintf(out, n, "Off");
        break;
    case SC_TILE_UPDATES:
        switch (g_upd) {
        case UPD_CHECKING:  snprintf(out, n, "Checking\xe2\x80\xa6"); break;
        case UPD_CURRENT:   snprintf(out, n, "Up to date"); break;
        case UPD_AVAILABLE: snprintf(out, n, "Update ready"); break;
        case UPD_NONE:      snprintf(out, n, "No release yet"); break;
        default:            snprintf(out, n, "Can't check"); break;
        }
        break;
    default:
        snprintf(out, n, "Open");
        break;
    }
}

static const char *sc_tile_note(int k)
{
    switch (k) {
    case SC_TILE_GRAPHICS: return "Resolution, widescreen, texture sharpness, frame rate and fullscreen.";
    case SC_TILE_CONTROLS: return "Keyboard and mouse, controllers, mouse look and sensitivity.";
    case SC_TILE_ONLINE:   return "Online co-op with a friend, or System Link for multiplayer.";
    case SC_TILE_UPDATES: {
        static char note[200];
        switch (g_upd) {
        case UPD_CHECKING:  return "Asking GitHub for the newest release.";
        case UPD_CURRENT:   snprintf(note, sizeof note, "You have the newest release, %s. Enter checks again.", g_upd_tag); return note;
        case UPD_AVAILABLE: snprintf(note, sizeof note, "%s is out. Enter installs it: it builds again from your own disc, a few minutes.", g_upd_tag); return note;
        case UPD_NONE:      return "No release has been published on GitHub yet. Enter checks again.";
        default:            return "GitHub could not be asked. Enter tries again.";
        }
    }
    default:               return "The game's saved profiles and progress, in your file browser.";
    }
}

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

/* -------------------------------------------------------------- geometry */

static ThemeRect sc_card_rect(int i) { return sc_rect(60 + i * 436, 250, 404, 224); }
/* The Graphics tile is wider than the rest, its summary being longer: two
 * tiles across with four, as the Mac's; 1.6 with five (Updates), so the
 * others still hold "Keys & Mouse". */
static ThemeRect sc_tile_rect(int k)
{
    int n = sc_tiles(), xi;
    double gw = n > 4 ? 1.6 : 2.0, gwide;
    double u = (840 - 16 * (n - 1) - 16 * (gw - 1)) / (n - 1 + gw);
    double x0, x1;

    gwide = gw * u + 16 * (gw - 1);
    if (k == 0) {
        x0 = 60;
        x1 = x0 + gwide;
    } else {
        x0 = 60 + gwide + 16 + (k - 1) * (u + 16);
        x1 = x0 + u;
    }
    xi = (int)(x0 + 0.5);
    return sc_rect(xi, 500, (int)(x1 + 0.5) - xi, 66);
}
static ThemeRect sc_play_rect(void) { return sc_rect(SC_W - 60 - 200, SC_H - 68, 200, 46); }
static ThemeRect sc_quit_rect(void) { return sc_rect(SC_W - 60 - 200 - 20 - 150, SC_H - 68, 150, 46); }

/* A panel is the Mac's 720 wide, as tall as its rows. */
enum { SC_PANEL_X = 120, SC_PANEL_W = 720, SC_PANEL_TOP = 26, SC_ROWS_AT = 80, SC_PITCH = 48,
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
/* Under the rows: their help (44); on Controls the slot for the columns'
 * names; on Online the box draw_link adds (with no help over it when the
 * only row is System Link's, which the box explains). */
static int sc_panel_extra(void)
{
    return g_sc_open == TAB_LINK  ? (g_link_count > 1 ? 52 : 12) + link_box_h() + 30
         : g_sc_open == TAB_INPUT ? 48 + SC_BIND_GAP
#ifdef LAUNCHER_GAMES
         : g_sc_open == TAB_ADVANCED ? 64
#endif
         : 48;
}

/* The rows' pitch: the Mac's 48, or as much less as lets every row and the
 * help under them fit above the buttons (Graphics has nine rows: at 48 its
 * help was cut off by the panel's edge). */
static int sc_pitch(void)
{
    int rows = sc_panel_rows(), room = SC_H - 92 - 12 - SC_PANEL_TOP - SC_ROWS_AT - sc_panel_extra();
    int pitch = rows > 0 ? room / rows : SC_PITCH;

    return pitch >= SC_PITCH ? SC_PITCH : pitch < 38 ? 38 : pitch;
}

static ThemeRect sc_panel_rect(void)
{
    int h = SC_ROWS_AT + sc_panel_rows() * sc_pitch() + sc_panel_extra();

    if (h > SC_H - 92 - 12 - SC_PANEL_TOP)
        h = SC_H - 92 - 12 - SC_PANEL_TOP;
    return sc_rect(SC_PANEL_X, SC_PANEL_TOP, SC_PANEL_W, h);
}

/* Where the plain launcher's rows go inside a panel (row_rect reads these). */
static void sc_rows_in_panel(void)
{
    ThemeRect p = sc_panel_rect();

    g_row_x = p.x + 30; g_row_y = p.y + SC_ROWS_AT; g_row_w = p.w - 60;
    g_row_pitch = sc_pitch(); g_row_h = g_row_pitch - 6;
    g_content_bottom = p.y + p.h - 14;
    /* Controls: a slot over the first control for its columns' names. */
    g_row_gap = g_row_gap_at = 0;
    if (g_sc_open == TAB_INPUT) {
        g_row_gap = SC_BIND_GAP;
        g_row_gap_at = BIND_HEAD_ROWS - g_bind_top > 0 ? BIND_HEAD_ROWS - g_bind_top : 0;
    }
}

#ifdef LAUNCHER_GAMES
/* The Advanced panel's way back to Graphics: at its title's left. */
static ThemeRect sc_advanced_back_rect(void)
{
    ThemeRect p = sc_panel_rect();

    return sc_rect(p.x + 30, p.y + 20, 120, 28);
}
#endif

/* The Graphics panel's ADVANCED button: at its title's right. */
static ThemeRect sc_advanced_rect(void)
{
    ThemeRect p = sc_panel_rect();

    return sc_rect(p.x + p.w - 30 - 132, p.y + 20, 132, 28);
}

static int sc_hit(int mx, int my)
{
    int i;

    if (g_sc_open == TAB_VIDEO && sc_has_advanced() && sc_in(sc_advanced_rect(), mx, my))
        return SC_ADVANCED;
#ifdef LAUNCHER_GAMES
    if (g_sc_open == TAB_ADVANCED && sc_in(sc_advanced_back_rect(), mx, my))
        return SC_BACK;
#endif

    if (sc_in(sc_play_rect(), mx, my)) return SC_PLAY;
    if (sc_in(sc_quit_rect(), mx, my)) return g_sc_open >= 0 ? SC_BACK : SC_QUIT;
    if (g_sc_open >= 0)
        return sc_in(sc_panel_rect(), mx, my) ? SC_NONE : (my < SC_H - 92 ? SC_BACK : SC_NONE);
    for (i = 0; i < 2; i++)
        if (sc_in(sc_card_rect(i), mx, my)) return SC_CARD + i;
    for (i = 0; i < sc_tiles(); i++)
        if (sc_in(sc_tile_rect(i), mx, my)) return SC_TILE + i;
    return SC_NONE;
}

/* -------------------------------------------------------------- pieces */

/* A button: the primary one filled with the accent and glowing, the other
 * dark; both with corner marks, lit when hot or focused, faded when it has
 * nothing to do. */
static void sc_button_ex(ThemeRect r, const char *title, int primary, int hot, int down, int off)
{
    ThemeRect br = r;
    double fade = off ? 0.45 : 1.0;

    if (primary && !off) {
        double at[2] = { 0, 1 };
        uint32_t c[2] = { THEME_ACCENT_RGB, THEME_ACCENT_DIM_RGB };
        double a[2] = { down ? 0.85 : hot ? 0.72 : 0.58, down ? 0.9 : 0.72 };

        theme_glow_rect(r, THEME_ACCENT_RGB, hot ? 0.5 : 0.3, hot ? 9 : 6);
        theme_fill(r, THEME_INK_RGB, 0.9);
        theme_vgrad(r, 2, at, c, a);
        theme_outline(r, THEME_ACCENT_RGB, 1, 1);
    } else {
        theme_fill(r, THEME_INK_RGB, down ? 0.95 : 0.82);
        theme_outline(r, hot && !off ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, fade, 1);
    }
    br.x -= 4; br.y -= 4; br.w += 8; br.h += 8;
    theme_brackets(br, 9, 2, (primary && !off) || hot ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, fade);
    theme_text_ex(r, title, primary ? 14 : 12, 700,
                  off ? sc_c(THEME_TEXT_DIM_RGB)
                      : (primary || hot) ? RGB(255, 255, 255) : sc_c(THEME_TEXT_RGB),
                  THEME_CENTRE, 5, THEME_FACE_DISPLAY, 1);
}

static void sc_button(ThemeRect r, const char *title, int code, int primary)
{
    sc_button_ex(r, title, primary, g_sc_hover == code, g_sc_pressed == code, 0);
}

/* A framed box: the dark panel, its hairline, corner marks. */
static void sc_box(ThemeRect r, double fill, int marks)
{
    ThemeRect b = r;

    theme_fill(r, THEME_INK_RGB, fill);
    theme_outline(r, THEME_ACCENT_DIM_RGB, 0.9, 1);
    if (marks) {
        b.x -= 5; b.y -= 5; b.w += 10; b.h += 10;
        theme_brackets(b, 16, 2, THEME_ACCENT_RGB, 0.9);
    }
}

/* ------------------------------------------------------------- the rows */

/* The background of a row in a panel: nothing at rest, a tint and a bar on
 * the focused one, a hairline under every row but the last. */
static void sc_row_bg(ThemeRect rr, int foc, int last)
{
    if (foc) {
        theme_fill(sc_rect(rr.x - 10, rr.y - 2, rr.w + 20, rr.h + 4), THEME_ACCENT_RGB, 0.12);
        theme_fill(sc_rect(rr.x - 10, rr.y - 2, 3, rr.h + 4), THEME_ACCENT_RGB, 1);
    }
    if (!last)
        theme_fill(sc_rect(rr.x, rr.y + rr.h + 2, rr.w, 1), THEME_ACCENT_DIM_RGB, 0.3);
}

/* The two chevrons a value sits between, and where a click on them lands
 * (hit_arrow reads the same numbers). */
static void sc_row_arrows(ThemeRect rr, int can_left, int can_right, int foc)
{
    double cy = rr.y + rr.h / 2.0;

    theme_chevron(rr.x + rr.w - 315, cy, 0, 8, 14,
                  can_left ? (foc ? 0xFFFFFF : THEME_ACCENT_RGB) : THEME_ACCENT_DIM_RGB,
                  can_left ? 1.0 : 0.5);
    theme_chevron(rr.x + rr.w - 19, cy, 1, 8, 14,
                  can_right ? (foc ? 0xFFFFFF : THEME_ACCENT_RGB) : THEME_ACCENT_DIM_RGB,
                  can_right ? 1.0 : 0.5);
}

static void sc_row_label(ThemeRect rr, const char *label, int foc)
{
    sc_caps(sc_rect(rr.x + 6, rr.y, 300, rr.h), label, 11, foc ? THEME_ACCENT_RGB : THEME_TEXT_RGB,
            THEME_LEFT, 3);
}

static void sc_row_value(ThemeRect rr, const char *value, int dim)
{
    theme_text_ex(sc_rect(rr.x + rr.w - 300, rr.y, 262, rr.h), value, 13, 600,
                  dim ? sc_c(THEME_TEXT_DIM_RGB) : sc_c(THEME_TEXT_RGB), THEME_CENTRE, 0,
                  THEME_FACE_BODY, 0);
}

static int sc_draw_rows(const Row *rows, int count, int wrap_help)
{
    char value[160];
    int i;
    ThemeRect r;

    for (i = 0; i < count; i++) {
        const Row *row = &rows[i];
        ThemeRect rr = row_rect(i);
        int foc = i == g_sel, dim_value = 0;

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
    r = sc_rect(g_row_x + 6, g_row_y + count * g_row_pitch + 4, g_row_w - 12, 40);
    if (wrap_help && count == 1)
        return r.y;
    if (g_sel < count)
        theme_text_wrapped(r, row_help(&rows[g_sel]), 10, THEME_TEXT_DIM);
    return r.y + r.h;
}

/* Where a binding row's two columns start (the clicks read it too). */
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
            /* The columns' names, in the slot over the first control. */
            sc_caps(sc_rect(sc_bind_col_x(rr, 0), rr.y - 22, SC_BIND_COL_W, 16), "CONTROLLER", 8,
                    g_bind_col == 0 ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, THEME_CENTRE, 2);
            sc_caps(sc_rect(sc_bind_col_x(rr, 1), rr.y - 22, SC_BIND_COL_W, 16), "KEYS & MOUSE", 8,
                    g_bind_col == 1 ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, THEME_CENTRE, 2);
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
            /* An action, not a value: said as one, at the row's end. */
            sc_row_label(rr, label, foc);
            sc_caps(sc_rect(rr.x + rr.w - 300, rr.y, 292, rr.h), "BACK TO THE DEFAULTS", 9,
                    foc ? 0xFFFFFF : THEME_ACCENT_RGB, THEME_RIGHT, 3);
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
        /* A control: its name, then what the pad and the keyboard give it. */
        {
            int c = k - BIND_HEAD_ROWS;

            sc_row_label(rr, bind_control_labels[c], foc);
            cv = sc_rect(sc_bind_col_x(rr, 0) + 4, rr.y, SC_BIND_COL_W - 8, rr.h);
            theme_text_ex(cv, (g_capturing && foc && g_bind_col == 0) ? "press" SC_ELL
                                                                     : bind_source_label(bp->pad_src[c]),
                          11, 600, foc && g_bind_col == 0 ? sc_c(THEME_ACCENT_RGB) : sc_c(THEME_TEXT_RGB),
                          THEME_CENTRE, 0, THEME_FACE_BODY, 0);
            cv.x = sc_bind_col_x(rr, 1) + 4;
            theme_text_ex(cv, (g_capturing && foc && g_bind_col == 1) ? "press" SC_ELL
                                                                     : bind_source_label(bp->key_src[c]),
                          11, 600, foc && g_bind_col == 1 ? sc_c(THEME_ACCENT_RGB) : sc_c(THEME_TEXT_RGB),
                          THEME_CENTRE, 0, THEME_FACE_BODY, 0);
        }
    }
    {   /* Where we are in a list longer than the panel: a track and a thumb. */
        ThemeRect t = row_rect(0), th;
        int span = row_rect(BIND_ROWS_VISIBLE - 1).y + g_row_h - t.y;

        t.x = g_row_x + g_row_w + 19; t.w = 2; t.h = span;
        theme_fill(t, THEME_ACCENT_DIM_RGB, 0.5);
        th = t;
        th.h = span * BIND_ROWS_VISIBLE / BIND_TOTAL_ROWS;
        th.y = t.y + (span - th.h) * g_bind_top / (BIND_TOTAL_ROWS - BIND_ROWS_VISIBLE);
        theme_fill(th, THEME_ACCENT_RGB, 1);
    }
    {
        int k = g_bind_top + g_sel;
        ThemeRect r = sc_rect(g_row_x + 6, row_rect(BIND_ROWS_VISIBLE - 1).y + g_row_pitch + 4,
                              g_row_w - 12, 40);

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
            10, THEME_TEXT_DIM);
    }
}

/* -------------------------------------------------------- the main screen */

#ifdef LAUNCHER_GAMES
/* A game's card: its own picture, from its own art folder (the player's,
 * made from their disc), whether it is there to play, its name and what is
 * in it. The renderer cards' numbers. */
static void sc_card_draw(int i)
{
    const LauncherGame *g = &launcher_games[i];
    ThemeRect r = sc_card_rect(i), t;
    int ok = sc_card_ok(i), sel = ok && sc_card() == i, hot = g_sc_hover == SC_CARD + i && ok;
    int foc = sel && g_sc_focus == -1 && g_sc_open < 0, w = 0, h = 0;
    const uint32_t *shot = launcher_art_image_of(g->title_id, g->card_art, &w, &h);

    if (sel)
        theme_glow_rect(r, THEME_ACCENT_RGB, g_sc_focus >= 0 ? 0.22 : 0.45, 13);
    theme_fill(r, 0x000000, 1);
    if (shot)
        theme_cover(r, shot, w, h, 0.82, 0.5, 0.38, sel ? 1.0 : hot ? 0.85 : ok ? 0.58 : 0.3,
                    1 + i);
    {
        double at[3] = { 0, 0.42, 1 };
        uint32_t c[3] = { 0x000000, 0x000000, THEME_INK_RGB };
        double a[3] = { 0, 0.3, 0.94 };

        theme_vgrad(r, 3, at, c, a);
    }
    if (!sel)
        theme_fill(r, THEME_INK_RGB, hot ? 0.16 : ok ? 0.32 : 0.5);
    t = sc_rect(r.x + 20, r.y + r.h - 100, 360, 16);
    sc_caps(t, ok ? "READY TO PLAY" : "NOT INSTALLED", 8,
            sel || hot ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, THEME_LEFT, 3);
    t = sc_rect(r.x + 18, r.y + r.h - 86, 380, 52);
    theme_text_ex(t, g->card_name, 25, 700,
                  !ok ? sc_c(THEME_TEXT_DIM_RGB)
                  : sel || hot ? RGB(255, 255, 255) : RGB(0xC1, 0xC6, 0xD0),
                  THEME_LEFT, 4, THEME_FACE_HEAVY, 1);
    t = sc_rect(r.x + 20, r.y + r.h - 32, 370, 20);
    theme_text_ex(t, ok ? g->card_line : "Install it from its disc to play it here", 10, 400,
                  sel || hot ? sc_c(THEME_TEXT_RGB) : sc_c(THEME_TEXT_DIM_RGB),
                  THEME_LEFT, 0, THEME_FACE_BODY, 0);
    theme_outline(r, sel || hot ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, sel ? 1 : ok ? 0.7 : 0.4,
                  sel ? 2 : 1);
    if (sel) {
        ThemeRect b = r, tag = sc_rect(r.x + r.w - 104, r.y + 12, 92, 22);

        b.x -= 6; b.y -= 6; b.w += 12; b.h += 12;
        theme_brackets(b, foc ? 22 : 16, foc ? 3 : 2, THEME_ACCENT_RGB, 1);
        theme_fill(tag, THEME_ACCENT_RGB, 0.92);
        sc_caps(tag, "SELECTED", 8, THEME_INK_RGB, THEME_CENTRE, 2);
    }
}
#else
static void sc_card_draw(int i)
{
#if defined(_WIN32)
    static const char *const kicker[] = { "NATIVE RENDERER", "CROSS-PLATFORM RENDERER" };
    static const char *const name[] = { "DIRECT3D 11", "VULKAN" };
    const char *const line[] = {
        sc_default_card() == 0 ? "Windows' own graphics API, the default" : "Windows' own graphics API",
        sc_default_card() == 1 ? "The default: the renderer Linux uses too"
                               : "The renderer Linux and the Steam Deck use" };
#else
    static const char *const kicker[] = { "NATIVE RENDERER", "WINDOWS ONLY" };
    static const char *const name[] = { "VULKAN", "DIRECT3D 11" };
    static const char *const line[] = { "The renderer this system draws with",
                                        "Microsoft's API: on Windows only" };
#endif
    static const char *const art[] = { THEME_SC_CARD0_ART, THEME_SC_CARD1_ART };
    ThemeRect r = sc_card_rect(i), t;
    int sel = sc_card() == i, hot = g_sc_hover == SC_CARD + i && sc_card_ok(i);
    int foc = sel && g_sc_focus == -1 && g_sc_open < 0, w = 0, h = 0;
    const uint32_t *shot = launcher_art_image(art[i], &w, &h);

    if (sel)
        theme_glow_rect(r, THEME_ACCENT_RGB, g_sc_focus >= 0 ? 0.22 : 0.45, 13);
    theme_fill(r, 0x000000, 1);
    if (shot)   /* the hub's portal kept high, clear of the words; the base above the HUD */
        theme_cover(r, shot, w, h, i == 0 ? 0.95 : 0.78, 0.5, i == 0 ? 0.32 : 0.35,
                    sel ? 1.0 : hot ? 0.85 : 0.58, 1 + i);
    {
        double at[3] = { 0, 0.42, 1 };
        uint32_t c[3] = { 0x000000, 0x000000, THEME_INK_RGB };
        double a[3] = { 0, 0.3, 0.94 };

        theme_vgrad(r, 3, at, c, a);
    }
    if (!sel)
        theme_fill(r, THEME_INK_RGB, hot ? 0.16 : 0.32);
    t = sc_rect(r.x + 20, r.y + r.h - 100, 360, 16);
    sc_caps(t, kicker[i], 8, sel || hot ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, THEME_LEFT, 3);
    t = sc_rect(r.x + 18, r.y + r.h - 86, 380, 52);
    theme_text_ex(t, name[i], 30, 700,
                  !sc_card_ok(i) ? sc_c(THEME_TEXT_DIM_RGB)
                  : sel || hot ? RGB(255, 255, 255) : RGB(0xC1, 0xC6, 0xD0),
                  THEME_LEFT, 6, THEME_FACE_HEAVY, 1);
    t = sc_rect(r.x + 20, r.y + r.h - 32, 370, 20);
    theme_text_ex(t, line[i], 10, 400, sel || hot ? sc_c(THEME_TEXT_RGB) : sc_c(THEME_TEXT_DIM_RGB),
                  THEME_LEFT, 0, THEME_FACE_BODY, 0);
    theme_outline(r, sel || hot ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, sel ? 1 : 0.7, sel ? 2 : 1);
    if (sel) {
        ThemeRect b = r, tag = sc_rect(r.x + r.w - 104, r.y + 12, 92, 22);

        b.x -= 6; b.y -= 6; b.w += 12; b.h += 12;
        theme_brackets(b, foc ? 22 : 16, foc ? 3 : 2, THEME_ACCENT_RGB, 1);
        theme_fill(tag, THEME_ACCENT_RGB, 0.92);
        sc_caps(tag, "SELECTED", 8, THEME_INK_RGB, THEME_CENTRE, 2);
    }
}
#endif

static void sc_tile_draw(int k)
{
    ThemeRect r = sc_tile_rect(k), e;
    int hot = g_sc_hover == SC_TILE + k, foc = g_sc_focus == k && g_sc_open < 0;
    char summary[96];

    theme_fill(r, THEME_INK_RGB, hot ? 0.9 : 0.8);
    if (foc)
        theme_fill(r, THEME_ACCENT_RGB, 0.10);
    theme_outline(r, hot || foc ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, hot || foc ? 1 : 0.8, 1);
    e = r; e.h = 2;
    theme_fill(e, hot || foc ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, 1);
    if (foc) {
        ThemeRect b = r;

        b.x -= 4; b.y -= 4; b.w += 8; b.h += 8;
        theme_brackets(b, 10, 2, THEME_ACCENT_RGB, 1);
    }
    sc_caps(sc_rect(r.x, r.y + 10, r.w, 18), sc_tile_title(k), 8,
            hot || foc ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, THEME_CENTRE, 3);
    sc_tile_summary(k, summary, sizeof summary);
    theme_text_ex(sc_rect(r.x + 12, r.y + 32, r.w - 24, 24), summary, 11, 600,
                  hot || foc ? RGB(255, 255, 255) : sc_c(THEME_TEXT_RGB), THEME_CENTRE, 0,
                  THEME_FACE_BODY, 0);
}

/* The art and the name across the top: the backdrop under it all, the
 * game's logo over its own glow and a soft accent halo, the project's name,
 * the line that says what this is. `compact` for the first-run screens. */
static void sc_header(int compact)
{
    int w = 0, h = 0, lw = 0, lh = 0, gw = 0, gh = 0;
    const uint32_t *bd = sc_art("backdrop", &w, &h);
    const uint32_t *logo = sc_art("logo", &lw, &lh);
    const uint32_t *glow = sc_art("glow", &gw, &gh);
    ThemeRect lr = compact ? sc_rect(290, 12, 380, 92) : sc_rect(230, 16, 500, 132), t;
    int sub_y = compact ? 106 : 150, tag_y = compact ? 140 : 184;

    theme_fill(sc_rect(0, 0, SC_W, SC_H), THEME_INK_RGB, 1);
    if (bd) {
        double at[4] = { 0, 0.32, 0.66, 1 };
        uint32_t c[4] = { THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB };
        double a[4] = { 0.45, 0.08, 0.5, 1.0 };
        ThemeRect top = sc_rect(0, 0, SC_W, 470);

        theme_cover(top, bd, w, h, 0.84, 0.5, 0.2, 1, 0);
        theme_fill(top, THEME_INK_RGB, 0.16);
        theme_vgrad(top, 4, at, c, a);
        /* The logo sits on bright rock: a pool of ink under it. */
        theme_glow_rect(sc_rect(SC_W / 2 - 1, lr.y + lr.h / 2, 2, 2), THEME_INK_RGB, 0.55,
                        compact ? 150 : 190);
    } else {
        /* No art yet (a first start): a low, wide glow behind the game's name. */
        theme_glow_rect(sc_rect(280, (compact ? 70 : 96) - 1, SC_W - 560, 2), THEME_GLOW_RGB, 0.6, 60);
    }
    theme_vignette(0.55, 0.5, 0.25);
    if (bd) {
        int top = sub_y - 14, bottom = compact ? 260 : 290;
        double at[4] = { 0, compact ? 0.25 : 0.2, 0.6, 1 };
        uint32_t c[4] = { THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB, THEME_INK_RGB };
        double a[4] = { 0, 0.6, 0.6, 0 };

        theme_vgrad(sc_rect(0, top, SC_W, bottom - top), 4, at, c, a);
    }
    if (logo) {
        theme_picture_glow(lr, logo, lw, lh, THEME_CENTRE, THEME_ACCENT_RGB, 0.45, 12, 5);
        if (glow) {
            ThemeRect drawn;

            theme_picture_ex(lr, logo, lw, lh, THEME_CENTRE, 0, 6, &drawn);   /* where it lands */
            drawn.x -= 2; drawn.y -= 2; drawn.w += 4; drawn.h += 4;
            theme_picture_ex(drawn, glow, gw, gh, THEME_CENTRE, 0.85, 7, NULL);
        }
        theme_picture_ex(lr, logo, lw, lh, THEME_CENTRE, 1, 6, NULL);
    } else {
        t = sc_rect(0, compact ? 34 : 60, SC_W, 72);
#ifdef LAUNCHER_GAMES
        theme_text_ex(t, launcher_games[g_game].name, compact ? 32 : 44, 700, sc_c(THEME_TEXT_RGB),
                      THEME_CENTRE,
#else
        theme_text_ex(t, THEME_SC_NAME, compact ? 32 : 44, 700, sc_c(THEME_TEXT_RGB), THEME_CENTRE,
#endif
                      8, THEME_FACE_HEAVY, 1);
        theme_fill(sc_rect(SC_W / 2 - 180, compact ? 102 : 141, 360, 1), THEME_ACCENT_DIM_RGB, 0.8);
    }
    t = sc_rect(0, sub_y, SC_W, 32);
    theme_text_ex(t, THEME_SC_SUBTITLE, compact ? 15 : 17, 700, sc_c(THEME_TEXT_RGB), THEME_CENTRE,
                  14, THEME_FACE_HEAVY, 1);
    sc_caps(sc_rect(0, tag_y, SC_W, 20), SC_TAGLINE, 9, THEME_ACCENT_RGB, THEME_CENTRE, 4);
}

static const char *sc_hint(void)
{
    int k = -1;

    if (g_sc_open >= 0)
        return g_pad_seen ? "D-pad changes" SC_DOT "B goes back"
                          : "\xe2\x86\x90/\xe2\x86\x92 change" SC_DOT "\xe2\x86\x91/\xe2\x86\x93 move" SC_DOT "Esc goes back";
    if (g_sc_hover >= SC_TILE && g_sc_hover < SC_TILE + sc_tiles())
        k = g_sc_hover - SC_TILE;
    else if (g_sc_hover == SC_NONE && g_sc_focus >= 0)
        k = g_sc_focus;
    if (k >= 0)
        return sc_tile_note(k);
#if defined(LAUNCHER_GAMES)
    if (g_sc_open < 0 && g_sc_adv_focus == 0 && g_sc_hover >= SC_CARD &&
        g_sc_hover < SC_CARD + 2) {
        static char line[160];
        int i = g_sc_hover - SC_CARD;

        if (!sc_card_ok(i))
            snprintf(line, sizeof line, "%s is not installed here.",
                     launcher_games[i].card_name);
        else
            snprintf(line, sizeof line, "%s: click to choose it, double-click to play.",
                     launcher_games[i].card_name);
        return line;
    }
#elif defined(_WIN32)
    if (g_sc_hover == SC_CARD)
        return sc_default_card() == 0
            ? "Direct3D 11: Windows' own renderer, the default. Double-click to play."
            : "Direct3D 11: Windows' own renderer. Double-click to play.";
    if (g_sc_hover == SC_CARD + 1)
        return sc_default_card() == 1
            ? "Vulkan: the default, the renderer the Linux build uses too. Double-click to play."
            : "Vulkan: the renderer the Linux build uses. Double-click to play.";
#endif
    return g_pad_seen ? "D-pad chooses" SC_DOT "A plays" SC_DOT "B quits"
                      : "\xe2\x86\x90/\xe2\x86\x92 choose" SC_DOT "\xe2\x86\x91/\xe2\x86\x93 settings" SC_DOT "Enter plays" SC_DOT "Esc quits";
}

static void sc_footer_line(const char *hint)
{
    theme_fill(sc_rect(60, SC_H - 92, 840, 1), THEME_ACCENT_DIM_RGB, 0.5);
    theme_text_ex(sc_rect(60, SC_H - 66, 440, 40), hint, 9, 400, sc_c(THEME_TEXT_DIM_RGB),
                  THEME_LEFT, 0, THEME_FACE_BODY, 0);
}

static void sc_footer(void)
{
    sc_footer_line(sc_hint());
    sc_button(sc_quit_rect(), g_sc_open >= 0 ? "BACK" : "QUIT", g_sc_open >= 0 ? SC_BACK : SC_QUIT, 0);
    sc_button(sc_play_rect(), "PLAY", SC_PLAY, 1);
}

static void draw_tab_body(void);          /* launcher.c: a tab's rows */

static void sc_panel(void)
{
    static const char *const titles[] = { "GRAPHICS OPTIONS", "CONTROLS", "ONLINE", "ABOUT",
                                          "ADVANCED OPTIONS" };
    ThemeRect p = sc_panel_rect(), b = p;

    theme_fill(sc_rect(0, 0, SC_W, SC_H - 92), THEME_INK_RGB, 0.94);
    theme_glow_rect(p, THEME_ACCENT_RGB, 0.28, 15);
    theme_fill(p, THEME_INK_RGB, 0.97);
    theme_outline(p, THEME_ACCENT_DIM_RGB, 1, 1);
    b.x -= 6; b.y -= 6; b.w += 12; b.h += 12;
    theme_brackets(b, 22, 3, THEME_ACCENT_RGB, 1);
    sc_caps(sc_rect(p.x, p.y + 18, p.w, 30), titles[g_sc_open < 5 ? g_sc_open : 3], 15,
            THEME_ACCENT_RGB, THEME_CENTRE, 6);
    theme_fill(sc_rect(p.x + 30, p.y + 58, p.w - 60, 1), THEME_ACCENT_DIM_RGB, 0.6);
    /* Graphics: what a player seldom needs is a step away, behind ADVANCED. */
    if (g_sc_open == TAB_VIDEO && sc_has_advanced()) {
        ThemeRect a = sc_advanced_rect();
        int hot = g_sc_hover == SC_ADVANCED, foc = g_sc_adv_focus;

        theme_fill(a, foc ? THEME_ACCENT_RGB : THEME_INK_RGB, foc ? 0.22 : 0.9);
        theme_outline(a, hot || foc ? THEME_ACCENT_RGB : THEME_ACCENT_DIM_RGB, hot || foc ? 1 : 0.8, 1);
        sc_caps(sc_rect(a.x, a.y, a.w - 14, a.h), "ADVANCED", 8,
                hot || foc ? THEME_HILITE_RGB : THEME_TEXT_DIM_RGB, THEME_CENTRE, 3);
        theme_chevron(a.x + a.w - 18, a.y + a.h / 2.0, 1, 5, 10,
                      hot || foc ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, 1.0);
        if (foc) {
            ThemeRect bb = a;

            bb.x -= 4; bb.y -= 4; bb.w += 8; bb.h += 8;
            theme_brackets(bb, 8, 2, THEME_ACCENT_RGB, 1);
        }
    }
#ifdef LAUNCHER_GAMES
    /* Advanced: where it came from, and the way back. */
    if (g_sc_open == TAB_ADVANCED) {
        ThemeRect back = sc_advanced_back_rect();
        int hot = g_sc_hover == SC_BACK;

        theme_chevron(back.x + 4, back.y + back.h / 2.0, 0, 5, 10,
                      hot ? THEME_ACCENT_RGB : THEME_TEXT_DIM_RGB, 1.0);
        sc_caps(sc_rect(back.x + 16, back.y, back.w - 16, back.h), "GRAPHICS", 8,
                hot ? THEME_HILITE_RGB : THEME_TEXT_DIM_RGB, THEME_LEFT, 3);
    }
#endif
    sc_rows_in_panel();
    draw_tab_body();
}

/* ------------------------------------------------- the first-run screens */

static ThemeRect sc_setup_low(void) { return sc_rect(60, SETUP_LOW_Y, 840, SETUP_LOW_H); }

static void sc_setup(void)
{
    ThemeRect p = sc_rect(60, SETUP_DESC_Y, 840, SETUP_DESC_H), r;
    int i;

    sc_box(p, 0.9, 0);
    sc_caps(sc_rect(p.x + 24, p.y + 16, p.w - 48, 22),
            g_rel_moved ? "The game's files have moved" : g_rel_stale ? "Build the game again"
                                                        : "Your copy of the game", 11,
            THEME_ACCENT_RGB, THEME_LEFT, 4);
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
        theme_outline(r, THEME_ACCENT_RGB, 1, 1);
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
        static const char *const lb[] = { "", "DISC IMAGE" SC_ELL, "FOLDER" SC_ELL, "BUILD" };
        const char *label = (i == 3 && g_rel_moved) ? "LINK AGAIN"
                          : (i == 3 && g_rel_stale) ? "BUILD AGAIN" : lb[i];

        sc_button_ex(setup_rect(i), label, i == 3,
                     g_setup_sel == i || (i == 3 && g_sc_hover == SC_PLAY), 0, i == 3 && !g_source[0]);
    }
    sc_button_ex(sc_quit_rect(), "QUIT", 0, g_sc_hover == SC_QUIT, 0, 0);

    r = sc_setup_low();
    if (g_rel_error[0]) {
        sc_box(r, 0.9, 1);
        sc_caps(sc_rect(r.x + 24, r.y + 14, r.w - 48, 22),
                g_rel_unsupported ? "This disc can't be used"
                : g_rel_relinking ? "The game's files were not linked" : "The build did not finish",
                11, THEME_ACCENT_RGB, THEME_LEFT, 4);
        theme_text_wrapped(sc_rect(r.x + 24, r.y + 42, r.w - 48, r.h - 48), g_rel_error, 10, THEME_TEXT);
    } else if (g_rel_note[0]) {
        theme_text_wrapped(sc_rect(r.x, r.y, r.w, 40), g_rel_note, 11, THEME_TEXT_DIM);
    } else {
        /* The drop target. The whole window takes a drop; this is where the
         * eye goes to see that it does. */
        double lit = g_pulse < 0.5 ? g_pulse * 2.0 : (1.0 - g_pulse) * 2.0;
        ThemeRect b = r;

        theme_fill(r, THEME_INK_RGB, 0.82);
        theme_fill(r, THEME_ACCENT_RGB, 0.03 + 0.04 * lit);
        theme_outline(r, THEME_ACCENT_DIM_RGB, 0.6 + 0.4 * lit, 1);
        b.x -= 5; b.y -= 5; b.w += 10; b.h += 10;
        theme_brackets(b, 22, 3, THEME_ACCENT_RGB, 0.6 + 0.4 * lit);
        sc_caps(sc_rect(r.x, r.y + r.h / 2 - 30, r.w, 30), "DROP THE GAME HERE", 15,
                THEME_TEXT_RGB, THEME_CENTRE, 6);
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
    ThemeRect p = sc_rect(60, SETUP_PANEL_Y, 840, 300), r, b;
    char pct[16], text[LAUNCHER_PATH + 400];

    sc_box(p, 0.9, 1);
    sc_caps(sc_rect(p.x + 30, p.y + 24, p.w - 60, 24),
            done ? "The game is ready" : g_rel_relinking ? "Finding the game's files" : "Building the game",
            14, done ? THEME_ACCENT_RGB : THEME_TEXT_RGB, THEME_LEFT, 5);
    r = sc_rect(p.x + 30, p.y + 72, p.w - 60, 24);
    theme_text_ex(r, done ? (g_rel_relinking ? "Files found" : "Build complete") : g_build_label,
                  12, 600, sc_c(done ? THEME_TEXT_RGB : THEME_ACCENT_RGB), THEME_LEFT, 0,
                  THEME_FACE_BODY, 0);
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

/* An update running, or how it ended (the Updates tile, launcher.c). */
static int sc_update_shown(void)
{
    return g_upd == UPD_RUNNING || g_upd == UPD_DONE || g_upd == UPD_FAILED;
}

static void sc_update_screen(void)
{
    ThemeRect p = sc_rect(60, SETUP_PANEL_Y, 840, 300), r, b;
    int done = g_upd == UPD_DONE, failed = g_upd == UPD_FAILED;
    char title[120], pct[16], text[600];

    sc_box(p, 0.9, 1);
    snprintf(title, sizeof title, done ? "Updated to %s" : failed ? "The update did not finish"
                                                                  : "Updating to %s", g_upd_tag);
    sc_caps(sc_rect(p.x + 30, p.y + 24, p.w - 60, 24), title, 14,
            done ? THEME_ACCENT_RGB : THEME_TEXT_RGB, THEME_LEFT, 5);
    r = sc_rect(p.x + 30, p.y + 72, p.w - 60, 24);
    theme_text_ex(r, failed ? "Stopped" : g_upd_phase, 12, 600,
                  sc_c(failed ? THEME_TEXT_RGB : THEME_ACCENT_RGB), THEME_LEFT, 0, THEME_FACE_BODY, 0);
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

/* Enter (accept) or Esc (cancel) on the update screen. */
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
            g_upd = UPD_CURRENT;          /* the new launcher next time */
    } else if (accept || cancel) {
        upd_check();
    }
}

/* The whole window. 1 when drawn (it always is in this layout). */
static int sc_draw(void)
{
    int i, hov = g_sc_hover, foc = g_sc_focus;

    if (sc_update_shown()) {
        sc_header(1);
        sc_update_screen();
        return 1;
    }
    if (g_release && g_rel != REL_READY) {
        sc_header(1);
        if (g_rel == REL_BUILDING)
            sc_progress(0);
        else if (g_rel == REL_DONE)
            sc_progress(1);
        else
            sc_setup();
        return 1;
    }
    sc_header(0);
#ifdef LAUNCHER_GAMES
    sc_caps(sc_rect(60, 226, 400, 16), "CHOOSE YOUR GAME", 8, THEME_TEXT_DIM_RGB, THEME_LEFT, 3);
    sc_caps(sc_rect(SC_W - 460, 226, 400, 16), launcher_games[g_game].place, 8, THEME_TEXT_DIM_RGB,
            THEME_RIGHT, 3);
#else
    sc_caps(sc_rect(60, 226, 400, 16), "CHOOSE YOUR RENDERER", 8, THEME_TEXT_DIM_RGB, THEME_LEFT, 3);
    sc_caps(sc_rect(SC_W - 460, 226, 400, 16), THEME_SC_PLACE, 8, THEME_TEXT_DIM_RGB, THEME_RIGHT, 3);
#endif
    if (g_sc_open >= 0)
        g_sc_hover = g_sc_focus = SC_NONE;     /* under a panel, nothing lit */
    for (i = 0; i < 2; i++)
        sc_card_draw(i);
    for (i = 0; i < sc_tiles(); i++)
        sc_tile_draw(i);
    if (g_sc_open >= 0) {
        g_sc_hover = hov;
        g_sc_focus = foc;
        sc_panel();
    }
    sc_footer();
    return 1;
}

/* ----------------------------------------------------------------- input */

static void sc_open_tile(int k)
{
    static const int tab_of[] = { TAB_VIDEO, TAB_INPUT, TAB_LINK };

    if (k == SC_TILE_SAVES) {
        sc_open_saves();
        return;
    }
    if (k == SC_TILE_UPDATES) {
        upd_activate();
        return;
    }
    g_sc_open = tab_of[k];
    set_tab((Tab)tab_of[k]);
    g_sel = 0;
    g_sc_adv_focus = 0;
    sc_rows_in_panel();
}

/* The Graphics panel's Advanced part, and back. */
static void sc_open_advanced(int open)
{
#ifdef LAUNCHER_GAMES
    g_sc_open = open ? TAB_ADVANCED : TAB_VIDEO;
    set_tab(open ? TAB_ADVANCED : TAB_VIDEO);
    g_sel = 0;
    g_sc_adv_focus = !open;              /* back on Graphics: the button, as left */
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

/* 1 when the layout handled it; 0 sends it on to the plain launcher's (a
 * panel's rows are its rows). */
static int sc_nav(int dx, int dy, int accept, int cancel, int tabdelta)
{
    if (sc_update_shown()) {
        sc_update_input(accept, cancel);
        return 1;
    }
    if (g_sc_open >= 0) {
        if (g_capturing)
            return 0;
        if (g_sc_open == TAB_VIDEO && sc_has_advanced()) {
            if (g_sc_adv_focus) {
                if (accept)
                    sc_open_advanced(1);
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
            static const int order[] = { TAB_VIDEO, TAB_INPUT, TAB_LINK };
            int i, at = 0;           /* Advanced counts as Graphics */

            for (i = 0; i < 3; i++)
                if (order[i] == g_sc_open)
                    at = i;
            sc_open_tile((at + tabdelta + 3) % 3);
            return 1;
        }
        sc_rows_in_panel();
        return 0;
    }
    if (dy) {
        g_sc_focus += dy;
        if (g_sc_focus >= sc_tiles()) g_sc_focus = -1;
        if (g_sc_focus < -1) g_sc_focus = sc_tiles() - 1;
    }
    if (dx) {
        if (g_sc_focus == -1) {
            int c = sc_card() + dx;

            if (c >= 0 && c < 2)
                sc_set_card(c);
        } else {
            g_sc_focus += dx;
            if (g_sc_focus < 0) g_sc_focus = 0;
            if (g_sc_focus >= sc_tiles()) g_sc_focus = sc_tiles() - 1;
        }
    }
    if (accept) {
        if (g_sc_focus >= 0)
            sc_open_tile(g_sc_focus);
        else
            do_play();
    }
    if (cancel)
        lhost_close();
    (void)tabdelta;
    return 1;
}

static int sc_click(int mx, int my, int dbl)
{
    int h = sc_hit(mx, my);

    if (sc_update_shown()) {
        if (sc_in(sc_play_rect(), mx, my))
            sc_update_input(1, 0);
        return 1;
    }
    if (h == SC_PLAY) { do_play(); return 1; }
    if (h == SC_QUIT) { lhost_close(); return 1; }
    if (h == SC_BACK) { sc_close(); return 1; }
    if (h == SC_ADVANCED) { sc_open_advanced(1); return 1; }
    if (g_sc_open >= 0) {
        sc_rows_in_panel();
        return 0;                             /* a panel's rows: the plain launcher's */
    }
    if (h >= SC_CARD && h < SC_CARD + 2) {
        sc_set_card(h - SC_CARD);
        g_sc_focus = -1;
        if (dbl && sc_card_ok(h - SC_CARD))
            do_play();
        return 1;
    }
    if (h >= SC_TILE && h < SC_TILE + sc_tiles()) {
        g_sc_focus = h - SC_TILE;
        sc_open_tile(h - SC_TILE);
        return 1;
    }
    return 1;
}

static void sc_hover_at(int mx, int my)
{
    g_sc_hover = sc_hit(mx, my);
    if (g_sc_open >= 0)
        sc_rows_in_panel();
}
