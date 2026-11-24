/*
 * recomp_lockstep_net.c -- the controllers between machines in a lockstep
 * session (recomp_lockstep.h).
 *
 * One copy hosts, the others join it. Every machine runs the whole game;
 * what crosses the network is each machine's own controller, one sample per
 * frame, and a hash of the game every 60 frames to prove the copies still
 * agree. A frame starts only when every player's controller for it is in, so
 * a machine that falls behind holds the others at the frame boundary rather
 * than letting the games drift apart.
 *
 *   RECOMP_LOCKSTEP_NET=host           listen for the other player
 *   RECOMP_LOCKSTEP_NET=join           RECOMP_LOCKSTEP_PEER=<address>[:port]
 *   RECOMP_LOCKSTEP_PORT=27620         the host's UDP port
 *   RECOMP_LOCKSTEP_DELAY=4            frames between a press and the frame it
 *                                      lands in, on every machine: time for it
 *                                      to cross the network
 *
 * The host plays controller 1 and the joiner controller 2, each with its own
 * machine's controller 1. Inputs for the first DELAY frames are neutral on
 * every machine, so each side runs that far ahead before it has to hear from
 * the other.
 *
 * Both machines start the game from the host's saves. Before its game
 * starts the host takes this title's folders from its hard disk (E:\UDATA\
 * <title id> and E:\TDATA\<title id>); a joiner receives them before its own
 * game starts, writes them into a session folder (<data dir>/coop-session/
 * <title id>) and plays from that as its E: (recomp_lockstep_hdd_dir). The
 * joiner's own saves are not touched, and what its game saves during the
 * session stays in the session folder. Settings that change the game itself
 * (widescreen widens the title's own camera) must match too: the host sends
 * its own, and a joiner whose differ is refused and told which.
 *
 * Each message is UDP, 'X' 'L' 'S' version type, then:
 *   HELLO    joiner -> host: the build, so two different builds never play
 *   WELCOME  host -> joiner: the session's epoch (its clock), the delay, the
 *            joiner's controller, the settings, and the saves' size and hash
 *   REFUSE   host -> joiner: why not (a different build, a full session)
 *   SAVEREQ  joiner -> host: up to 64 chunks of the saves it still needs
 *   SAVE     host -> joiner: one chunk (1 KB) of the saves
 *   INPUT    both ways, every frame and every 50 ms while waiting: the
 *            sender's controller for the frames the other side has not
 *            acknowledged (up to 32), what it has received of the other's,
 *            and its latest checkpoint hash
 *   BYE      leaving
 * Loss costs nothing but a resend: every INPUT carries everything not yet
 * acknowledged, and a joiner asks again for any chunk that has not come.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET ls_socket;
#define LS_BAD_SOCKET INVALID_SOCKET
#define ls_close closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include "win32_compat.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
typedef int ls_socket;
#define LS_BAD_SOCKET (-1)
#define ls_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "recomp_lockstep.h"
#include "recomp_lockstep_net.h"
#include "recomp_config.h"
#include "net_upnp.h"

/* kernel_path.c / kernel_xbox.c: E:'s folder, the console's disk key. */
int xbox_path_hdd_dir_utf8(char *out, size_t n);
int xbox_path_save_dir_utf8(char *out, size_t n);
extern unsigned char xbox_HDKey[16];

#define LS_VERSION      2u
#define DEFAULT_PORT    27620
#define DEFAULT_DELAY   4u
#define RING            512u          /* frames of input kept per player */
#define MAX_SEND        32u           /* frames of input in one message */
#define CHECKPOINTS     64u           /* recent hashes kept for the other's to meet */
#define CHUNK_BYTES     1024u         /* one SAVE message's share of the saves */
#define MAX_SAVES       (32u << 20)   /* the most a host hands over */
#define MAX_SAVE_FILES  4096
#define SETTINGS_LEN    256

enum { T_HELLO = 1, T_WELCOME, T_REFUSE, T_INPUT, T_BYE, T_SAVEREQ, T_SAVE };

typedef struct Checkpoint { uint32_t tick; uint64_t hash; } Checkpoint;

static struct {
    int on, host;
    ls_socket sock;
    struct sockaddr_in peer;
    int have_peer;
    unsigned local_port, remote_port;
    uint32_t delay;
    int64_t epoch;
    char build[64];
    /* inputs, by tick % RING */
    RecompLockstepPad local[RING], remote[RING];
    uint32_t local_upto;              /* latest tick with this machine's input */
    uint32_t remote_upto;             /* latest tick with all of the other's before it */
    uint32_t remote_acked;            /* latest of ours the other has */
    Checkpoint mine[CHECKPOINTS], theirs;
    uint32_t desyncs, first_desync;
    uint64_t last_heard_ms;
    int gone;                         /* the other player left or went silent */
    int welcomed;
    char refused[160];
    /* the host's saves: its snapshot (host) or what has arrived (joiner) */
    uint8_t *saves;
    uint32_t saves_size;
    uint64_t saves_hash;
    uint8_t *have;                    /* joiner: chunk arrived */
    uint32_t have_count;
    char settings[SETTINGS_LEN];      /* this machine's, name=value;... */
    char host_settings[SETTINGS_LEN]; /* joiner: the host's */
    char session_dir[1024];           /* joiner: its E: for the session */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    HANDLE arrived;                   /* auto-reset: something came in */
} g;

