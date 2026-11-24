/*
 * launcher_image.h -- a JPEG or PNG file as 0xAARRGGBB rows, top row first
 * (launcher_image.c). NULL when it cannot be read or decoded; the caller
 * frees what it gets.
 */
#ifndef XBOXRECOMP_LAUNCHER_IMAGE_H
#define XBOXRECOMP_LAUNCHER_IMAGE_H
#include <stddef.h>
#include <stdint.h>

uint32_t *launcher_image_load(const char *path, int *w, int *h);
/* The same from bytes in memory: a picture compiled into the launcher. */
uint32_t *launcher_image_decode(const unsigned char *bytes, size_t size, int *w, int *h);

#endif /* XBOXRECOMP_LAUNCHER_IMAGE_H */
