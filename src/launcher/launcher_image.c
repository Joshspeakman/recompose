/*
 * launcher_image.c -- pictures for the launcher: a title's artwork (a JPEG or
 * PNG, launcher_art.c) decoded into 0xAARRGGBB rows for launcher_theme.c.
 *
 * stb_image (third_party/stb, public domain), JPEG and PNG only, from files
 * the launcher fetched or found itself. A picture larger than 4096 on a side,
 * or one that does not decode, is refused rather than drawn.
 */
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "launcher_image.h"

#include <stdio.h>
#include <stdlib.h>

uint32_t *launcher_image_decode(const unsigned char *bytes, size_t size, int *w, int *h)
{
    unsigned char *rgba;
    uint32_t *out;
    int x, y, n, i;

    if (!bytes || !size || size > 32u * 1024 * 1024)
        return NULL;
    rgba = stbi_load_from_memory(bytes, (int)size, &x, &y, &n, 4);
    if (!rgba)
        return NULL;
    out = malloc((size_t)x * y * sizeof *out);
    if (out) {
        for (i = 0; i < x * y; i++)
            out[i] = ((uint32_t)rgba[i * 4 + 3] << 24) | ((uint32_t)rgba[i * 4] << 16) |
                     ((uint32_t)rgba[i * 4 + 1] << 8) | (uint32_t)rgba[i * 4 + 2];
        *w = x;
        *h = y;
    }
    stbi_image_free(rgba);
    return out;
}

uint32_t *launcher_image_load(const char *path, int *w, int *h)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    unsigned char *bytes = NULL, *rgba;
    uint32_t *out = NULL;
    long size;
    int x, y, n, i;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size < 32L * 1024 * 1024 &&
        fseek(f, 0, SEEK_SET) == 0 && (bytes = malloc((size_t)size)) != NULL &&
        fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        free(bytes);
        bytes = NULL;
    }
    fclose(f);
    if (!bytes)
        return NULL;
    rgba = stbi_load_from_memory(bytes, (int)size, &x, &y, &n, 4);
    free(bytes);
    if (!rgba)
        return NULL;
    out = malloc((size_t)x * y * sizeof *out);
    if (out) {
        for (i = 0; i < x * y; i++)
            out[i] = ((uint32_t)rgba[i * 4 + 3] << 24) | ((uint32_t)rgba[i * 4] << 16) |
                     ((uint32_t)rgba[i * 4 + 1] << 8) | (uint32_t)rgba[i * 4 + 2];
        *w = x;
        *h = y;
    }
    stbi_image_free(rgba);
    return out;
}