static void lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&g.lock);
#else
    pthread_mutex_lock(&g.lock);
#endif
}

static void unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&g.lock);
#else
    pthread_mutex_unlock(&g.lock);
#endif
}

static uint64_t now_ms(void) { return GetTickCount64(); }

#define say(...) do { fprintf(stderr, "[LOCKSTEP-NET] " __VA_ARGS__); \
                      fputc('\n', stderr); fflush(stderr); } while (0)

/* ── encoding ─────────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void pad_put(uint8_t *b, const RecompLockstepPad *p)
{
    int i;
    put16(b, p->buttons);
    for (i = 0; i < 8; i++)
        b[2 + i] = p->analog[i];
    put16(b + 10, (uint16_t)p->thumb_lx);
    put16(b + 12, (uint16_t)p->thumb_ly);
    put16(b + 14, (uint16_t)p->thumb_rx);
    put16(b + 16, (uint16_t)p->thumb_ry);
}

static void pad_get(RecompLockstepPad *p, const uint8_t *b)
{
    int i;
    p->buttons = get16(b);
    for (i = 0; i < 8; i++)
        p->analog[i] = b[2 + i];
    p->thumb_lx = (int16_t)get16(b + 10);
    p->thumb_ly = (int16_t)get16(b + 12);
    p->thumb_rx = (int16_t)get16(b + 14);
    p->thumb_ry = (int16_t)get16(b + 16);
}

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--)
        h = (h ^ *p++) * 0x100000001B3ull;
    return h;
}

static void header(uint8_t *b, int type)
{
    b[0] = 'X'; b[1] = 'L'; b[2] = 'S'; b[3] = (uint8_t)LS_VERSION; b[4] = (uint8_t)type;
}

static void send_raw(const uint8_t *b, int n)
{
    if (g.have_peer)
        sendto(g.sock, (const char *)b, n, 0, (const struct sockaddr *)&g.peer, sizeof g.peer);
}

static void send_to(const struct sockaddr_in *to, const uint8_t *b, int n)
{
    sendto(g.sock, (const char *)b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

/* ── files: the host's saves ──────────────────────────────────────── */

#ifdef _WIN32
#define PATH_SEP '\\'
static FILE *ufopen(const char *path, const char *mode)
{
    wchar_t wp[1100], wm[8];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1100);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8);
    return _wfopen(wp, wm);
}
#else
#define PATH_SEP '/'
static FILE *ufopen(const char *path, const char *mode) { return fopen(path, mode); }
#endif

typedef void (*DirFn)(void *ctx, const char *name, int is_dir);

/* Every entry of a folder but . and .., in no particular order. */
static int list_dir(const char *dir, DirFn fn, void *ctx)
{
#ifdef _WIN32
    char pat[1100];
    wchar_t wpat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*", dir);
    MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 1100);
    h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char name[600];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        if (strcmp(name, ".") && strcmp(name, ".."))
            fn(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char full[1100];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (stat(full, &st) == 0)
            fn(ctx, e->d_name, S_ISDIR(st.st_mode));
    }
    closedir(d);
    return 1;
#endif
}

typedef struct SaveFile { char rel[512], full[1100]; } SaveFile;
typedef struct SaveList { SaveFile *files; int count, cap; const char *root; char rel[512]; } SaveList;

static void collect(void *ctx, const char *name, int is_dir);

static void collect_dir(SaveList *l, const char *rel)
{
    char full[1100], keep[512];

    snprintf(full, sizeof full, "%s%c%s", l->root, PATH_SEP, rel);
    snprintf(keep, sizeof keep, "%s", l->rel);
    snprintf(l->rel, sizeof l->rel, "%s", rel);
    list_dir(full, collect, l);
    snprintf(l->rel, sizeof l->rel, "%s", keep);
}

static void collect(void *ctx, const char *name, int is_dir)
{
    SaveList *l = (SaveList *)ctx;
    char rel[512];

    snprintf(rel, sizeof rel, "%s/%s", l->rel, name);
    if (is_dir) {
        collect_dir(l, rel);
        return;
    }
    if (l->count >= MAX_SAVE_FILES)
        return;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->files = (SaveFile *)realloc(l->files, (size_t)l->cap * sizeof *l->files);
    }
    snprintf(l->files[l->count].rel, sizeof l->files[0].rel, "%s", rel);
    snprintf(l->files[l->count].full, sizeof l->files[0].full, "%s%c%s", l->root, PATH_SEP, rel);
    l->count++;
}

/* The two top folders (UDATA, TDATA) and, inside, this title's, matched
 * without regard to case as the console's file system does. */
typedef struct TopFind { const char *want; char found[256]; } TopFind;

