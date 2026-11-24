/*
 * launcher_art.h -- a title's backdrop and logo for the launcher, found or
 * fetched in the background (launcher_art.c).
 */
#ifndef XBOXRECOMP_LAUNCHER_ART_H
#define XBOXRECOMP_LAUNCHER_ART_H
#include <stdint.h>

/* Look for the pictures, and start fetching any missing. Once, after the
 * title's id is known. */
void launcher_art_init(uint32_t title_id);
/* Each tick: 1 when a fetch finished, so the window should be drawn again. */
int  launcher_art_tick(void);
/* 0xAARRGGBB rows, or NULL while there is none. Owned here. */
const uint32_t *launcher_art_backdrop(int *w, int *h);
const uint32_t *launcher_art_logo(int *w, int *h);

#endif /* XBOXRECOMP_LAUNCHER_ART_H */
