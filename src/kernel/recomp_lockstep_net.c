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
 * The two machines are kept level (lsnet_frame; RECOMP_LOCKSTEP_BALANCE=0
 * turns it off): left alone, the one whose frames run ahead waits on the
 * other's controller at every frame and stalls on any late packet, while the
 * other has all the spare delay. The one with clearly less slack holds back
 * a frame until the two have about the same. RECOMP_LOCKSTEP_STATS=1 logs
 * every 600 frames how each side waited, its missed frames and its slack.
 *
 * Both machines start the game from the host's saves. Before its game
 * starts the host takes this title's folders from its hard disk (E:\UDATA\
 * <title id> and E:\TDATA\<title id>); a joiner receives them before its own
 * game starts, writes them into a session folder (<data dir>/coop-session/
 * <title id>) and plays from that as its E: (recomp_lockstep_hdd_dir). The
 * joiner's own saves are not touched, and what its game saves during the
 * session stays in the session folder. Settings that change the game itself
 * (widescreen widens the title's own camera) must be the same on both: the
 * host sends its own, and the joiner takes the host's for the session where
 * a player chooses them (widescreen, field of view, crosshair, mouse look,
 * the co-op view, TimeSplitters 2's menus; adopt_host_settings) and says so,
 * its settings file untouched. What a player cannot choose (the console's
 * disk key) still refuses, and says which.
 *
 * Each copy is one player for one game: a joiner's HELLO carries a number of
 * its own, so a host -- which through an online server hears every joiner
 * from the server's address -- can tell a new copy from the one it took. A
 * joiner that leaves before the game begins (refused, or its saves failed)
 * says BYE with why, and the host waits for another; once the game has
 * begun, a new copy is refused, since a game cannot take a player in midway.
 *
 * Each message is UDP, 'X' 'L' 'S' version type, then:
 *   HELLO    joiner -> host: the build, so two different builds never play,
 *            and the copy's own number (8 bytes)
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
 *
 * Through an online server (online_server = name[:port], RECOMP_ONLINE_SERVER;
 * recompose's tools/online_server): neither side opens a port. Both send to
 * the server, which pairs a host and a joiner by their session code
 * (syslink_code, RECOMP_LOCKSTEP_CODE) and passes these same messages
 * between them. Each registers first and every two seconds after -- which
 * keeps its router's way back open -- and answers the number the server
 * sends it, so a registration in someone else's name does not count:
 *   'X' 'R' 1 REGISTER  role (1 host, 2 join), nonce (8), code length, code
 *   'X' 'R' 1 CHALLENGE nonce (8)     'X' 'R' 1 OK role, the other's there
 *   'X' 'R' 1 ERROR     why, as text: a joiner leaves with it
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
#include <time.h>
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

#define LS_VERSION      5u      /* 4: a pad carries the mouse's look; 5: HELLO the copy's number */
#define DEFAULT_PORT    27620
#define SERVER_PORT     27630         /* tools/online_server's */
#define REGISTER_EVERY  2000u         /* ms between registrations with the server */
#define DEFAULT_DELAY   4u
#define RING            512u          /* frames of input kept per player */
#define MAX_SEND        32u           /* frames of input in one message */
#define CHECKPOINTS     64u           /* recent hashes kept for the other's to meet */
#define CHUNK_BYTES     1024u         /* one SAVE message's share of the saves */
#define MAX_SAVES       (32u << 20)   /* the most a host hands over */
#define MAX_SAVE_FILES  4096
#define SETTINGS_LEN    256

enum { T_HELLO = 1, T_WELCOME, T_REFUSE, T_INPUT, T_BYE, T_SAVEREQ, T_SAVE,
       T_PING, T_PONG };
enum { RELAY_REGISTER = 1, RELAY_CHALLENGE, RELAY_OK, RELAY_ERROR };

typedef struct Checkpoint { uint32_t tick; uint64_t hash; } Checkpoint;

static struct {
    int on, host;
    ls_socket sock;
    struct sockaddr_in peer;
    int have_peer;
    /* Through an online server: where it is, the session code, the number it
     * last sent this copy, whether it has said yes. */
    int relay, relay_ok;
    struct sockaddr_in relay_addr;
    char code[64], server[256];
    uint8_t relay_nonce[8];
    uint64_t relay_last;
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
    uint64_t instance;                /* this copy's number, in its HELLO (joiner) */
    uint64_t peer_instance;           /* host: the joiner it took */
    /* The input delay is agreed before either game takes a frame: a joiner
     * measures the round trip once it is in and every INPUT it sends carries
     * the delay that covers it; the host's first frame waits for that. A
     * delay the host's settings name is used as it is. */
    int started;                      /* the delay is agreed */
    int delay_fixed;                  /* the host's settings named one */
    uint32_t rtt_ms;
    /* Keeping the two machines level (lsnet_frame, RECOMP_LOCKSTEP_BALANCE):
     * each one's lead -- how far ahead of its frame the other's controller
     * was when the frame began -- averaged, this machine's from its own
     * frames and the other's from what its INPUTs say. */
    int balance;
    double lead_self, lead_other;
    uint32_t lead_self_n, lead_other_n;
    uint32_t hold_cooldown, holds;
    uint32_t hold_every;              /* frames between holds: 120, doubled while they do nothing */
    double hold_lead;                 /* lead_self before the last hold; 1e9: none to judge */
    uint64_t frame_us;                /* when the last frame began */
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

/* One frame's pad on the wire: the gamepad's 18 bytes, the mouse's look and
 * a number key. */
#define NET_PAD 23

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
    put16(b + 18, (uint16_t)p->look_x);
    put16(b + 20, (uint16_t)p->look_y);
    b[22] = p->number_key;
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
    p->look_x = (int16_t)get16(b + 18);
    p->look_y = (int16_t)get16(b + 20);
    p->number_key = b[22];
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

/* For trying the internet on one machine, applied to what this copy sends:
 *   RECOMP_LOCKSTEP_SIM_DELAY_MS=50   hold every message back this long
 *   RECOMP_LOCKSTEP_SIM_LOSS=5        drop this percentage of them
 * Held messages wait in a queue the receive thread sends from. */
#define SIM_QUEUE 512
static struct {
    int ready, delay_ms, loss;
    uint32_t rng;
    struct { uint64_t due; struct sockaddr_in to; int n; uint8_t b[1100]; } q[SIM_QUEUE];
    int head, count;
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t cs;
#endif
} s_sim;

static void sim_lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&s_sim.cs);
#else
    pthread_mutex_lock(&s_sim.cs);
#endif
}

static void sim_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_sim.cs);
#else
    pthread_mutex_unlock(&s_sim.cs);
#endif
}

