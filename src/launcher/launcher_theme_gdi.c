/*
 * launcher_theme_gdi.c -- the Windows half of launcher_theme.h: the back
 * buffer, a 32-bit DIB, and the text, which goes through GDI. GDI can draw
 * into the same DIB the shapes are written into (launcher_theme.c) and
 * already knows how to shape and hint a font. The launcher asks Windows not
 * to scale it, so a window unit is a pixel here.
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static HDC      g_dc;
static HBITMAP  g_bmp, g_old;

int theme_begin(int width, int height)
{
    BITMAPINFO bi;

    if (g_dc && width == theme_w && height == theme_h)
        return 1;
    theme_shutdown();

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_dc = CreateCompatibleDC(NULL);
    if (!g_dc)
        return 0;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&theme_px, NULL, 0);
    if (!g_bmp) {
        DeleteDC(g_dc);
        g_dc = NULL;
        return 0;
    }
    g_old = (HBITMAP)SelectObject(g_dc, g_bmp);
    theme_w = width;
    theme_h = height;
    theme_scale = 1.0;
    SetBkMode(g_dc, TRANSPARENT);
    return 1;
}

void theme_shutdown(void)
{
    if (g_dc) {
        SelectObject(g_dc, g_old);
        DeleteObject(g_bmp);
        DeleteDC(g_dc);
    }
    g_dc = NULL;
    g_bmp = NULL;
    theme_px = NULL;
    theme_w = theme_h = 0;
}

void theme_present(HDC dc)
{
    if (g_dc)
        BitBlt(dc, 0, 0, theme_w, theme_h, g_dc, 0, 0, SRCCOPY);
}

/* --------------------------------------------------------------- text */

void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align)
{
    HFONT font, old;
    RECT rc;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;

    if (!g_dc || !text)
        return;

    /* A face that is on every Windows install, and is not the console's.
     * Segoe UI where it exists, Arial where it does not. */
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!font)
        return;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextColor(g_dc, colour);

    rc.left = r.x;
    rc.top = r.y;
    rc.right = r.x + r.w;
    rc.bottom = r.y + r.h;
    if (align == THEME_CENTRE) flags |= DT_CENTER;
    else if (align == THEME_RIGHT) flags |= DT_RIGHT;

    DrawTextA(g_dc, text, -1, &rc, flags);
    SelectObject(g_dc, old);
    DeleteObject(font);
}

/* The showcase's text: Segoe UI for reading, Bahnschrift (Windows 10's DIN)
 * for the spaced capitals and the big names. Placed by hand rather than by
 * DrawText, which lines text up as if SetTextCharacterExtra's spacing were
 * not there: measured with it (less the trailing space after the last
 * letter), cut to fit with an ellipsis, centred on the box. The shadow, on
 * large text only, is the window's ink rather than black. */
void theme_text_ex(ThemeRect r, const char *text, int size, int weight, COLORREF colour,
                   ThemeAlign align, int tracking, int face, int shadow)
{
    static const char *const faces[] = { "Segoe UI", "Bahnschrift", "Bahnschrift",
                                         "Bahnschrift SemiBold Condensed", "Bahnschrift Condensed" };
    WCHAR wide[512];
    HFONT font, old;
    TEXTMETRICW tm;
    SIZE sz;
    int len, x, y, pass, vis;

    if (!g_dc || !text)
        return;
    if (face < 0 || face > 4)
        face = 0;
    len = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 510);
    if (len <= 1)
        return;
    len--;                                         /* without the terminator */
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0,
                       face == 2 || face == 4 ? 700 : face == 3 ? 400 : weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, faces[face]);
    if (!font)
        return;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextCharacterExtra(g_dc, tracking);
    GetTextMetricsW(g_dc, &tm);
    GetTextExtentPoint32W(g_dc, wide, len, &sz);
    vis = sz.cx - tracking;
    if (vis > r.w && len > 1) {                    /* too long: cut, with an ellipsis */
        while (len > 1 && vis > r.w) {
            len--;
            while (len > 1 && wide[len - 1] == L' ')
                len--;
            wide[len] = 0x2026;
            GetTextExtentPoint32W(g_dc, wide, len + 1, &sz);
            vis = sz.cx - tracking;
        }
        len++;
    }
    x = align == THEME_LEFT ? r.x : align == THEME_RIGHT ? r.x + r.w - vis : r.x + (r.w - vis) / 2;
    y = r.y + (r.h - tm.tmHeight) / 2;
    for (pass = (shadow && size >= 12) ? 0 : 1; pass < 2; pass++) {
        int off = pass == 0 ? 1 : 0;

        SetTextColor(g_dc, pass == 0 ? THEME_RGB_(THEME_INK_SHADOW_RGB) : colour);
        ExtTextOutW(g_dc, x + off, y + off, 0, NULL, wide, (UINT)len, NULL);
    }
    SetTextCharacterExtra(g_dc, 0);
    SelectObject(g_dc, old);
    DeleteObject(font);
}

int theme_text_width(const char *text, int size, int weight, int tracking, int face)
{
    static const char *const faces[] = { "Segoe UI", "Bahnschrift", "Bahnschrift",
                                         "Bahnschrift SemiBold Condensed", "Bahnschrift Condensed" };
    WCHAR wide[512];
    HFONT font, old;
    SIZE sz;
    int len;

    if (!g_dc || !text)
        return 0;
    if (face < 0 || face > 4)
        face = 0;
    len = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 510);
    if (len <= 1)
        return 0;
    len--;
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0,
                       face == 2 || face == 4 ? 700 : face == 3 ? 400 : weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, faces[face]);
    if (!font)
        return 0;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextCharacterExtra(g_dc, tracking);
    GetTextExtentPoint32W(g_dc, wide, len, &sz);
    SetTextCharacterExtra(g_dc, 0);
    SelectObject(g_dc, old);
    DeleteObject(font);
    return sz.cx - tracking;
}

void theme_text_wrapped(ThemeRect r, const char *text, int size, COLORREF colour)
{
    HFONT font, old;
    RECT rc;

    if (!g_dc || !text)
        return;
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0, 400, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!font)
        return;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextColor(g_dc, colour);

    rc.left = r.x;
    rc.top = r.y;
    rc.right = r.x + r.w;
    rc.bottom = r.y + r.h;
    DrawTextA(g_dc, text, -1, &rc, DT_WORDBREAK | DT_TOP | DT_NOPREFIX);
    SelectObject(g_dc, old);
    DeleteObject(font);
}
