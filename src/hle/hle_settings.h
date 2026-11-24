/*
 * hle_settings.h - the settings a player can change while a title runs.
 *
 * What the desktop's F9 and F10 do, for a host with no keyboard: Android's
 * in-game menu (src/host/android_main.c) calls these. Each takes effect at
 * the next frame and needs no restart. Any thread.
 */
#ifndef XBOXRECOMP_HLE_SETTINGS_H
#define XBOXRECOMP_HLE_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The frame-rate counter in the top-left corner (F9). */
int  hle_fps_overlay(void);
void hle_set_fps_overlay(int on);

/* The field of view (the fov setting), for a title that applies it: the
 * title says so with its own default (vertical degrees), and asks each time
 * it builds a camera. hle_fov answers the setting, or 0 for the game's own.
 * Online the setting is the session's, which the title checks with the other
 * machine, so the menu does not change it then. */
void hle_fov_title_default(int degrees);
int  hle_fov_title_has(void);
int  hle_fov(void);
void hle_set_fov(int degrees);

/* The frame cap (F10): "adaptive", "60", "30" or "off". */
const char *hle_frame_cap(void);
void        hle_set_frame_cap(const char *name);

#ifdef __cplusplus
}
#endif

#endif