static void find_named(void *ctx, const char *name, int is_dir)
{
    TopFind *t = (TopFind *)ctx;
    size_t i, n = strlen(t->want);

    if (!is_dir || strlen(name) != n || t->found[0])
        return;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)t->want[i]))
            return;
    snprintf(t->found, sizeof t->found, "%s", name);
}

static int cmp_rel(const void *a, const void *b)
{
    return strcmp(((const SaveFile *)a)->rel, ((const SaveFile *)b)->rel);
}

/* The host, before its game starts: this title's saves, as one block --
 * "XLSF", a count, each file's path and size, then the files' bytes. */
static void snapshot_saves(void)
{
    static const char *const tops[] = { "UDATA", "TDATA" };
    char root[1024], id[16];
    SaveList l;
    uint32_t total = 8;
    int t, i;
    uint8_t *p;

    memset(&l, 0, sizeof l);
    if (!xbox_path_hdd_dir_utf8(root, sizeof root)) {
        say("no hard disk folder to take the saves from; the joiner starts with none");
        return;
    }
    l.root = root;
    snprintf(id, sizeof id, "%08x", (unsigned)recomp_config_title_id());
    for (t = 0; t < 2; t++) {
        TopFind top, title;
        char dir[1100];

        memset(&top, 0, sizeof top);
        top.want = tops[t];
        list_dir(root, find_named, &top);
        if (!top.found[0])
            continue;
        memset(&title, 0, sizeof title);
        title.want = id;
        snprintf(dir, sizeof dir, "%s%c%s", root, PATH_SEP, top.found);
        list_dir(dir, find_named, &title);
        if (!title.found[0])
            continue;
        snprintf(dir, sizeof dir, "%s/%s", top.found, title.found);
        collect_dir(&l, dir);
    }
    qsort(l.files, (size_t)l.count, sizeof *l.files, cmp_rel);
    for (i = 0; i < l.count; i++) {
        FILE *f = ufopen(l.files[i].full, "rb");
        long n = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fclose(f);
        }
        total += 2u + (uint32_t)strlen(l.files[i].rel) + 4u + (uint32_t)(n > 0 ? n : 0);
    }
    if (total > MAX_SAVES) {
        say("the saves come to %u bytes, more than a session hands over; the joiner starts "
            "with none", (unsigned)total);
        free(l.files);
        return;
    }
    g.saves = (uint8_t *)calloc(1, total);
    p = g.saves;
    memcpy(p, "XLSF", 4);
    put32(p + 4, (uint32_t)l.count);
    p += 8;
    for (i = 0; i < l.count; i++) {
        size_t nl = strlen(l.files[i].rel);
        put16(p, (uint16_t)nl);
        memcpy(p + 2, l.files[i].rel, nl);
        p += 2 + nl;
        p += 4;                                   /* the size, written below */
    }
    /* Then each file's size into the table and its bytes after it, in the
     * same order. */
    {
        uint8_t *q = g.saves + 8;
        uint8_t *data = p;
        for (i = 0; i < l.count; i++) {
            size_t nl = get16(q);
            FILE *f = ufopen(l.files[i].full, "rb");
            uint32_t n = 0;
            q += 2 + nl;
            if (f) {
                n = (uint32_t)fread(data, 1, (size_t)(g.saves + total - data), f);
                fclose(f);
            }
            put32(q, n);
            q += 4;
            data += n;
        }
        g.saves_size = (uint32_t)(data - g.saves);
    }
    g.saves_hash = fnv64(g.saves, g.saves_size);
    say("the host's saves for the joiner: %d file(s), %u bytes, from %s", l.count,
        (unsigned)g.saves_size, root);
    free(l.files);
}

static void remove_tree(const char *dir);

static void remove_entry(void *ctx, const char *name, int is_dir)
{
    char full[1100];
    snprintf(full, sizeof full, "%s%c%s", (const char *)ctx, PATH_SEP, name);
    if (is_dir) {
        remove_tree(full);
    } else {
#ifdef _WIN32
        wchar_t w[1100];
        MultiByteToWideChar(CP_UTF8, 0, full, -1, w, 1100);
        DeleteFileW(w);
#else
        remove(full);
#endif
    }
}

/* Empties and removes a folder this file made: only ever the session one. */
static void remove_tree(const char *dir)
{
    list_dir(dir, remove_entry, (void *)dir);
#ifdef _WIN32
    {
        wchar_t w[1100];
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1100);
        RemoveDirectoryW(w);
    }
#else
    rmdir(dir);
#endif
}

