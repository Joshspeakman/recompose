/*
 * launcher_theme.h -- how the launcher looks, kept behind an interface.
 *
 * Everything drawn goes through here, so the platform's drawing is kept
 * out of the launcher. The shapes are arithmetic over a 32-bit buffer
 * (launcher_theme.c), the same everywhere; the buffer and the text are the
 * host's: GDI and a DIB on Windows (launcher_theme_gdi.c), CoreText on
 * macOS (launcher_theme_coretext.c), FreeType on Linux
 * (launcher_theme_freetype.c).
 *
 * The look is the original Xbox dashboard's: black, a green glow that
 * comes from behind, panels with soft edges, and the selected thing
 * lit rather than outlined. None of the console's own artwork is used
 * or reproduced -- no logos, no wordmarks, no fonts, no ripped
 * textures. What is here is drawn from arithmetic at run time.
 */
#ifndef XBOXRECOMP_LAUNCHER_THEME_H
#define XBOXRECOMP_LAUNCHER_THEME_H

#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
/* GDI's colour, 0x00BBGGRR, so the colours below read the same everywhere. */
typedef uint32_t COLORREF;
#define RGB(r, g, b) ((COLORREF)(((r) & 0xFF) | (((g) & 0xFF) << 8) | \
                                 (((b) & 0xFF) << 16)))
#endif

typedef struct ThemeRect { int x, y, w, h; } ThemeRect;
typedef enum { THEME_LEFT, THEME_CENTRE, THEME_RIGHT } ThemeAlign;

/* A title's own look (recomp_add_launcher THEME <header>): its header defines
 * any of the colours below, as 0xRRGGBB, and where its artwork can be fetched
 * (launcher_art.c). What it leaves out is the toolkit's own, the dashboard's
 * green, so a title without one looks as it always did. */
#ifdef LAUNCHER_THEME_HEADER
#include LAUNCHER_THEME_HEADER
#endif
#ifndef THEME_ACCENT_RGB
#define THEME_ACCENT_RGB      0x9BCE1E   /* glows, rims, the bar, the underline */
#endif
#ifndef THEME_ACCENT_DIM_RGB
#define THEME_ACCENT_DIM_RGB  0x4E6C14
#endif
#ifndef THEME_HILITE_RGB
#define THEME_HILITE_RGB      0xE4F5B0   /* the bar's moving highlight */
#endif
#ifndef THEME_BAR_DARKEN_RGB
#define THEME_BAR_DARKEN_RGB  0x30300A   /* the bar's fill darkens by this downwards */
#endif
#ifndef THEME_TRACK_RGB
#define THEME_TRACK_RGB       0x101A0C   /* the bar's empty track */
#endif
#ifndef THEME_SLAB_RGB
#define THEME_SLAB_RGB        0x0A1208   /* a panel at rest */
#endif
#ifndef THEME_SLAB_LIT_RGB
#define THEME_SLAB_LIT_RGB    0x183C0E   /* added to it as it lights */
#endif
#ifndef THEME_BG_RGB
#define THEME_BG_RGB          0x181E12   /* the background, outside the glow */
#endif
#ifndef THEME_GLOW_RGB
#define THEME_GLOW_RGB        0x30B022   /* added to it at the glow's heart */
#endif
#ifndef THEME_TEXT_RGB
#define THEME_TEXT_RGB        0xE8F2D8
#endif
#ifndef THEME_TEXT_DIM_RGB
#define THEME_TEXT_DIM_RGB    0x8A977C
#endif
#define THEME_RGB_(v) RGB(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)

/* The accent (the Xbox green unless a title says otherwise), and the rest. */
#define THEME_GREEN      THEME_RGB_(THEME_ACCENT_RGB)
#define THEME_GREEN_DIM  THEME_RGB_(THEME_ACCENT_DIM_RGB)
#define THEME_TEXT       THEME_RGB_(THEME_TEXT_RGB)
#define THEME_TEXT_DIM   THEME_RGB_(THEME_TEXT_DIM_RGB)

/* A back buffer the size of the window. Everything below draws into it
 * and theme_present puts it on screen in one go, so nothing flickers. */
int  theme_begin(int width, int height);
void theme_shutdown(void);
#ifdef _WIN32
void theme_present(HDC dc);
#else
/* Pixels per window unit, for a display with more than one (Retina): the
 * buffer is drawn at the display's own resolution rather than stretched up
 * to it. Takes effect at the next theme_begin; 1 until then. */
void theme_set_scale(double scale);
/* The buffer as it is now, for the front end to put on screen, or NULL.
 * macOS: a CGImageRef the caller releases, a copy, so drawing the next
 * frame does not reach into the one being shown. Linux: the buffer itself
 * (theme_px), which the front end copies to the screen before drawing
 * again. */
void *theme_image(void);
#endif

/* The background: black, with the glow behind where the content sits.
 * `pulse` runs 0..1 and drifts, so the glow breathes as the dashboard's
 * did rather than sitting still. */
void theme_background(double pulse);

/* A picture behind everything instead of the glow: `argb` (0xAARRGGBB rows,
 * w by h) fills the window, cropped to its shape, and is darkened downwards
 * so the panels over it read. Kept by pointer, not copied: NULL goes back to
 * the glow. */
void theme_backdrop(const uint32_t *argb, int w, int h);

/* A picture with its own transparency, as large as fits in `r` without
 * changing its shape, aligned left, centred or right. A clear logo where the
 * game's name would be written. */
void theme_picture(ThemeRect r, const uint32_t *argb, int w, int h, ThemeAlign align);

/* A panel. `lit` from 0 (resting) to 1 (selected) takes it from a dark
 * translucent slab to one edged and washed with green. */
void theme_panel(ThemeRect r, double lit);

/* Text, left/centre/right aligned within `r`. `size` is a point size;
 * `weight` is 400 for normal and 700 for bold. */
void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align);

/* The same, wrapped to the rectangle and top aligned, for the lines
 * of explanation. theme_text is single line: a newline in its text
 * draws as nothing and the line runs off the edge. */
void theme_text_wrapped(ThemeRect r, const char *text, int size,
                        COLORREF colour);

/* The tab strip's underline, which slides rather than jumping: `from`
 * and `to` are tab rectangles and `t` runs 0..1 between them. */
void theme_tab_underline(ThemeRect from, ThemeRect to, double t);

/* A left/right chevron pair beside a value, dimmed when the value is at
 * the end of its range and there is nowhere further to go. */
void theme_arrows(ThemeRect r, int can_left, int can_right, double lit);

/* A progress bar: a dark rounded track, green across the first `fraction`
 * (0..1) of it. The self-building release's build screen. `phase` (0..1,
 * advanced by the caller each frame) moves a soft highlight along it, so a
 * stage that sits on one percentage still shows the build is alive; a
 * negative phase draws it still. */
void theme_bar(ThemeRect r, double fraction, double phase);

#endif /* XBOXRECOMP_LAUNCHER_THEME_H */
