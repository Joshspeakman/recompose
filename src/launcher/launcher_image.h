/*
 * launcher_image.h -- a JPEG or PNG file as 0xAARRGGBB rows, top row first
 * (launcher_image.c). NULL when it cannot be read or decoded; the caller
 * frees what it gets.
 */
#ifndef XBOXRECOMP_LAUNCHER_IMAGE_H
#define XBOXRECOMP_LAUNCHER_IMAGE_H
#include <stdint.h>

uint32_t *launcher_image_load(const char *path, int *w, int *h);

#endif /* XBOXRECOMP_LAUNCHER_IMAGE_H */
