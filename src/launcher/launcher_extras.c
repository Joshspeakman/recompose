/*
 * launcher_extras.c -- see launcher_extras.h.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher.h"
#include "launcher_extras.h"
#include "platform/host_main.h"

#define EX_PATH 1024
#define EX_NAMES 256

/* ------------------------------------------------------------ the kinds */

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot && dot != name ? dot + 1 : "";
}

static int ext_in(const char *ext, const char *const *list)
{
    int i;

    for (i = 0; list[i]; i++) {
        const char *a = ext, *b = list[i];

        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b)
            return 1;
    }
    return 0;
}

static const char *const k_manual[] = { "pdf", "epub", "xps", "djvu", "cbz", "cbr", NULL };
static const char *const k_video[] = { "mp4", "m4v", "mkv", "webm", "mov", "avi", "wmv", "mpg", "mpeg", NULL };
static const char *const k_picture[] = { "jpg", "jpeg", "png", "gif", "webp", "bmp", NULL };
static const char *const k_link[] = { "url", "webloc", NULL };

static int kind_of_name(const char *name)
{
    const char *e = ext_of(name);

    if (ext_in(e, k_manual))  return EXTRA_MANUAL;
    if (ext_in(e, k_video))   return EXTRA_VIDEO;
    if (ext_in(e, k_picture)) return EXTRA_PICTURE;
    if (ext_in(e, k_link))    return EXTRA_LINK;
    return EXTRA_OTHER;
}

static const char *section_of_kind(int kind)
{
    switch (kind) {
    case EXTRA_MANUAL:  return "Manuals";
    case EXTRA_VIDEO:   return "Videos";
    case EXTRA_PICTURE:
    case EXTRA_FOLDER:  return "Gallery";
    case EXTRA_LINK:    return "Links";
    default:            return "Other";
    }
}

const char *extras_kind_name(int kind)
{
    switch (kind) {
    case EXTRA_MANUAL:  return "MANUAL";
    case EXTRA_VIDEO:   return "VIDEO";
    case EXTRA_PICTURE: return "PICTURE";
    case EXTRA_LINK:    return "LINK";
    case EXTRA_FOLDER:  return "FOLDER";
    default:            return "FILE";
    }
}

/* ------------------------------------------------------------- folders */

int extras_dir(uint32_t id, char *out, size_t n, int make)
{
#if defined(_WIN32)
    const char *roam = getenv("APPDATA");
    char part[EX_PATH];

    if (!roam || !*roam)
        return 0;
    snprintf(out, n, "%s\\xboxrecomp\\extras\\%08X", roam, (unsigned)id);
    if (make) {
        snprintf(part, sizeof part, "%s\\xboxrecomp", roam);
        CreateDirectoryA(part, NULL);
        snprintf(part, sizeof part, "%s\\xboxrecomp\\extras", roam);
        CreateDirectoryA(part, NULL);
        CreateDirectoryA(out, NULL);
    }
    return 1;
#else
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char base[EX_PATH], part[EX_PATH];

#if defined(__APPLE__)
    (void)data;
    if (home && *home)
        snprintf(base, sizeof base, "%s/Library/Application Support/xboxrecomp", home);
    else
        return 0;
#else
    if (data && *data)
        snprintf(base, sizeof base, "%s/xboxrecomp", data);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.local/share/xboxrecomp", home);
    else
        return 0;
#endif
    snprintf(out, n, "%s/extras/%08X", base, (unsigned)id);
    if (make) {
        mkdir(base, 0755);
        snprintf(part, sizeof part, "%s/extras", base);
        mkdir(part, 0755);
        mkdir(out, 0755);
    }
    return 1;
#endif
}

static int is_dir(const char *path)
{
#ifdef _WIN32
    WCHAR w[EX_PATH];
    DWORD a;

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, EX_PATH))
        return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int is_file(const char *path)
{
#ifdef _WIN32
    WCHAR w[EX_PATH];
    DWORD a;

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, EX_PATH))
        return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int icmp(const char *x, const char *y)
{
    for (; *x && *y; x++, y++) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);

        if (cx != cy)
            return cx - cy;
    }
    return tolower((unsigned char)*x) - tolower((unsigned char)*y);
}

