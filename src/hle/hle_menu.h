/*
 * hle_menu.h -- the in-game menu: Esc while a title runs.
 *
 * What a PC game has and a console game never needed: a way to stop, change
 * how it looks and plays, and quit, without leaving the game. Opening it
 * holds the title at its next frame (the flip gate, as Android's menu does)
 * and holds the sound with it, so nothing moves on behind it -- except in an
 * online co-op session, where the other player's game cannot wait: there the
 * game goes on, and the menu only takes the keyboard and mouse from it.
 *
 * It is drawn by the renderer over every presented frame (Dear ImGui through
 * the RHI, xbox_D3D8SetUiHook), so it looks the same on Direct3D 11, Vulkan
 * and Metal. A setting is applied at once where the runtime can change it
 * while the title runs, and written to the title's settings file either way;
 * the rest take effect at the next start, and the menu says so.
 *
 * Threads: the window's thread feeds input (recomp_menu_*) and opens or
 * closes it; the presenting thread draws it. Any thread may ask whether it
 * is open.
 */
#ifndef XBOXRECOMP_HLE_MENU_H
#define XBOXRECOMP_HLE_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

int  recomp_menu_is_open(void);
void recomp_menu_set_open(int open);
void recomp_menu_toggle(void);

/* Registers the menu with the renderer. Called once the device exists. */
void recomp_menu_install(void);

/* Input while it is open, from the window's thread. Positions are in the
 * swap chain's pixels (the window's client area). */
enum {
    RECOMP_MENU_KEY_NONE, RECOMP_MENU_KEY_TAB, RECOMP_MENU_KEY_LEFT, RECOMP_MENU_KEY_RIGHT,
    RECOMP_MENU_KEY_UP, RECOMP_MENU_KEY_DOWN, RECOMP_MENU_KEY_PAGE_UP, RECOMP_MENU_KEY_PAGE_DOWN,
    RECOMP_MENU_KEY_HOME, RECOMP_MENU_KEY_END, RECOMP_MENU_KEY_DELETE, RECOMP_MENU_KEY_BACKSPACE,
    RECOMP_MENU_KEY_SPACE, RECOMP_MENU_KEY_ENTER, RECOMP_MENU_KEY_ESCAPE, RECOMP_MENU_KEY_SHIFT,
    RECOMP_MENU_KEY_CTRL, RECOMP_MENU_KEY_ALT
};
void recomp_menu_mouse_pos(float x, float y);
void recomp_menu_mouse_button(int button, int down);   /* 0 left, 1 right, 2 middle */
void recomp_menu_mouse_wheel(float dy);                /* + is away from the player */
void recomp_menu_key(int key, int down);               /* RECOMP_MENU_KEY_* */
void recomp_menu_char(unsigned codepoint);

/* The window's side, which the menu asks for (src/hle/hle_d3d8.c). */
void hle_window_set_fullscreen(int on);   /* any thread */
int  hle_window_fullscreen(void);
void hle_window_menu_pointer(int menu_open);   /* give the pointer back, or take it */
/* The title's own menu is up (1: the pointer shown and free, the mouse's
 * left button A, right B, wheel up and down) or play is back (0). Any
 * thread; a title calls it as its screens change. */
void hle_window_title_menu(int on);

#ifdef __cplusplus
}
#endif

#endif
