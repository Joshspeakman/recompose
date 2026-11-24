/*
 * launcher_art.c -- a title's artwork behind the launcher: a backdrop, and a
 * logo where its name would be written.
 *
 * Where they come from, in order:
 *   - an art folder beside the launcher (art/backdrop.jpg, art/logo.png): the
 *     player's own, or what a title's tools made from the player's own copy
 *     of the game (Timeslip's mac/make_art.sh);
 *   - the player's art cache (below), the same;
 *   - for the logo, the title's own, compiled in (recomp_add_launcher LOGO:
 *     the project's mark, not the game's);
 *   - only if the player asks (RECOMP_LAUNCHER_FETCH_ART=1), a download into
 *     the cache from where the title's theme header says
 *     (THEME_ART_BACKDROP_URL, THEME_ART_LOGO_URL), in the background, with
 *     the system's curl. A game's key art is its publisher's, so nothing
 *     downloads it unasked.
 * Nothing waits on any of it: without a backdrop the launcher draws its own
 * background, and without a logo it writes the game's name.
 *
 * Cache: %LOCALAPPDATA%\xboxrecomp\art\<title id> on Windows,
 * ~/Library/Application Support/xboxrecomp/art/<id> on macOS,
 * $XDG_DATA_HOME (or ~/.local/share)/xboxrecomp/art/<id> elsewhere.
 * RECOMP_LAUNCHER_NO_ART=1 leaves the pictures out entirely.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_art.h"
#include "launcher_image.h"
#include "launcher_theme.h"
#include "platform/host_main.h"
#ifdef LAUNCHER_LOGO_HEADER
#include LAUNCHER_LOGO_HEADER   /* launcher_logo_png[], from recomp_add_launcher LOGO */
#endif

#define ART_PATH 1024

typedef struct {
    const char *url;          /* where it is fetched from, or NULL */
    const char *name;         /* backdrop / logo, with the URL's extension */
    char        file[ART_PATH];
    uint32_t   *px;
    int         w, h;
    int         fetching, done;
#ifdef _WIN32
    HANDLE      child;
#else
    pid_t       child;
#endif
} Art;

static Art g_art[2];
static int g_on;
static char g_here[ART_PATH], g_dir[ART_PATH];   /* beside the launcher; the art cache */