static int name_cmp(const void *a, const void *b)
{
    return icmp(*(const char *const *)a, *(const char *const *)b);
}

/* A folder's entries (not "." or "..", nor hidden ones), sorted, as UTF-8
 * names the caller frees. */
static int list_dir(const char *dir, char **names, int max)
{
    int n = 0;
#ifdef _WIN32
    WCHAR pat[EX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char pattern[EX_PATH];

    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    if (!MultiByteToWideChar(CP_UTF8, 0, pattern, -1, pat, EX_PATH))
        return 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char name[512];

        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        if (n < max && (names[n] = malloc(strlen(name) + 1)) != NULL)
            strcpy(names[n++], name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (n < max && (names[n] = malloc(strlen(e->d_name) + 1)) != NULL)
            strcpy(names[n++], e->d_name);
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    return n;
}

static void free_names(char **names, int n)
{
    while (n-- > 0)
        free(names[n]);
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
#ifdef _WIN32
    snprintf(out, n, "%s\\%s", dir, name);
#else
    snprintf(out, n, "%s/%s", dir, name);
#endif
}

static char *read_text(const char *path, size_t limit)
{
    FILE *f;
    char *buf;
    long size;

#ifdef _WIN32
    WCHAR w[EX_PATH];

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, EX_PATH) || !(f = _wfopen(w, L"rb")))
        return NULL;
#else
    if (!(f = fopen(path, "rb")))
        return NULL;
#endif
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || (size_t)size > limit || !(buf = malloc((size_t)size + 1))) {
        fclose(f);
        return NULL;
    }
    size = (long)fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------ manifest */

/* A JSON string at p (its opening quote), unescaped into out; past it. */
static const char *json_string(const char *p, char *out, size_t n)
{
    size_t k = 0;

    if (*p != '"')
        return NULL;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;

        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                int i;

                for (i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0'
                                                                         : (tolower((unsigned char)p[i]) - 'a' + 10));
                p += i - 1;
                if (v < 0x80) c = v;
                else {                                   /* to UTF-8 */
                    char u[3];
                    int m = 0, j;

                    if (v < 0x800) { u[m++] = (char)(0xC0 | (v >> 6)); }
                    else { u[m++] = (char)(0xE0 | (v >> 12)); u[m++] = (char)(0x80 | ((v >> 6) & 0x3F)); }
                    u[m++] = (char)(0x80 | (v & 0x3F));
                    for (j = 0; j < m; j++)
                        if (k + 1 < n) out[k++] = u[j];
                    continue;
                }
                break;
            }
            default: c = (unsigned char)*p; break;     /* \" \\ \/ */
            }
        }
        if (k + 1 < n)
            out[k++] = (char)c;
    }
    if (n)
        out[k] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

/* The string field `key` of the flat object in [obj, end), or 0. */
static int field(const char *obj, const char *end, const char *key, char *out, size_t n)
{
    const char *p = obj;
    size_t kl = strlen(key);

    while (p && p < end) {
        const char *q = strchr(p, '"');
        char name[64];

        if (!q || q >= end)
            return 0;
        p = json_string(q, name, sizeof name);
        if (!p)
            return 0;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p != ':')
            continue;                                   /* a value, not a key */
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (strlen(name) == kl && !memcmp(name, key, kl) && *p == '"')
            return json_string(p, out, n) != NULL;
        if (*p == '"')
            p = json_string(p, name, sizeof name);
    }
    return 0;
}

/* The end of the object or array opening at p, strings skipped. */
static const char *close_of(const char *p)
{
    int depth = 0;

    for (; *p; p++) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1])
                    p++;
            if (!*p)
                return NULL;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0)
                return p;
        }
    }
    return NULL;
}