static void sim_init(void)
{
    const char *d = getenv("RECOMP_LOCKSTEP_SIM_DELAY_MS");
    const char *l = getenv("RECOMP_LOCKSTEP_SIM_LOSS");
    s_sim.delay_ms = d && *d ? atoi(d) : 0;
    s_sim.loss = l && *l ? atoi(l) : 0;
    s_sim.rng = 0x9E3779B9u ^ (uint32_t)now_ms();
#ifdef _WIN32
    InitializeCriticalSection(&s_sim.cs);
#else
    pthread_mutex_init(&s_sim.cs, NULL);
#endif
    s_sim.ready = 1;
    if (s_sim.delay_ms || s_sim.loss)
        say("simulating the internet: %d ms more each way, %d%% of messages lost",
            s_sim.delay_ms, s_sim.loss);
}

static void send_sock(const struct sockaddr_in *to, const uint8_t *b, int n)
{
    if (!s_sim.ready)
        sim_init();
    if (s_sim.loss) {
        s_sim.rng = s_sim.rng * 1103515245u + 12345u;
        if ((int)((s_sim.rng >> 16) % 100u) < s_sim.loss)
            return;
    }
    if (s_sim.delay_ms && n <= (int)sizeof s_sim.q[0].b) {
        sim_lock();
        if (s_sim.count < SIM_QUEUE) {
            int k = (s_sim.head + s_sim.count++) % SIM_QUEUE;
            s_sim.q[k].due = now_ms() + (uint64_t)s_sim.delay_ms;
            s_sim.q[k].to = *to;
            s_sim.q[k].n = n;
            memcpy(s_sim.q[k].b, b, (size_t)n);
        }
        sim_unlock();
        return;
    }
    sendto(g.sock, (const char *)b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

/* The receive thread: what is due goes out. */
static void sim_flush(void)
{
    uint64_t now = now_ms();
    if (!s_sim.ready || !s_sim.delay_ms)
        return;
    sim_lock();
    while (s_sim.count && s_sim.q[s_sim.head].due <= now) {
        sendto(g.sock, (const char *)s_sim.q[s_sim.head].b, s_sim.q[s_sim.head].n, 0,
               (const struct sockaddr *)&s_sim.q[s_sim.head].to, sizeof s_sim.q[0].to);
        s_sim.head = (s_sim.head + 1) % SIM_QUEUE;
        s_sim.count--;
    }
    sim_unlock();
}

static void send_raw(const uint8_t *b, int n)
{
    if (g.have_peer)
        send_sock(&g.peer, b, n);
}

static void send_to(const struct sockaddr_in *to, const uint8_t *b, int n)
{
    send_sock(to, b, n);
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

/* ── the player's view of the session: the window's title ──────────── */

static RecompLockstepStatusSink s_sink;
static char s_status[160];
static int s_status_set;

void recomp_lockstep_set_status_sink(RecompLockstepStatusSink fn)
{
    s_sink = fn;
    if (s_sink && s_status_set)
        s_sink(s_status[0] ? s_status : NULL);     /* what it missed */
}

static void status(const char *text)
{
    if (s_status_set && strcmp(s_status, text ? text : "") == 0)
        return;
    snprintf(s_status, sizeof s_status, "%s", text ? text : "");
    s_status_set = 1;
    if (s_sink)
        s_sink(s_status[0] ? s_status : NULL);
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

/* ── how the session runs (RECOMP_LOCKSTEP_STATS=1) ──────────────── */

/* For tuning the delay and finding stalls, nothing changes but the log:
 * every 600 frames, how many frames waited for the other player's
 * controller and for how long (p50, p95, max), how far ahead of the frame
 * the other player's controller was when the frame began (the lead; under
 * 0 the frame waited), and, on the joiner, the round trip during play (a
 * PING every 2 s, which the host answers as at the join). */
#define STATS_SPAN 600u
static struct {
    int on;
    uint32_t frames, waited, first_tick;
    uint32_t wait_us[STATS_SPAN];
    int32_t lead_min, lead_max;
    int64_t lead_sum;
    uint32_t rtt_min, rtt_max, rtt_n;
    uint64_t rtt_sum, last_ping;
    uint32_t long_waits, long_frames, holds_before;
    uint64_t last_frame_us;
} st;

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000u +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void stats_frame_locked(uint32_t tick, int32_t lead, uint64_t waited_us)
{
    if (!st.on)
        return;
    if (!st.frames) {
        st.first_tick = tick;
        st.lead_min = st.lead_max = lead;
    }
    st.frames++;
    st.lead_sum += lead;
    if (lead < st.lead_min) st.lead_min = lead;
    if (lead > st.lead_max) st.lead_max = lead;
    if (waited_us)
        st.wait_us[st.waited++ % STATS_SPAN] = (uint32_t)(waited_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : waited_us);
    /* A wait past 8 ms most likely cost a vblank; frames more than 25 ms
     * apart missed one, whatever held them (a wait, a hold, the game). */
    if (waited_us >= 8000u)
        st.long_waits++;
    {
        uint64_t now = now_us();
        if (st.last_frame_us && now - st.last_frame_us > 25000u)
            st.long_frames++;
        st.last_frame_us = now;
    }
    if (st.frames >= STATS_SPAN) {
        uint32_t n = st.waited < STATS_SPAN ? st.waited : STATS_SPAN;
        double p50 = 0, p95 = 0, mx = 0;
        char rtt[96] = "";

        if (n) {
            qsort(st.wait_us, n, sizeof st.wait_us[0], cmp_u32);
            p50 = st.wait_us[n / 2] / 1000.0;
            p95 = st.wait_us[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] / 1000.0;
            mx = st.wait_us[n - 1] / 1000.0;
        }
        if (st.rtt_n)
            snprintf(rtt, sizeof rtt, "; round trip %u/%llu/%u ms (min/avg/max of %u)", st.rtt_min,
                     (unsigned long long)(st.rtt_sum / st.rtt_n), st.rtt_max, st.rtt_n);
        fprintf(stderr, "[LOCKSTEP-NET] stats ticks %u-%u: %u of %u frames waited, "
                        "%.1f/%.1f/%.1f ms (p50/p95/max), %u over 8 ms; %u frames over 25 ms apart; "
                        "lead %d/%.1f/%d frames (min/avg/max), the other's %.1f; %u held back; "
                        "delay %u%s\n",
                st.first_tick, tick, st.waited, st.frames, p50, p95, mx, st.long_waits,
                st.long_frames, st.lead_min, (double)st.lead_sum / st.frames, st.lead_max,
                g.lead_other, g.holds - st.holds_before, g.delay, rtt);
        fflush(stderr);
        st.long_waits = st.long_frames = 0;
        st.holds_before = g.holds;
        st.frames = st.waited = 0;
        st.lead_sum = 0;
        st.rtt_n = 0;
        st.rtt_sum = 0;
        st.rtt_min = st.rtt_max = 0;
    }
}

/* ── input ────────────────────────────────────────────────────────── */

/* What the other side has not acknowledged, up to MAX_SEND frames, with our
 * own receipt and newest checkpoint. Called with the lock held. */
static void send_input_locked(void)
{
    uint8_t b[30 + MAX_SEND * NET_PAD];
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
    b[27] = (uint8_t)(g.started ? g.delay : 0u);
    put16(b + 28, (uint16_t)(g.rtt_ms > 65535u ? 65535u : g.rtt_ms));
    for (i = 0; i < count; i++)
        pad_put(b + 30 + NET_PAD * i, &g.local[(first + i) % RING]);
    send_raw(b, 30 + NET_PAD * (int)count);
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
            if (!g.desyncs++) {
                g.first_desync = tick;
                status("online co-op: the two games differ (see the log)");
            }
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

    if (n < 30)
        return;
    if (g.host && !g.started && b[27]) {
        /* The joiner's measured delay: the session starts. */
        g.delay = b[27];
        g.rtt_ms = get16(b + 28);
        g.started = 1;
        say("the joiner measured %u ms there and back: input delay %u frames",
            (unsigned)g.rtt_ms, g.delay);
    }
    ack = get32(b + 6);
    first = get32(b + 10);
    count = b[14];
    ck_tick = get32(b + 15);
    ck_hash = get64(b + 19);
    if (n < 30 + NET_PAD * (int)count)
        return;
    if (ack > g.remote_acked && ack <= g.local_upto)
        g.remote_acked = ack;
    /* The sender's lead when it sent this: what it had of ours (ack) against
     * its frame (its newest input, first + count - 1, lands DELAY frames
     * after the frame it was taken in). */
    if (count && g.started && first + count - 1 > 2u * g.delay) {
        int32_t their_tick = (int32_t)(first + count - 1 - g.delay);
        double l = (double)(int32_t)(ack - (uint32_t)their_tick);
        g.lead_other = g.lead_other_n++ ? g.lead_other + (l - g.lead_other) / 32.0 : l;
    }
    for (i = 0; i < count; i++) {
        uint32_t t = first + i;
        if (t == g.remote_upto + 1) {
            pad_get(&g.remote[t % RING], b + 30 + NET_PAD * i);
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
    uint64_t inst;

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
    inst = n >= 5 + 64 + 8 ? get64(b + 5 + 64) : 0;
    if (g.have_peer && inst != g.peer_instance) {
        /* Another copy. Before the joiner it took has sent a frame, that one
         * may have left without a word (quit, or lost): quiet for 5 s, it is
         * let go and this one taken. Once the game has begun, never. */
        int begun = g.remote_upto > 0;

        if (!begun && (g.gone || now_ms() - g.last_heard_ms > 5000)) {
            say("player 2 left before the game began; taking a new copy");
            g.have_peer = 0;
            g.gone = 0;
        } else {
            uint8_t f[5 + 96];
            header(f, T_REFUSE);
            snprintf((char *)f + 5, 96, "%s", begun
                     ? "its game has already begun; join when the host starts a new one"
                     : "the session already has two players");
            send_to(from, f, 5 + 96);
            return;
        }
    }
    if (!g.have_peer) {
        char a[64], st[128];
        inet_ntop(AF_INET, &from->sin_addr, a, sizeof a);
        say("player 2 joined from %s:%u", a, (unsigned)ntohs(from->sin_port));
        snprintf(st, sizeof st, "online co-op, player 1, with %s", a);
        status(st);
    }
    g.peer = *from;
    g.have_peer = 1;
    g.peer_instance = inst;
    g.last_heard_ms = now_ms();
    header(w, T_WELCOME);
    put64(w + 5, (uint64_t)g.epoch);
    w[13] = (uint8_t)(g.delay_fixed ? g.delay : 0u);   /* 0: the joiner measures */
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

/* Under the lock: register with the online server (the header's REGISTER). */
static void relay_register_locked(void)
{
    uint8_t r[4 + 1 + 8 + 1 + 63];
    size_t cl = strlen(g.code);

    if (cl > 63)
        cl = 63;
    r[0] = 'X'; r[1] = 'R'; r[2] = 1; r[3] = RELAY_REGISTER;
    r[4] = g.host ? 1 : 2;
    memcpy(r + 5, g.relay_nonce, 8);
    r[13] = (uint8_t)cl;
    memcpy(r + 14, g.code, cl);
    send_sock(&g.relay_addr, r, 14 + (int)cl);
    g.relay_last = now_ms();
}

/* Under the lock: what the online server says. */
static void relay_receive_locked(const uint8_t *b, int n)
{
    if (n < 4 || b[2] != 1)
        return;
    if (b[3] == RELAY_CHALLENGE && n >= 12) {
        memcpy(g.relay_nonce, b + 4, 8);
        relay_register_locked();
    } else if (b[3] == RELAY_OK && n >= 6) {
        if (!g.relay_ok) {
            g.relay_ok = 1;
            say("the online server has this copy in the session%s",
                b[5] ? ", with the other player" : "");
            if (g.host)
                status("online co-op: waiting for player 2 to join through the online server");
        }
    } else if (b[3] == RELAY_ERROR) {
        char why[160];

        snprintf(why, sizeof why, "%.*s", n - 4 < 150 ? n - 4 : 150, (const char *)b + 4);
        g.relay_ok = 0;
        if (!g.host && strncmp(why, "no host", 7) == 0) {
            /* Not yet: the host may start after this copy. It keeps asking,
             * every two seconds, for the five minutes a joiner waits. */
            static int told;
            if (!told) {
                told = 1;
                say("the online server: %s; waiting for the host", why);
                status("online co-op: waiting for the host to start, through the online server");
            }
        } else if (!g.host) {
            snprintf(g.refused, sizeof g.refused, "the online server: %s", why);
            SetEvent(g.arrived);
        } else {
            char st[200];
            snprintf(st, sizeof st, "online co-op: the online server says %s", why);
            say("the online server: %s", why);
            status(st);
        }
    }
}

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

        sim_flush();
        FD_ZERO(&rd);
        FD_SET(g.sock, &rd);
        tv.tv_sec = 0;
        tv.tv_usec = s_sim.delay_ms ? 2000 : 50000;
        r = select((int)g.sock + 1, &rd, NULL, NULL, &tv);
        if (r > 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(g.sock, (char *)b, (int)sizeof b, 0,
                                  (struct sockaddr *)&from, &fl);
            if (g.relay && n >= 4 && b[0] == 'X' && b[1] == 'R' &&
                from.sin_addr.s_addr == g.relay_addr.sin_addr.s_addr &&
                from.sin_port == g.relay_addr.sin_port) {
                lock();
                relay_receive_locked(b, n);
                unlock();
            } else if (n >= 5 && b[0] == 'X' && b[1] == 'L' && b[2] == 'S') {
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
                        if (b[13]) {
                            g.delay = b[13];
                            g.delay_fixed = 1;
                        }
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
                } else if (g.host && type == T_PING && from_peer && n >= 13) {
                    uint8_t pong[13];
                    memcpy(pong, b, 13);
                    header(pong, T_PONG);
                    send_to(&from, pong, 13);
                } else if (!g.host && type == T_PONG && n >= 13) {
                    /* Timed in microseconds (the host sends the 8 bytes back
                     * as they came): the millisecond tick is 15.6 ms coarse on
                     * Windows, enough to put the delay a frame out. */
                    uint32_t rtt = (uint32_t)((now_us() - get64(b + 5) + 500u) / 1000u);
                    if (!g.rtt_ms || rtt < g.rtt_ms)
                        g.rtt_ms = rtt ? rtt : 1;   /* the best: the path, not a hiccup */
                    if (st.on && g.started) {
                        if (!st.rtt_n || rtt < st.rtt_min) st.rtt_min = rtt;
                        if (rtt > st.rtt_max) st.rtt_max = rtt;
                        st.rtt_sum += rtt;
                        st.rtt_n++;
                    }
                    SetEvent(g.arrived);
                } else if (type == T_INPUT && from_peer) {
                    g.last_heard_ms = now_ms();
                    on_input_locked(b, n);
                    SetEvent(g.arrived);
                } else if (type == T_BYE && from_peer) {
                    char why[128];

                    snprintf(why, sizeof why, "%.*s", n > 5 ? (n - 5 < 127 ? n - 5 : 127) : 0,
                             (const char *)b + 5);
                    if (g.host && g.have_peer && g.remote_upto == 0) {
                        /* Not yet a game: the joiner went before its first
                         * frame (refused itself, its saves failed). Wait
                         * for another. */
                        g.have_peer = 0;
                        g.gone = 0;
                        say("player 2 left before the game began%s%s; waiting for another",
                            why[0] ? ": " : "", why);
                        status("online co-op: waiting for player 2 to join");
                    } else if (!g.gone) {
                        g.gone = 1;
                        say("the other player left the session%s%s", why[0] ? ": " : "", why);
                        status("online co-op: the other player left");
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
        /* Through an online server: register again every two seconds. */
        if (g.relay && now_ms() - g.relay_last >= REGISTER_EVERY) {
            lock();
            relay_register_locked();
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
    /* The host has counted this copy in (and an online server holds its
     * place): tell both it is going, and why. */
    if (g.on && !g.host && g.have_peer && g.welcomed) {
        uint8_t b[5 + 120];
        int len = (int)strlen(why) < 119 ? (int)strlen(why) : 119;

        header(b, T_BYE);
        memcpy(b + 5, why, (size_t)len);
        lock();
        send_raw(b, 5 + len);
        send_raw(b, 5 + len);
        unlock();
    }
    _Exit(code);
}

/* The process's environment, which every settings lookup reads before the
 * settings file (recomp_config.c). */
static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* A joiner whose settings differ from the host's takes the host's, for the
 * session only (the environment, not the settings file), where a player
 * chooses them; everything that reads them does so after this, or reads
 * again (the titles' main.c: hle_session_settings_reread). 1 if what is
 * left differs. */
static int adopt_host_settings(char *why, size_t n)
{
    static const struct { const char *name, *env; } k_adopt[] = {
        { "widescreen", "RECOMP_WIDESCREEN" }, { "fov", "RECOMP_FOV" },
        { "crosshair", "RECOMP_CROSSHAIR" }, { "mouse_look", "RECOMP_MOUSE_LOOK" },
        { "coop_view", "RECOMP_COOP_VIEW" }, { "ts2_menus", "RECOMP_TS2_MENUS" },
    };
    char took[200] = "";
    const char *p = g.host_settings;
    size_t i;

    /* What a player cannot choose has to be the same already. */
    while (p && *p) {
        size_t nl = strcspn(p, "="), ml = 0, tl = 0;
        const char *mine = setting_in(g.settings, p, nl, &ml);
        const char *theirs = setting_in(g.host_settings, p, nl, &tl);
        int adoptable = 0;

        for (i = 0; i < sizeof k_adopt / sizeof k_adopt[0]; i++)
            if (strlen(k_adopt[i].name) == nl && strncmp(p, k_adopt[i].name, nl) == 0)
                adoptable = 1;
        if (!adoptable && (!mine || ml != tl || strncmp(mine, theirs, ml) != 0)) {
            snprintf(why, n, "%.*s is %.*s here but %.*s on the host", (int)nl, p,
                     mine ? (int)ml : 7, mine ? mine : "not set", (int)tl, theirs);
            return 1;
        }
        p = strchr(p, ';');
        if (p)
            p++;
    }
    for (i = 0; i < sizeof k_adopt / sizeof k_adopt[0]; i++) {
        size_t nl = strlen(k_adopt[i].name), ml = 0, tl = 0;
        const char *mine = setting_in(g.settings, k_adopt[i].name, nl, &ml);
        const char *theirs = setting_in(g.host_settings, k_adopt[i].name, nl, &tl);
        char v[64];

        if (!theirs || (mine && ml == tl && strncmp(mine, theirs, tl) == 0))
            continue;
        snprintf(v, sizeof v, "%.*s", (int)tl, theirs);
        set_env(k_adopt[i].env, v);
        snprintf(took + strlen(took), sizeof took - strlen(took), "%s%s %s (here %.*s)",
                 took[0] ? ", " : "", k_adopt[i].name, v, mine ? (int)ml : 7,
                 mine ? mine : "not set");
    }
    if (took[0]) {
        char st[160];

        say("taking the host's settings for this session: %s", took);
        snprintf(st, sizeof st, "online co-op: playing with the host's settings (%s)", took);
        status(st);
    }
    /* The host's, then, in what this copy reports from here on. */
    snprintf(g.settings, sizeof g.settings, "%s", g.host_settings);
    return 0;
}

/* The joiner, before its game starts: the host's answer, then its saves. */
static void join(const char *peer, int port)
{
    uint64_t start = now_ms(), last = 0;
    char why[400];

    say("joining %s; build %s", peer, g.build);
    for (;;) {
        if (now_ms() - last >= 250) {
            uint8_t h[5 + 64 + 8];
            last = now_ms();
            header(h, T_HELLO);
            memset(h + 5, 0, 64);
            memcpy(h + 5, g.build, strlen(g.build) < 64 ? strlen(g.build) : 64);
            put64(h + 5 + 64, g.instance);
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
            if (g.relay)
                snprintf(why, sizeof why, "no answer through the online server %s in 5 minutes: "
                         "is the host's copy started, with the same session code?", peer);
            else
                snprintf(why, sizeof why, "no answer from %s in 5 minutes: is it hosting, and is "
                         "its UDP port %d open?", peer, port);
            leave_now(3, why);
        }
    }
    if (g.refused[0]) {
        if (strncmp(g.refused, "the online server", 17) == 0)
            snprintf(why, sizeof why, "%s", g.refused);
        else
            snprintf(why, sizeof why, "the host refused: %s", g.refused);
        leave_now(3, why);
    }
    if (settings_differ(why + 200, 200) && adopt_host_settings(why + 200, 200)) {
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

    /* The round trip, by ten pings 30 ms apart, and the delay that covers
     * half of it with two frames to spare, so a press reaches the other
     * machine before the frame it belongs to: 3 frames on a LAN, 5 at 50 ms
     * each way, 8 at 100. */
    {
        int i;
        for (i = 0; i < 10; i++) {
            uint8_t ping[13];
            header(ping, T_PING);
            put64(ping + 5, now_us());
            send_raw(ping, 13);
            WaitForSingleObject(g.arrived, 30);
        }
        WaitForSingleObject(g.arrived, 300);
        lock();
        if (!g.delay_fixed) {
            uint32_t one_way = g.rtt_ms ? (g.rtt_ms + 1) / 2 : 100;
            g.delay = (one_way * 60u + 999u) / 1000u + 2u;      /* frames, rounded up, +2 */
            if (g.delay < 3) g.delay = 3;
            if (g.delay > 30) g.delay = 30;
        }
        g.started = 1;
        unlock();
        say("measured %u ms there and back; input delay %u frames%s", (unsigned)g.rtt_ms,
            g.delay, g.delay_fixed ? " (the host's setting)" : "");
    }
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
    /* Through an online server, with the session code (the launcher's one code). */
    const char *server = recomp_config_lookup("RECOMP_ONLINE_SERVER", "online_server");
    const char *via = recomp_config_lookup("RECOMP_ONLINE_VIA", "online_via");
    const char *code = recomp_config_lookup("RECOMP_LOCKSTEP_CODE", "syslink_code");
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
    g.delay_fixed = delay && *delay;
    if (g.delay < 1) g.delay = 1;
    if (g.delay > 60) g.delay = 60;
    g.local_port = g.host ? 0u : 1u;
    g.remote_port = g.host ? 1u : 0u;
    g.epoch = *epoch;
    if (!g.build[0])
        snprintf(g.build, sizeof g.build, "unnamed build");
    st.on = recomp_config_bool("RECOMP_LOCKSTEP_STATS", "lockstep_stats", 0);
    g.balance = recomp_config_bool("RECOMP_LOCKSTEP_BALANCE", "lockstep_balance", 1);
    {
        /* A number of this copy's own: the clock, the process, an address
         * of the stack, mixed. Not secret; only different. */
        uint64_t seed[3];
        int here;

        seed[0] = now_ms();
#ifdef _WIN32
        seed[1] = (uint64_t)GetCurrentProcessId();
#else
        seed[1] = (uint64_t)getpid();
#endif
        seed[2] = (uint64_t)(uintptr_t)&here;
        g.instance = fnv64((const uint8_t *)seed, sizeof seed) | 1u;
    }

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
    if (via && strcmp(via, "direct") == 0 && !getenv("RECOMP_ONLINE_SERVER"))
        server = NULL;                       /* an address kept, but not chosen */
    if (via && strcmp(via, "server") == 0 && (!server || !*server)) {
        /* Chosen, but not usable: not direct instead, which would open the
         * router's port for a player who chose not to. */
        say("online co-op through a server needs the server's address (online_server)");
        status("online co-op: no online server address; playing alone");
        ls_close(g.sock);
        return 0;
    }
    if (server && *server) {
        if (!code || strlen(code) < 4) {
            say("online co-op through a server needs a session code of at least four "
                "characters (syslink_code)");
            status("online co-op: no session code (four or more characters) for the online"
                   " server; playing alone");
            ls_close(g.sock);
            return 0;
        }
        if (!resolve(server, SERVER_PORT, &g.relay_addr)) {
            say("the online server \"%s\" is not an address", server);
            status("online co-op: the online server's address was not found; playing alone");
            ls_close(g.sock);
            return 0;
        }
        g.relay = 1;
        snprintf(g.code, sizeof g.code, "%s", code);
        snprintf(g.server, sizeof g.server, "%s", server);
    }
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((unsigned short)(g.host && !g.relay ? port : 0));
    if (bind(g.sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        say("cannot receive on UDP port %d (another copy hosting?)", port);
        ls_close(g.sock);
        return 0;
    }
    if (!g.host && g.relay) {
        g.peer = g.relay_addr;               /* the host is reached through the server */
        g.have_peer = 1;
    } else if (!g.host) {
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
    if (g.relay) {
        lock();
        relay_register_locked();
        unlock();
    }
    if (g.host && g.relay) {
        say("hosting through the online server %s as controller 1; input delay %u frames; "
            "build %s; settings %s", g.server, g.delay, g.build, g.settings);
        status("online co-op: reaching the online server");
        return 1;
    }
    if (g.host) {
        say("hosting on UDP port %d as controller 1; input delay %u frames; build %s; "
            "settings %s", port, g.delay, g.build, g.settings);
        /* Friends on the internet need the port open on this side's router:
         * asked for as System Link's host asks (net_upnp.c), unless
         * syslink_upnp = 0. A joiner needs nothing opened. */
        if (recomp_config_bool("RECOMP_LOCKSTEP_UPNP", "syslink_upnp", 1))
            net_upnp_open_async(port);
        {
            char st[96];
            snprintf(st, sizeof st, "online co-op: waiting for player 2 to join (UDP port %d)", port);
            status(st);
        }
        return 1;
    }
    join(g.relay ? g.server : peer, port);
    *epoch = g.epoch;
    {
        char st[128];
        snprintf(st, sizeof st, "online co-op, player 2, with %s", peer);
        status(st);
    }
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

/* The host, before its first frame: until the joiner has measured the
 * round trip, the delay (and so where this frame's sample lands) is not
 * known. lsnet_frame waits for it too; this lets the caller ask first. */
void lsnet_wait_start(void)
{
    lock();
    while (g.host && !g.started && !g.gone) {
        unlock();
        WaitForSingleObject(g.arrived, 50);
        lock();
    }
    unlock();
}

void lsnet_frame(uint32_t tick, const RecompLockstepPad *local_now, int have_checkpoint,
                 uint64_t hash, RecompLockstepPad *pads_out)
{
    uint32_t want, t;
    uint64_t wait_start = 0, last_said = 0, wait_t0 = 0;
    int32_t lead;

    lock();
    /* The host's first frame waits for the session's start (the joiner's
     * measured delay): nothing of this machine's may land before it. */
    while (g.host && !g.started && !g.gone) {
        unlock();
        WaitForSingleObject(g.arrived, 50);
        lock();
        if (!wait_start)
            wait_start = now_ms();
        if (!g.have_peer && now_ms() - wait_start > 600000) {
            g.gone = 1;
            g.started = 1;
            fprintf(stderr, "[LOCKSTEP-NET] nobody joined in 10 minutes; playing alone\n");
            fflush(stderr);
        }
    }
    want = tick + g.delay;
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
    lead = (int32_t)(g.remote_upto - tick);
    if (st.on && !g.host && g.started && now_ms() - st.last_ping >= 2000) {
        uint8_t ping[13];
        st.last_ping = now_ms();
        header(ping, T_PING);
        put64(ping + 5, now_us());
        send_raw(ping, 13);
    }
    /* Keep the two machines level. The one that runs ahead (its frames start
     * earlier than the other's) reaches each frame just as the other's
     * controller for it arrives, and waits on that every frame: any late
     * packet stalls it, while the other machine has the whole spare delay to
     * itself (docs/technical/lockstep.md has the measurements). The machine
     * whose lead is two frames or more below the other's holds back one
     * frame -- one missed vblank, as a stall costs -- so each ends up with
     * about the same slack; two seconds apart, as both averages settle again.
     * Two, not less: a hold moves a frame of slack from one to the other, so
     * a gap under two would only change sides, and at 1.25 two copies took
     * turns holding every few seconds for a whole game. The hold is two
     * frame periods: the flip gate (adaptive) presents a late frame at once
     * and the next one still makes its vblank, so a hold of one period is
     * made up straight away and changes nothing; two lose exactly one frame.
     * Should holds still not raise the lead by half a frame by the next
     * chance, the wait between them doubles, up to ten seconds, so a game
     * that makes up frames some other way pays little for them. A pause of a
     * fifth of a second or more (a level loading) starts it all afresh: what
     * was true before it need not be after. */
    if (g.started && g.have_peer && !g.gone) {
        uint64_t now = now_us();
        if (!g.hold_every || (g.frame_us && now - g.frame_us > 200000u)) {
            g.hold_every = 120;
            g.hold_lead = 1e9;
            g.lead_self_n = g.lead_other_n = 0;
            g.hold_cooldown = 120;
        }
        g.frame_us = now;
        g.lead_self = g.lead_self_n++ ? g.lead_self + ((double)lead - g.lead_self) / 32.0 : (double)lead;
        if (g.hold_cooldown)
            g.hold_cooldown--;
        if (g.balance && !g.hold_cooldown && tick > 4u * g.delay + 120u && g.lead_self_n >= 120 &&
            g.lead_other_n >= 60 && g.lead_other - g.lead_self >= 2.0 && g.lead_other >= 1.0) {
            uint64_t t0 = now_us();
            if (g.hold_lead < 1e8)
                g.hold_every = g.lead_self - g.hold_lead >= 0.5 ? 120u
                             : g.hold_every * 2u > 600u ? 600u : g.hold_every * 2u;
            g.hold_lead = g.lead_self;
            g.holds++;
            if (g.holds <= 5 || g.holds % 30 == 0)
                say("tick %u: holding back one frame to even the slack (lead here %.1f, there %.1f; "
                    "%u so far, the next in %u frames at the soonest)", tick, g.lead_self,
                    g.lead_other, g.holds, g.hold_every);
            unlock();
            while (now_us() - t0 < 33333u)
                Sleep(1);
            lock();
            g.lead_self += 1.0;
            g.lead_other -= 1.0;
            g.hold_cooldown = g.hold_every;
            lead = (int32_t)(g.remote_upto - tick);
        } else if (!g.hold_cooldown && g.hold_lead < 1e8 && g.lead_other - g.lead_self < 2.0) {
            g.hold_every = 120;               /* level now: the next imbalance starts afresh */
            g.hold_lead = 1e9;
        }
    }
    if (st.on && g.remote_upto < tick && !g.gone)
        wait_t0 = now_us();
    /* Wait for the other player's controller for this frame. */
    while (g.remote_upto < tick && !g.gone) {
        uint64_t now = now_ms();
        unlock();
        if (!wait_start)
            wait_start = now;
        if (now - wait_start >= 1000 && g.have_peer && !last_said)
            status("online co-op: waiting for the other player");
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
            status("online co-op: lost the other player");
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
    if (wait_start && now_ms() - wait_start >= 1000 && !g.gone && g.have_peer) {
        /* Back from a wait the title said: say who it is with again. */
        char st[128], a[64];
        inet_ntop(AF_INET, &g.peer.sin_addr, a, sizeof a);
        snprintf(st, sizeof st, "online co-op, player %u, with %s", g.local_port + 1u, a);
        status(st);
    }
    if (g.have_peer && !g.gone && g.started)
        stats_frame_locked(tick, lead, wait_t0 ? now_us() - wait_t0 : 0);
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
