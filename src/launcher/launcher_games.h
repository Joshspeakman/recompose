/*
 * launcher_games.h -- one launcher for a collection of games
 * (recomp_add_launcher ... GAMES <file>).
 *
 * The file names the games, in the order their cards are shown: where each
 * one's program is, what its card says, and its own defaults (the same
 * title_defaults.c its game is built with). The launcher finds which of them
 * are installed, shows a card for each, and everything else on it -- the
 * art across the top, the settings the tiles open, the controls, the
 * updater, Play -- is the picked game's: its own settings file, its own
 * bindings, its own art folder. The pictures are still the player's own,
 * made from their discs into each game's art folder (launcher_art.c).
 *
 * The showcase layout (LAUNCHER_SHOWCASE) puts the games where a single
 * game's launcher puts its renderer cards; the renderer moves into the
 * Graphics panel's Advanced part.
 */
#ifndef XBOXRECOMP_LAUNCHER_GAMES_H
#define XBOXRECOMP_LAUNCHER_GAMES_H

#include <stdint.h>

#define LAUNCHER_GAMES_MAX 4

typedef struct LauncherGame {
    const char *key;            /* a short word, remembered as the game last picked */
    uint32_t    title_id;       /* its id: its art folder, before its image is read */
    const char *exe;            /* its program, without ".exe" */
    const char *xbe;            /* the image it boots, as its main.c's YOUR_GAME_XBE_NAME */
    const char *game_dir;       /* its main.c's YOUR_GAME_DIR, from its program's folder;
                                 * NULL for none */
    /* Where its program may be, from the launcher's folder, in order, NULL
     * at the end: "." (beside the launcher), a sibling install's folder, a
     * build tree's. The first with the program in it is the game's. */
    const char *const *dirs;
    const char *name;           /* written across the top when it has no logo */
    const char *card_name;      /* its card's name */
    const char *card_line;      /* the line under it */
    const char *place;          /* over the cards, at the right */
    const char *card_art;       /* its card's picture, from its own art folder */
    /* What its Widescreen does (recomp_config.h RecompWideMode: 1 its own 16:9
     * mode, 2 its camera widened), which a single game's build is told at
     * compile time; 0 leaves the build's. */
    int         widescreen;
    const char *const *bindings;   /* its recomp_title_bindings[] */
    const char *const *settings;   /* its recomp_title_settings[] */
} LauncherGame;

extern const LauncherGame launcher_games[];
extern const int launcher_game_count;

#endif /* XBOXRECOMP_LAUNCHER_GAMES_H */
