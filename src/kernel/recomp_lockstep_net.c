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
 * Each message is UDP, 'X' 'L' 'S' version type, then:
 *   HELLO    joiner -> host: the build, so two different builds never play
 *   WELCOME  host -> joiner: the session's epoch (its clock), the delay, the
 *            joiner's controller
 *   REFUSE   host -> joiner: why not (a different build, a full session)
 *   INPUT    both ways, every frame and every 50 ms while waiting: the
 *            sender's controller for the frames the other side has not
 *            acknowledged (up to 32), what it has received of the other's,
 *            and its latest checkpoint hash
 *   BYE      leaving
 * Loss costs nothing but a resend: every INPUT carries everything not yet
 * acknowledged.
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
typedef int ls_socket;
#define LS_BAD_SOCKET (-1)
#define ls_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "recomp_lockstep.h"
#include "recomp_lockstep_net.h"

#define LS_VERSION      1u
#define DEFAULT_PORT    27620
#define DEFAULT_DELAY   4u
#define RING            512u          /* frames of input kept per player */
#define MAX_SEND        32u           /* frames of input in one message */
#define CHECKPOINTS     64u           /* recent hashes kept for the other's to meet */

enum { T_HELLO = 1, T_WELCOME, T_REFUSE, T_INPUT, T_BYE };

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
    char refused[128];
#ifdef _WIN32
    CRITICAL_SECTION lock;
    HANDLE arrived;                   /* auto-reset: input came in */
#else
    pthread_mutex_t lock;
    HANDLE arrived;
#endif
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

static void header(uint8_t *b, int type)
{
    b[0] = 'X'; b[1] = 'L'; b[2] = 'S'; b[3] = (uint8_t)LS_VERSION; b[4] = (uint8_t)type;
}

static void send_raw(const uint8_t *b, int n)
{
    if (g.have_peer)
        sendto(g.sock, (const char *)b, n, 0, (const struct sockaddr *)&g.peer, sizeof g.peer);
}

/* What the other side has not acknowledged, up to MAX_SEND frames, with our
 * own receipt and newest checkpoint. Called with the lock held. */
static void send_input_locked(void)
{
    uint8_t b[16 + 13 + MAX_SEND * 18];
    uint32_t first = g.remote_acked + 1, count, i;
    const Checkpoint *ck = NULL;

    if (!g.have_peer || g.gone)
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
        if (t == g.remote_upto + 1 && t + 0u <= g.remote_upto + RING) {
            pad_get(&g.remote[t % RING], b + 27 + 18 * i);
            g.remote_upto = t;
        }
    }
    check_theirs_locked(ck_tick, ck_hash);
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
            if (n >= 5 && b[0] == 'X' && b[1] == 'L' && b[2] == 'S' && b[3] == LS_VERSION) {
                int type = b[4];
                lock();
                if (g.host && type == T_HELLO) {
                    char build[65];
                    uint8_t w[64];
                    memcpy(build, b + 5, 64);
                    build[64] = 0;
                    if (strncmp(build, g.build, 64) != 0) {
                        uint8_t f[5 + 128];
                        int len;
                        header(f, T_REFUSE);
                        len = snprintf((char *)f + 5, 128, "a different build: the host runs %s, "
                                       "you run %s", g.build, build);
                        if (len < 0 || len > 127)
                            len = 127;
                        sendto(g.sock, (const char *)f, 5 + len + 1, 0,
                               (struct sockaddr *)&from, sizeof from);
                        say("refused a player with a different build (%s)", build);
                    } else if (g.have_peer && memcmp(&from, &g.peer, sizeof from) != 0 && !g.gone) {
                        uint8_t f[5 + 64];
                        header(f, T_REFUSE);
                        snprintf((char *)f + 5, 64, "the session already has two players");
                        sendto(g.sock, (const char *)f, 5 + 64, 0,
                               (struct sockaddr *)&from, sizeof from);
                    } else {
                        if (!g.have_peer) {
                            char a[64];
                            inet_ntop(AF_INET, &from.sin_addr, a, sizeof a);
                            say("player 2 joined from %s:%u", a, (unsigned)ntohs(from.sin_port));
                        }
                        g.peer = from;
                        g.have_peer = 1;
                        g.last_heard_ms = now_ms();
                        header(w, T_WELCOME);
                        put64(w + 5, (uint64_t)g.epoch);
                        w[13] = (uint8_t)g.delay;
                        w[14] = (uint8_t)g.remote_port;
                        sendto(g.sock, (const char *)w, 15, 0, (struct sockaddr *)&from, sizeof from);
                    }
                } else if (!g.host && type == T_WELCOME && n >= 15) {
                    if (!g.welcomed) {
                        g.epoch = (int64_t)get64(b + 5);
                        g.delay = b[13];
                        g.local_port = b[14];
                        g.remote_port = 0;
                        g.welcomed = 1;
                    }
                    g.last_heard_ms = now_ms();
                    SetEvent(g.arrived);
                } else if (!g.host && type == T_REFUSE) {
                    snprintf(g.refused, sizeof g.refused, "%.*s", n - 5, (const char *)b + 5);
                    SetEvent(g.arrived);
                } else if (type == T_INPUT && g.have_peer &&
                           from.sin_addr.s_addr == g.peer.sin_addr.s_addr &&
                           from.sin_port == g.peer.sin_port) {
                    g.last_heard_ms = now_ms();
                    on_input_locked(b, n);
                    SetEvent(g.arrived);
                } else if (type == T_BYE && g.have_peer &&
                           from.sin_addr.s_addr == g.peer.sin_addr.s_addr) {
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

int lsnet_init(int64_t *epoch)
{
    const char *mode = getenv("RECOMP_LOCKSTEP_NET");
    const char *peer = getenv("RECOMP_LOCKSTEP_PEER");
    const char *ports = getenv("RECOMP_LOCKSTEP_PORT");
    const char *delay = getenv("RECOMP_LOCKSTEP_DELAY");
    int port = ports && *ports ? atoi(ports) : DEFAULT_PORT;
    struct sockaddr_in bind_addr;

    if (!mode || (strcmp(mode, "host") != 0 && strcmp(mode, "join") != 0))
        return 0;
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
        fprintf(stderr, "[LOCKSTEP-NET] hosting on UDP port %d as controller 1; input delay %u "
                        "frames; build %s\n", port, g.delay, g.build);
        fflush(stderr);
        return 1;
    }

    /* The joiner waits here, before the game has started, for the host's
     * clock and its controller number: the game cannot begin without them. */
    {
        uint64_t start = now_ms(), last = 0;
        fprintf(stderr, "[LOCKSTEP-NET] joining %s; build %s\n", peer, g.build);
        fflush(stderr);
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
            if (now_ms() - start > 60000) {
                fprintf(stderr, "[LOCKSTEP-NET] no answer from %s in 60 s: is it hosting, and is "
                                "its UDP port %d open?\n", peer, port);
                fflush(stderr);
                _Exit(3);
            }
        }
        if (g.refused[0]) {
            fprintf(stderr, "[LOCKSTEP-NET] the host refused: %s\n", g.refused);
            fflush(stderr);
            _Exit(3);
        }
        *epoch = g.epoch;
        fprintf(stderr, "[LOCKSTEP-NET] joined as controller %u; input delay %u frames\n",
                g.local_port + 1u, g.delay);
        fflush(stderr);
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
