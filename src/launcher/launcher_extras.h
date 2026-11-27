/*
 * launcher_extras.h -- a game's extras: manuals, videos, pictures and links
 * the player (or an install) puts beside it, for the hero layout's Extras
 * (launcher_extras.c).
 *
 * Two folders are read, in this order:
 *   - extras/<title id> beside the launcher: what an install brings;
 *   - the player's own: %APPDATA%\xboxrecomp\extras\<title id> on Windows,
 *     ~/Library/Application Support/xboxrecomp/extras/<id> on macOS,
 *     $XDG_DATA_HOME (or ~/.local/share)/xboxrecomp/extras/<id> elsewhere.
 * Every file and folder in them is an extra, opened with the program the
 * system has for it. An optional extras.json in a folder names, orders and
 * sorts them, gives each a picture and a line about it, and can add links
 * that are not files:
 *
 *   { "items": [
 *       { "title": "Instruction manual", "file": "manual.pdf",
 *         "section": "Manuals", "picture": "manual.jpg",
 *         "about": "The manual from the game's case." },
 *       { "title": "Making of", "url": "https://...", "section": "Behind the scenes" } ] }
 *
 * Without it: a picture with the same name as another file (manual.jpg
 * beside manual.pdf) is that file's picture; a folder's picture is the first
 * picture in it; the section comes from the kind of file.
 */
#ifndef XBOXRECOMP_LAUNCHER_EXTRAS_H
#define XBOXRECOMP_LAUNCHER_EXTRAS_H

#include <stddef.h>
#include <stdint.h>

#define EXTRAS_MAX 96

enum { EXTRA_MANUAL, EXTRA_VIDEO, EXTRA_PICTURE, EXTRA_LINK, EXTRA_FOLDER, EXTRA_OTHER };

typedef struct Extra {
    char title[96];
    char about[240];
    char section[48];
    char target[1024];          /* the file, the folder or the address it opens */
    char picture[1024];         /* its picture file, or "" */
    int  kind;
} Extra;

/* The title's extras, both folders, the manifests' first: how many. */
int extras_scan(uint32_t title_id, Extra *out, int max);
/* The player's extras folder for the title; made, with its parents, when
 * `make`. 0 when there is no home to put it in. */
int extras_dir(uint32_t title_id, char *out, size_t n, int make);
/* Open it with the system. 1 when it opened. */
int extras_open(const Extra *e);
/* "MANUAL", "VIDEO", "PICTURE", "LINK", "FOLDER", "FILE". */
const char *extras_kind_name(int kind);

#endif /* XBOXRECOMP_LAUNCHER_EXTRAS_H */