/* The joiner: the host's saves into a fresh session folder. */
static int write_saves(void)
{
    char base[1024];
    const uint8_t *p = g.saves, *end = g.saves + g.saves_size;
    const uint8_t *table, *data;
    uint32_t count, i;

    /* Beside this copy's own saves (RECOMP_SAVE_DIR when a test names one,
     * else the data folder), never in place of them. */
    if (!xbox_path_save_dir_utf8(base, sizeof base) && !recomp_data_dir(base, sizeof base))
        return 0;
    snprintf(g.session_dir, sizeof g.session_dir, "%s%ccoop-session%c%08x", base, PATH_SEP,
             PATH_SEP, (unsigned)recomp_config_title_id());
    remove_tree(g.session_dir);
    if (!recomp_make_dirs(g.session_dir))
        return 0;
    if (!g.saves_size)
        return 1;                              /* the host has none: neither do we */
    if (g.saves_size < 8 || memcmp(p, "XLSF", 4) != 0)
        return 0;
    count = get32(p + 4);
    table = p + 8;
    /* The data starts after the whole table. */
    data = table;
    for (i = 0; i < count; i++) {
        if (data + 2 > end)
            return 0;
        data += 2 + get16(data) + 4;
    }
    for (i = 0; i < count; i++) {
        char rel[600], full[1700], dir[1700];
        size_t nl = get16(table);
        uint32_t n;
        char *slash, *c;
        FILE *f;

        if (nl >= sizeof rel || table + 2 + nl + 4 > end)
            return 0;
        memcpy(rel, table + 2, nl);
        rel[nl] = 0;
        n = get32(table + 2 + nl);
        table += 2 + nl + 4;
        if (strstr(rel, "..") || data + n > end)
            return 0;                         /* never outside the session folder */
        for (c = rel; *c; c++)
            if (*c == '/')
                *c = PATH_SEP;
        snprintf(full, sizeof full, "%s%c%s", g.session_dir, PATH_SEP, rel + (rel[0] == PATH_SEP));
        snprintf(dir, sizeof dir, "%s", full);
        slash = strrchr(dir, PATH_SEP);
        if (slash) {
            *slash = 0;
            recomp_make_dirs(dir);
        }
        f = ufopen(full, "wb");
        if (!f)
            return 0;
        fwrite(data, 1, n, f);
        fclose(f);
        data += n;
    }
    return 1;
}

const char *lsnet_session_dir(void)
{
    return g.session_dir[0] ? g.session_dir : NULL;
}

/* ── settings that change the game ────────────────────────────────── */

void recomp_lockstep_add_setting(const char *name, const char *value)
{
    size_t n = strlen(g.settings);
    snprintf(g.settings + n, sizeof g.settings - n, "%s%s=%s", n ? ";" : "",
             name ? name : "", value ? value : "");
}

/* The value of `name` in "a=1;b=2", or NULL; *len gets its length. */
static const char *setting_in(const char *list, const char *name, size_t nl, size_t *len)
{
    const char *p = list;

    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            *len = strcspn(p + nl + 1, ";");
            return p + nl + 1;
        }
        p = strchr(p, ';');
        if (p)
            p++;
    }
    return NULL;
}

/* This machine's settings against the host's: the first that differs, for
 * the refusal. */
static int settings_differ(char *why, size_t n)
{
    const char *p = g.settings;

    if (strcmp(g.settings, g.host_settings) == 0)
        return 0;
    while (p && *p) {
        size_t nl = strcspn(p, "="), ml = 0, tl = 0;
        const char *mine = setting_in(g.settings, p, nl, &ml);
        const char *theirs = setting_in(g.host_settings, p, nl, &tl);

        if (!theirs || tl != ml || strncmp(mine, theirs, ml) != 0) {
            snprintf(why, n, "%.*s is %.*s here but %.*s on the host", (int)nl, p, (int)ml,
                     mine ? mine : "", theirs ? (int)tl : 7, theirs ? theirs : "not set");
            return 1;
        }
        p = strchr(p, ';');
        if (p)
            p++;
    }
    snprintf(why, n, "the host has settings this copy does not (%s against %s)",
             g.host_settings, g.settings);
    return 1;
}

/* ── input ────────────────────────────────────────────────────────── */

/* What the other side has not acknowledged, up to MAX_SEND frames, with our
 * own receipt and newest checkpoint. Called with the lock held. */
static void send_input_locked(void)
{
    uint8_t b[27 + MAX_SEND * 18];
    uint32_t first = g.remote_acked + 1, count, i;
    const Checkpoint *ck = NULL;

    if (!g.have_peer || g.gone || (!g.host && !g.welcomed))
        return;
    if (g.local_upto < first)
        count = 0;
    else
        count = g.local_upto - first + 1;
    if (count > MAX_SEND) {
        first = g.local_upto - MAX_SEND + 1;   /* the oldest are re-sent next time */
        count = MAX_SEND;
    }
    for (i = 0; i < CHECKPOINTS; i++)
        if (g.mine[i].tick && (!ck || g.mine[i].tick > ck->tick))
            ck = &g.mine[i];
    header(b, T_INPUT);
    b[5] = (uint8_t)g.local_port;
    put32(b + 6, g.remote_upto);
    put32(b + 10, first);
    b[14] = (uint8_t)count;
    put32(b + 15, ck ? ck->tick : 0u);
    put64(b + 19, ck ? ck->hash : 0u);
    for (i = 0; i < count; i++)
        pad_put(b + 27 + 18 * i, &g.local[(first + i) % RING]);
    send_raw(b, 27 + 18 * (int)count);
}