static void title_from(const char *name, char *out, size_t n)
{
    size_t k = 0;
    const char *dot = strrchr(name, '.');
    const char *end = dot && dot != name ? dot : name + strlen(name);

    for (; name < end && k + 1 < n; name++)
        out[k++] = (*name == '_' || *name == '-') ? ' ' : *name;
    out[k] = '\0';
}

static int listed(const Extra *out, int count, const char *target)
{
    int i;

    for (i = 0; i < count; i++)
        if (!icmp(out[i].target, target) || !icmp(out[i].picture, target))
            return 1;
    return 0;
}

/* A .url's address (its URL= line), so it opens the same on every system. */
static void url_of(const char *path, char *out, size_t n)
{
    char *text = read_text(path, 64 * 1024), *p;

    if (!text)
        return;
    for (p = text; p && *p; ) {
        if (!strncmp(p, "URL=", 4)) {
            size_t k = 0;

            for (p += 4; *p && *p != '\r' && *p != '\n' && k + 1 < n; p++)
                out[k++] = *p;
            out[k] = '\0';
            break;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(text);
}

/* A picture in dir with the same name as `file` (manual.jpg beside manual.pdf). */
static void picture_beside(const char *dir, const char *file, char *out, size_t n)
{
    static const char *const ext[] = { "jpg", "jpeg", "png", "webp", "gif", "bmp", NULL };
    char stem[512], path[EX_PATH];
    const char *dot = strrchr(file, '.');
    int i;

    snprintf(stem, sizeof stem, "%.*s", (int)(dot && dot != file ? dot - file : (int)strlen(file)), file);
    for (i = 0; ext[i]; i++) {
        char name[600];

        snprintf(name, sizeof name, "%s.%s", stem, ext[i]);
        join(path, sizeof path, dir, name);
        if (is_file(path)) {
            snprintf(out, n, "%s", path);
            return;
        }
    }
}

static void add_manifest(const char *dir, Extra *out, int *count, int max)
{
    char path[EX_PATH], v[EX_PATH];
    char *text;
    const char *p, *end;

    join(path, sizeof path, dir, "extras.json");
    if (!(text = read_text(path, 1024 * 1024)))
        return;
    p = strstr(text, "\"items\"");
    p = p ? strchr(p, '[') : strchr(text, '[');
    end = p ? close_of(p) : NULL;
    while (p && end && *count < max) {
        const char *o = strchr(p, '{'), *oe;
        Extra *e = &out[*count];

        if (!o || o > end || !(oe = close_of(o)))
            break;
        memset(e, 0, sizeof *e);
        if (field(o, oe, "url", v, sizeof v) && v[0]) {
            snprintf(e->target, sizeof e->target, "%s", v);
            e->kind = EXTRA_LINK;
        } else if (field(o, oe, "file", v, sizeof v) && v[0]) {
            join(e->target, sizeof e->target, dir, v);
            e->kind = is_dir(e->target) ? EXTRA_FOLDER : kind_of_name(v);
            if (!is_dir(e->target) && !is_file(e->target)) {
                p = oe + 1;                              /* named, but not there */
                continue;
            }
        } else {
            p = oe + 1;
            continue;
        }
        if (!field(o, oe, "title", e->title, sizeof e->title))
            title_from(v, e->title, sizeof e->title);
        field(o, oe, "about", e->about, sizeof e->about);
        if (!field(o, oe, "section", e->section, sizeof e->section))
            snprintf(e->section, sizeof e->section, "%s", section_of_kind(e->kind));
        {
            char file[EX_PATH];

            file[0] = '\0';
            field(o, oe, "file", file, sizeof file);
            if (field(o, oe, "picture", v, sizeof v) && v[0])
                join(e->picture, sizeof e->picture, dir, v);
            else if (e->kind == EXTRA_PICTURE)
                snprintf(e->picture, sizeof e->picture, "%s", e->target);
            else if (file[0] && e->kind != EXTRA_FOLDER)
                picture_beside(dir, file, e->picture, sizeof e->picture);
        }
        (*count)++;
        p = oe + 1;
    }
    free(text);
}

static void add_folder(const char *dir, Extra *out, int *count, int max)
{
    char *names[EX_NAMES];
    int n, i, j;

    if (!is_dir(dir))
        return;
    add_manifest(dir, out, count, max);
    n = list_dir(dir, names, EX_NAMES);
    for (i = 0; i < n && *count < max; i++) {
        char path[EX_PATH];
        Extra *e = &out[*count];
        int kind;

        if (!icmp(names[i], "extras.json"))
            continue;
        join(path, sizeof path, dir, names[i]);
        if (listed(out, *count, path))
            continue;
        kind = is_dir(path) ? EXTRA_FOLDER : kind_of_name(names[i]);
        /* A picture named as another file is that file's picture. */
        if (kind == EXTRA_PICTURE) {
            const char *dot = strrchr(names[i], '.');
            size_t stem = dot ? (size_t)(dot - names[i]) : strlen(names[i]);
            int paired = 0;

            for (j = 0; j < n; j++) {
                const char *d2 = strrchr(names[j], '.');
                size_t s2 = d2 ? (size_t)(d2 - names[j]) : strlen(names[j]);

                if (j != i && s2 == stem && !strncmp(names[i], names[j], stem) &&
                    kind_of_name(names[j]) != EXTRA_PICTURE)
                    paired = 1;
            }
            if (paired)
                continue;
        }
        memset(e, 0, sizeof *e);
        e->kind = kind;
        snprintf(e->target, sizeof e->target, "%s", path);
        title_from(names[i], e->title, sizeof e->title);
        if (kind == EXTRA_FOLDER)
            snprintf(e->title, sizeof e->title, "%s", names[i]);
        snprintf(e->section, sizeof e->section, "%s", section_of_kind(kind));
        if (kind == EXTRA_LINK)
            url_of(path, e->target, sizeof e->target);
        if (kind == EXTRA_PICTURE) {
            snprintf(e->picture, sizeof e->picture, "%s", path);
        } else if (kind == EXTRA_FOLDER) {
            char *inner[EX_NAMES];
            int m = list_dir(path, inner, EX_NAMES), k;

            for (k = 0; k < m; k++)
                if (kind_of_name(inner[k]) == EXTRA_PICTURE) {
                    join(e->picture, sizeof e->picture, path, inner[k]);
                    break;
                }
            free_names(inner, m);
        } else {
            /* Its picture: the same name, as a picture. */
            const char *dot = strrchr(names[i], '.');
            size_t stem = dot ? (size_t)(dot - names[i]) : strlen(names[i]);

            for (j = 0; j < n; j++) {
                const char *d2 = strrchr(names[j], '.');
                size_t s2 = d2 ? (size_t)(d2 - names[j]) : strlen(names[j]);

                if (j != i && s2 == stem && !strncmp(names[i], names[j], stem) &&
                    kind_of_name(names[j]) == EXTRA_PICTURE) {
                    join(e->picture, sizeof e->picture, dir, names[j]);
                    break;
                }
            }
        }
        (*count)++;
    }
    free_names(names, n);
}

int extras_scan(uint32_t id, Extra *out, int max)
{
    char here[EX_PATH], dir[EX_PATH];
    int count = 0;

    if (host_exe_path(here, sizeof here)) {
        char sub[EX_PATH];

        host_path_dirname(here);
        snprintf(sub, sizeof sub, "%08X", (unsigned)id);
        join(dir, sizeof dir, here, "extras");
        join(here, sizeof here, dir, sub);
        add_folder(here, out, &count, max);
    }
    if (extras_dir(id, dir, sizeof dir, 0))
        add_folder(dir, out, &count, max);
    return count;
}

int extras_open(const Extra *e)
{
    if (!e || !e->target[0])
        return 0;
    return lhost_open(e->target);               /* a folder opens in the file browser */
}