static const char *ext_of(const char *url)
{
    const char *dot = url ? strrchr(url, '.') : NULL;

    return dot && (!strcmp(dot, ".png") || !strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"))
           ? dot : ".png";
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int cache_dir(uint32_t id, char *out, size_t n)
{
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");

    if (!local || !*local)
        return 0;
    snprintf(out, n, "%s\\xboxrecomp\\art\\%08X", local, id);
    return 1;
#else
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
#if defined(__APPLE__)
    if (home && *home) {
        snprintf(out, n, "%s/Library/Application Support/xboxrecomp/art/%08X", home, id);
        return 1;
    }
#endif
    if (data && *data)
        snprintf(out, n, "%s/xboxrecomp/art/%08X", data, id);
    else if (home && *home)
        snprintf(out, n, "%s/.local/share/xboxrecomp/art/%08X", home, id);
    else
        return 0;
    return 1;
#endif
}

static void load(Art *a)
{
    a->px = launcher_image_load(a->file, &a->w, &a->h);
    a->done = 1;
}

/* curl -sfL into <file>.part; moved into place only when it succeeds, so a
 * cut-off download is never shown as a picture. */
static void fetch(Art *a)
{
    char part[ART_PATH + 8];

    snprintf(part, sizeof part, "%s.part", a->file);
#ifdef _WIN32
    {
        char cmd[ART_PATH * 2 + 128];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        snprintf(cmd, sizeof cmd, "curl.exe -sfL --max-time 30 --create-dirs -o \"%s\" \"%s\"",
                 part, a->url);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            return;
        CloseHandle(pi.hThread);
        a->child = pi.hProcess;
        a->fetching = 1;
    }
#else
    {
        char *argv[] = { "curl", "-sfL", "--max-time", "30", "--create-dirs", "-o", part,
                         (char *)a->url, NULL };
        pid_t pid;

        if (posix_spawnp(&pid, "curl", NULL, NULL, argv, environ) != 0)
            return;
        a->child = pid;
        a->fetching = 1;
    }
#endif
}

/* 1 when the fetch has finished (the picture is loaded if it worked). */
static int fetched(Art *a)
{
    char part[ART_PATH + 8];
    int ok;

    snprintf(part, sizeof part, "%s.part", a->file);
#ifdef _WIN32
    {
        DWORD code = 1;

        if (WaitForSingleObject(a->child, 0) != WAIT_OBJECT_0)
            return 0;
        GetExitCodeProcess(a->child, &code);
        CloseHandle(a->child);
        ok = code == 0 && MoveFileExA(part, a->file, MOVEFILE_REPLACE_EXISTING);
        if (!ok)
            DeleteFileA(part);
    }
#else
    {
        int st = 0;
        pid_t r = waitpid(a->child, &st, WNOHANG);

        if (r == 0)
            return 0;
        ok = r == a->child && WIFEXITED(st) && WEXITSTATUS(st) == 0 && rename(part, a->file) == 0;
        if (!ok)
            unlink(part);
    }
#endif
    a->fetching = 0;
    if (ok)
        load(a);
    else
        a->done = 1;           /* tried; the next start tries again */
    return 1;
}

void launcher_art_init(uint32_t title_id)
{
    char dir[ART_PATH], here[ART_PATH];
    const char *off = getenv("RECOMP_LAUNCHER_NO_ART");
    const char *want = getenv("RECOMP_LAUNCHER_FETCH_ART");
    int i, may_fetch = want && *want && strcmp(want, "0") != 0;

#ifdef THEME_ART_BACKDROP_URL
    g_art[0].url = THEME_ART_BACKDROP_URL;
#endif
#ifdef THEME_ART_LOGO_URL
    g_art[1].url = THEME_ART_LOGO_URL;
#endif
    g_art[0].name = "backdrop";
    g_art[1].name = "logo";
    if (off && *off && strcmp(off, "0") != 0)
        return;
    g_on = 1;
    if (!host_exe_path(here, sizeof here))
        here[0] = '\0';
    else
        host_path_dirname(here);
    if (!cache_dir(title_id, dir, sizeof dir))
        dir[0] = '\0';
    snprintf(g_here, sizeof g_here, "%s", here);
    snprintf(g_dir, sizeof g_dir, "%s", dir);

    for (i = 0; i < 2; i++) {
        Art *a = &g_art[i];
        const char *ext = ext_of(a->url);

        /* Beside the launcher first: art/backdrop.jpg or .png. */
        if (here[0]) {
            snprintf(a->file, sizeof a->file, "%s/art/%s.jpg", here, a->name);
            if (!file_exists(a->file))
                snprintf(a->file, sizeof a->file, "%s/art/%s.png", here, a->name);
            if (file_exists(a->file)) {
                load(a);
                continue;
            }
        }
        if (!dir[0]) {
            a->done = 1;
            continue;
        }
        /* Then the art cache, whoever put it there (a title's own tools,
         * from the player's disc; or an earlier fetch). */
        snprintf(a->file, sizeof a->file, "%s/%s.jpg", dir, a->name);
        if (!file_exists(a->file))
            snprintf(a->file, sizeof a->file, "%s/%s.png", dir, a->name);
        if (file_exists(a->file)) {
            load(a);
            continue;
        }
        if (!a->url) {
            a->done = 1;
            continue;
        }
#ifdef _WIN32
        snprintf(a->file, sizeof a->file, "%s\\%s%s", dir, a->name, ext);
#else
        snprintf(a->file, sizeof a->file, "%s/%s%s", dir, a->name, ext);
#endif
        if (may_fetch)
            fetch(a);
        else
            a->done = 1;
    }
#ifdef LAUNCHER_LOGO_HEADER
    /* The title's own mark where there is no other logo. */
    if (!g_art[1].px && !g_art[1].fetching)
        g_art[1].px = launcher_image_decode(launcher_logo_png, sizeof launcher_logo_png,
                                            &g_art[1].w, &g_art[1].h);
#endif
}

int launcher_art_tick(void)
{
    int i, changed = 0;

    if (!g_on)
        return 0;
    for (i = 0; i < 2; i++)
        if (g_art[i].fetching && fetched(&g_art[i]))
            changed = 1;
    return changed;
}

const uint32_t *launcher_art_backdrop(int *w, int *h)
{
    *w = g_art[0].w;
    *h = g_art[0].h;
    return g_art[0].px;
}

const uint32_t *launcher_art_logo(int *w, int *h)
{
    *w = g_art[1].w;
    *h = g_art[1].h;
    return g_art[1].px;
}

/* Any other picture by name (the showcase's glow and its cards): looked for
 * as <name>.jpg or .png beside the launcher (art/), then in the art cache,
 * the first time it is asked for, and kept. */
const uint32_t *launcher_art_image(const char *name, int *w, int *h)
{
    static struct { char name[32]; uint32_t *px; int w, h, tried; } kept[12];
    char path[ART_PATH + 64];
    int i, j;

    *w = *h = 0;
    if (!g_on || !name)
        return NULL;
    if (!strcmp(name, "backdrop"))
        return launcher_art_backdrop(w, h);
    if (!strcmp(name, "logo"))
        return launcher_art_logo(w, h);
    for (i = 0; i < 12 && kept[i].tried; i++)
        if (!strcmp(kept[i].name, name)) {
            *w = kept[i].w;
            *h = kept[i].h;
            return kept[i].px;
        }
    if (i == 12)
        return NULL;
    snprintf(kept[i].name, sizeof kept[i].name, "%s", name);
    kept[i].tried = 1;
    for (j = 0; j < 4 && !kept[i].px; j++) {
        const char *base = j < 2 ? g_here : g_dir, *ext = (j & 1) ? "png" : "jpg";

        if (!base[0])
            continue;
        snprintf(path, sizeof path, j < 2 ? "%s/art/%s.%s" : "%s/%s.%s", base, name, ext);
        if (file_exists(path))
            kept[i].px = launcher_image_load(path, &kept[i].w, &kept[i].h);
    }
    *w = kept[i].w;
    *h = kept[i].h;
    return kept[i].px;
}