/* The other side's checkpoint against ours for the same frame. */
static void check_theirs_locked(uint32_t tick, uint64_t hash)
{
    unsigned i;

    if (!tick || tick <= g.theirs.tick)
        return;
    for (i = 0; i < CHECKPOINTS; i++) {
        if (g.mine[i].tick != tick)
            continue;
        g.theirs.tick = tick;
        g.theirs.hash = hash;
        if (g.mine[i].hash != hash) {
            if (!g.desyncs++)
                g.first_desync = tick;
            if (g.desyncs <= 5)
                fprintf(stderr, "[LOCKSTEP-NET] tick %u: the games DIFFER (here %016llX, "
                        "the other machine %016llX)%s\n", tick,
                        (unsigned long long)g.mine[i].hash, (unsigned long long)hash,
                        g.desyncs == 1 ? " -- the first tick that does" : "");
        } else if (tick % 3600 == 0) {
            fprintf(stderr, "[LOCKSTEP-NET] tick %u: both machines the same (%u checks differed)\n",
                    tick, g.desyncs);
        }
        fflush(stderr);
        return;
    }
}

static void on_input_locked(const uint8_t *b, int n)
{
    uint32_t ack, first, count, i, ck_tick;
    uint64_t ck_hash;

    if (n < 27)
        return;
    ack = get32(b + 6);
    first = get32(b + 10);
    count = b[14];
    ck_tick = get32(b + 15);
    ck_hash = get64(b + 19);
    if (n < 27 + 18 * (int)count)
        return;
    if (ack > g.remote_acked && ack <= g.local_upto)
        g.remote_acked = ack;
    for (i = 0; i < count; i++) {
        uint32_t t = first + i;
        if (t == g.remote_upto + 1) {
            pad_get(&g.remote[t % RING], b + 27 + 18 * i);
            g.remote_upto = t;
        }
    }
    check_theirs_locked(ck_tick, ck_hash);
}

/* ── the host's answers ───────────────────────────────────────────── */

static void on_hello_locked(const uint8_t *b, int n, const struct sockaddr_in *from)
{
    char build[65];
    uint8_t w[29 + SETTINGS_LEN];

    if (n < 5 + 64)
        return;
    memcpy(build, b + 5, 64);
    build[64] = 0;
    if (strncmp(build, g.build, 64) != 0) {
        uint8_t f[5 + 160];
        int len;
        header(f, T_REFUSE);
        len = snprintf((char *)f + 5, 160, "a different build: the host runs %s, you run %s",
                       g.build, build);
        if (len < 0 || len > 159)
            len = 159;
        send_to(from, f, 5 + len + 1);
        say("refused a player with a different build (%s)", build);
        return;
    }
    if (g.have_peer && !g.gone &&
        (from->sin_addr.s_addr != g.peer.sin_addr.s_addr || from->sin_port != g.peer.sin_port)) {
        uint8_t f[5 + 64];
        header(f, T_REFUSE);
        snprintf((char *)f + 5, 64, "the session already has two players");
        send_to(from, f, 5 + 64);
        return;
    }
    if (!g.have_peer) {
        char a[64];
        inet_ntop(AF_INET, &from->sin_addr, a, sizeof a);
        say("player 2 joined from %s:%u", a, (unsigned)ntohs(from->sin_port));
    }
    g.peer = *from;
    g.have_peer = 1;
    g.last_heard_ms = now_ms();
    header(w, T_WELCOME);
    put64(w + 5, (uint64_t)g.epoch);
    w[13] = (uint8_t)g.delay;
    w[14] = (uint8_t)g.remote_port;
    put32(w + 15, g.saves_size);
    put64(w + 19, g.saves_hash);
    put16(w + 27, (uint16_t)strlen(g.settings));
    memcpy(w + 29, g.settings, strlen(g.settings));
    send_to(from, w, 29 + (int)strlen(g.settings));
}

static void on_savereq_locked(const uint8_t *b, int n, const struct sockaddr_in *from)
{
    uint32_t count, i;

    if (n < 6 || !g.saves)
        return;
    count = b[5];
    if (n < 6 + 4 * (int)count)
        return;
    for (i = 0; i < count; i++) {
        uint32_t index = get32(b + 6 + 4 * i), off = index * CHUNK_BYTES, len;
        uint8_t m[11 + CHUNK_BYTES];
        if (off >= g.saves_size)
            continue;
        len = g.saves_size - off;
        if (len > CHUNK_BYTES)
            len = CHUNK_BYTES;
        header(m, T_SAVE);
        put32(m + 5, index);
        put16(m + 9, (uint16_t)len);
        memcpy(m + 11, g.saves + off, len);
        send_to(from, m, 11 + (int)len);
    }
}

/* ── the receive thread ───────────────────────────────────────────── */

#ifdef _WIN32
static DWORD WINAPI receive_thread(LPVOID arg)
#else
static void *receive_thread(void *arg)
#endif
{
    uint8_t b[2048];
    uint64_t last_send = 0;

    (void)arg;
    for (;;) {
        fd_set rd;
        struct timeval tv;
        int r;

        FD_ZERO(&rd);
        FD_SET(g.sock, &rd);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        r = select((int)g.sock + 1, &rd, NULL, NULL, &tv);
        if (r > 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(g.sock, (char *)b, (int)sizeof b, 0,
                                  (struct sockaddr *)&from, &fl);
            if (n >= 5 && b[0] == 'X' && b[1] == 'L' && b[2] == 'S') {
                int type = b[4];
                int from_peer = g.have_peer && from.sin_addr.s_addr == g.peer.sin_addr.s_addr &&
                                from.sin_port == g.peer.sin_port;
                lock();
                if (b[3] != LS_VERSION) {
                    if (g.host && type == T_HELLO) {
                        uint8_t f[5 + 96];
                        header(f, T_REFUSE);
                        snprintf((char *)f + 5, 96, "an older or newer online co-op: update both copies");
                        send_to(&from, f, 5 + 96);
                    }
                } else if (g.host && type == T_HELLO) {
                    on_hello_locked(b, n, &from);
                } else if (g.host && type == T_SAVEREQ && from_peer) {
                    g.last_heard_ms = now_ms();
                    on_savereq_locked(b, n, &from);
                } else if (!g.host && type == T_WELCOME && n >= 29) {
                    if (!g.welcomed) {
                        uint16_t sl = get16(b + 27);
                        g.epoch = (int64_t)get64(b + 5);
                        g.delay = b[13];
                        g.local_port = b[14];
                        g.remote_port = 0;
                        g.saves_size = get32(b + 15);
                        g.saves_hash = get64(b + 19);
                        if (sl >= SETTINGS_LEN)
                            sl = SETTINGS_LEN - 1;
                        if (29 + sl <= n) {
                            memcpy(g.host_settings, b + 29, sl);
                            g.host_settings[sl] = 0;
                        }
                        g.welcomed = 1;
                    }
                    g.last_heard_ms = now_ms();
                    SetEvent(g.arrived);
                } else if (!g.host && type == T_REFUSE) {
                    snprintf(g.refused, sizeof g.refused, "%.*s", n - 5, (const char *)b + 5);
                    SetEvent(g.arrived);
                } else if (!g.host && type == T_SAVE && n >= 11 && g.have) {
                    uint32_t index = get32(b + 5), len = get16(b + 9);
                    uint32_t off = index * CHUNK_BYTES;
                    if (off < g.saves_size && len <= CHUNK_BYTES && 11 + (int)len <= n &&
                        off + len <= g.saves_size && !g.have[index]) {
                        memcpy(g.saves + off, b + 11, len);
                        g.have[index] = 1;
                        g.have_count++;
                    }
                    g.last_heard_ms = now_ms();
                    SetEvent(g.arrived);
                } else if (type == T_INPUT && from_peer) {
                    g.last_heard_ms = now_ms();
                    on_input_locked(b, n);
                    SetEvent(g.arrived);
                } else if (type == T_BYE && from_peer) {
                    if (!g.gone) {
                        g.gone = 1;
                        say("the other player left the session");
                    }
                    SetEvent(g.arrived);
                }
                unlock();
            }
        }
        /* Every 50 ms, whatever the game is doing (a level loading holds its
         * frames): what the other side has not acknowledged, again, which is
         * also how each side knows the other is still there. */
        if (now_ms() - last_send >= 50) {
            last_send = now_ms();
            lock();
            send_input_locked();
            unlock();
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* "host[:port]" to an address. */
static int resolve(const char *spec, int default_port, struct sockaddr_in *out)
{
    char host[256], port[16];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints, *res = NULL;
    size_t hl = colon ? (size_t)(colon - spec) : strlen(spec);

    if (hl == 0 || hl >= sizeof host)
        return 0;
    memcpy(host, spec, hl);
    host[hl] = 0;
    snprintf(port, sizeof port, "%d", colon && colon[1] ? atoi(colon + 1) : default_port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return 0;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 1;
}

/* ── the session ──────────────────────────────────────────────────── */

void recomp_lockstep_set_build_id(const char *id)
{
    snprintf(g.build, sizeof g.build, "%s", id ? id : "");
}

static void leave_now(int code, const char *why)
{
    fprintf(stderr, "[LOCKSTEP-NET] %s\n", why);
    fflush(stderr);
    _Exit(code);
}

/* The joiner, before its game starts: the host's answer, then its saves. */
static void join(const char *peer, int port)
{
    uint64_t start = now_ms(), last = 0;
    char why[400];

    say("joining %s; build %s", peer, g.build);
    for (;;) {
        if (now_ms() - last >= 250) {
            uint8_t h[5 + 64];
            last = now_ms();
            header(h, T_HELLO);
            memset(h + 5, 0, 64);
            memcpy(h + 5, g.build, strlen(g.build) < 64 ? strlen(g.build) : 64);
            send_raw(h, sizeof h);
        }
        WaitForSingleObject(g.arrived, 50);
        lock();
        if (g.refused[0] || g.welcomed) {
            unlock();
            break;
        }
        unlock();
        if (now_ms() - start > 300000) {
            snprintf(why, sizeof why, "no answer from %s in 5 minutes: is it hosting, and is "
                     "its UDP port %d open?", peer, port);
            leave_now(3, why);
        }
    }
    if (g.refused[0]) {
        snprintf(why, sizeof why, "the host refused: %s", g.refused);
        leave_now(3, why);
    }
    if (settings_differ(why + 200, 200)) {
        snprintf(why, 200, "cannot join: ");
        memmove(why + strlen(why), why + 200, strlen(why + 200) + 1);
        leave_now(3, why);
    }

    /* The host's saves, a chunk at a time, up to 64 asked for at once. */
    if (g.saves_size) {
        uint32_t chunks = (g.saves_size + CHUNK_BYTES - 1) / CHUNK_BYTES;
        uint64_t last_progress = now_ms(), last_ask = 0;
        uint32_t seen = 0, at_ask = 0, asked = 0;

        if (g.saves_size > MAX_SAVES)
            leave_now(3, "the host's saves are too big to hand over");
        lock();
        g.saves = (uint8_t *)calloc(1, g.saves_size);
        g.have = (uint8_t *)calloc(1, chunks);
        unlock();
        for (;;) {
            uint32_t done;
            lock();
            done = g.have_count;
            unlock();
            if (done >= chunks)
                break;
            if (done != seen) {
                seen = done;
                last_progress = now_ms();
            }
            if (now_ms() - last_progress > 60000)
                leave_now(3, "the host stopped sending its saves");
            /* Again when the last batch is all in, or after 100 ms for what
             * was lost on the way. */
            if (now_ms() - last_ask >= 100 || done - at_ask >= asked) {
                uint8_t q[6 + 64 * 4];
                uint32_t i, k = 0;
                last_ask = now_ms();
                at_ask = done;
                header(q, T_SAVEREQ);
                lock();
                for (i = 0; i < chunks && k < 64; i++)
                    if (!g.have[i])
                        put32(q + 6 + 4 * k++, i);
                unlock();
                q[5] = (uint8_t)k;
                asked = k;
                send_raw(q, 6 + 4 * (int)k);
            }
            WaitForSingleObject(g.arrived, 30);
        }
        if (fnv64(g.saves, g.saves_size) != g.saves_hash)
            leave_now(3, "the host's saves arrived damaged; try again");
    }
    if (!write_saves())
        leave_now(3, "could not write the host's saves into the session folder");
    say("joined as controller %u; input delay %u frames; playing from the host's saves "
        "(%u bytes) in %s", g.local_port + 1u, g.delay, (unsigned)g.saves_size, g.session_dir);
}

int lsnet_init(int64_t *epoch)
{
    /* The environment, else the title's settings file (the launcher's
     * Online Co-op tab): coop = host|join, coop_host, coop_port, coop_delay. */
    const char *mode = recomp_config_lookup("RECOMP_LOCKSTEP_NET", "coop");
    const char *peer = recomp_config_lookup("RECOMP_LOCKSTEP_PEER", "coop_host");
    /* The launcher asks for one address, the System Link tab's. */
    const char *peer_link = recomp_config_lookup("RECOMP_SYSLINK_HOST", "syslink_host");
    const char *ports = recomp_config_lookup("RECOMP_LOCKSTEP_PORT", "coop_port");
    const char *delay = recomp_config_lookup("RECOMP_LOCKSTEP_DELAY", "coop_delay");
    int port = ports && *ports ? atoi(ports) : DEFAULT_PORT;
    struct sockaddr_in bind_addr;

    if (!mode || (strcmp(mode, "host") != 0 && strcmp(mode, "join") != 0))
        return 0;
    if (!peer || !*peer)
        peer = peer_link;
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            say("no sockets: no online play");
            return 0;
        }
    }
    InitializeCriticalSection(&g.lock);
#else
    pthread_mutex_init(&g.lock, NULL);
#endif
    g.arrived = CreateEventA(NULL, FALSE, FALSE, NULL);
    g.host = strcmp(mode, "host") == 0;
    g.delay = delay && *delay ? (uint32_t)atoi(delay) : DEFAULT_DELAY;
    if (g.delay < 1) g.delay = 1;
    if (g.delay > 60) g.delay = 60;
    g.local_port = g.host ? 0u : 1u;
    g.remote_port = g.host ? 1u : 0u;
    g.epoch = *epoch;
    if (!g.build[0])
        snprintf(g.build, sizeof g.build, "unnamed build");

    /* What changes the game itself, the toolkit's part: the widescreen
     * camera, and the console's disk key, which signs and checks saves (by
     * its hash: a key is the player's own and never crosses the network). */
    {
        char v[32];
        snprintf(v, sizeof v, "%d", recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0) ? 1 : 0);
        recomp_lockstep_add_setting("widescreen", v);
        snprintf(v, sizeof v, "%08x", (unsigned)fnv64(xbox_HDKey, 16));
        recomp_lockstep_add_setting("disk_key", v);
    }

    g.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g.sock == LS_BAD_SOCKET) {
        say("no UDP socket: no online play");
        return 0;
    }
#ifdef _WIN32
    {
        BOOL report = FALSE;
        DWORD got;
        WSAIoctl(g.sock, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0, &got, NULL, NULL);
    }
#endif
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((unsigned short)(g.host ? port : 0));
    if (bind(g.sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        say("cannot receive on UDP port %d (another copy hosting?)", port);
        ls_close(g.sock);
        return 0;
    }
    if (!g.host) {
        if (!peer || !*peer || !resolve(peer, port, &g.peer)) {
            say("RECOMP_LOCKSTEP_PEER \"%s\" is not an address", peer ? peer : "");
            ls_close(g.sock);
            return 0;
        }
        g.have_peer = 1;
    } else {
        snapshot_saves();                    /* before the receive thread can hand it out */
    }
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, receive_thread, NULL, 0, NULL));
#else
    {
        pthread_t t;
        pthread_create(&t, NULL, receive_thread, NULL);
        pthread_detach(t);
    }
#endif
    g.on = 1;
    if (g.host) {
        say("hosting on UDP port %d as controller 1; input delay %u frames; build %s; "
            "settings %s", port, g.delay, g.build, g.settings);
        /* Friends on the internet need the port open on this side's router:
         * asked for as System Link's host asks (net_upnp.c), unless
         * syslink_upnp = 0. A joiner needs nothing opened. */
        if (recomp_config_bool("RECOMP_LOCKSTEP_UPNP", "syslink_upnp", 1))
            net_upnp_open_async(port);
        return 1;
    }
    join(peer, port);
    *epoch = g.epoch;
    return 1;
}

unsigned lsnet_local_port(void)
{
    return g.local_port;
}

uint32_t lsnet_delay(void)
{
    return g.delay;
}

void lsnet_frame(uint32_t tick, const RecompLockstepPad *local_now, int have_checkpoint,
                 uint64_t hash, RecompLockstepPad *pads_out)
{
    uint32_t want = tick + g.delay, t;
    uint64_t wait_start = 0, last_said = 0;

    lock();
    /* This machine's controller now lands DELAY frames ahead; the frames
     * before the first ever sample are neutral on every machine. */
    while (g.local_upto < want) {
        g.local_upto++;
        if (g.local_upto == want)
            g.local[g.local_upto % RING] = *local_now;
        else
            memset(&g.local[g.local_upto % RING], 0, sizeof g.local[0]);
    }
    if (have_checkpoint) {
        Checkpoint *c = &g.mine[(tick / 60u) % CHECKPOINTS];
        c->tick = tick;
        c->hash = hash;
    }
    if (g.remote_upto < g.delay)
        g.remote_upto = g.delay;      /* their first DELAY frames are neutral too */
    send_input_locked();
    /* Wait for the other player's controller for this frame. */
    while (g.remote_upto < tick && !g.gone) {
        uint64_t now = now_ms();
        unlock();
        if (!wait_start)
            wait_start = now;
        if (now - wait_start >= 2000 && now - last_said >= 5000) {
            last_said = now;
            fprintf(stderr, "[LOCKSTEP-NET] tick %u: waiting for the other player (%llu s)\n",
                    tick, (unsigned long long)((now - wait_start) / 1000));
            fflush(stderr);
        }
        WaitForSingleObject(g.arrived, 20);
        lock();
        if (g.have_peer && g.last_heard_ms && now_ms() - g.last_heard_ms > 30000 && !g.gone) {
            g.gone = 1;
            fprintf(stderr, "[LOCKSTEP-NET] tick %u: nothing from the other player for 30 s; "
                            "playing on without them\n", tick);
            fflush(stderr);
        }
        if (!g.have_peer && now_ms() - wait_start > 600000 && !g.gone) {
            g.gone = 1;
            fprintf(stderr, "[LOCKSTEP-NET] nobody joined in 10 minutes; playing alone\n");
            fflush(stderr);
        }
    }
    for (t = 0; t < RECOMP_LOCKSTEP_PORTS; t++)
        memset(&pads_out[t], 0, sizeof pads_out[t]);
    pads_out[g.local_port] = g.local[tick % RING];
    if (!g.gone && g.remote_upto >= tick)
        pads_out[g.remote_port] = g.remote[tick % RING];
    unlock();
}

void lsnet_leave(void)
{
    uint8_t b[5];

    if (!g.on)
        return;
    header(b, T_BYE);
    lock();
    send_raw(b, 5);
    send_raw(b, 5);
    unlock();
}

void lsnet_report(void)
{
    if (!g.on)
        return;
    fprintf(stderr, "[LOCKSTEP-NET] %u checkpoint(s) differed between the machines%s\n",
            g.desyncs, g.desyncs ? "" : ": the same game on both");
    if (g.desyncs)
        fprintf(stderr, "[LOCKSTEP-NET] the first at tick %u\n", g.first_desync);
    fflush(stderr);
}
