/*
 * recomp_lockstep.c -- the frame counter, the latched controllers and the
 * per-tick trace that online co-op rests on. What it is for and the switches
 * are in recomp_lockstep.h.
 *
 * The trace is text, one line per tick:
 *
 *   t=<tick> m=<connected mask> p=<pad>,<pad>... h=<hash> c=<chunk hashes>
 *
 * Each pad is the 18 bytes of XINPUT_GAMEPAD in hex (buttons, the 8 analog
 * bytes, the four stick axes, little-endian), one per connected port in port
 * order. h hashes every named region at the start of the tick, i.e. the state
 * the previous frames left; c is the same per 4 KB chunk (8 hex digits each,
 * regions in the order they were named), which is what says *where* two runs
 * first differ. A replay reads p= back and checks h= as it goes.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "recomp_lockstep.h"
#include "xbox_memory_layout.h"   /* the guest lock, dropped while waiting on a turn */

extern ptrdiff_t g_xbox_mem_offset;

static uint32_t s_chunk = 4096u;   /* RECOMP_LOCKSTEP_CHUNK: a page, so a trace says where to the page */
#define CHUNK s_chunk
#define MAX_REGIONS 16
#define MAX_DUMPS 16
#define ENUM_TICKS 30u   /* half a second: when the console's USB has found the pads */

typedef struct Region { uint32_t va, size; char name[32]; } Region;

typedef struct ReplayTick {
    unsigned mask;
    RecompLockstepPad pads[RECOMP_LOCKSTEP_PORTS];
    uint64_t hash;
    int has_hash;
    int present;                 /* a line for this tick; a compact recording
                                  * leaves out ticks whose input did not change */
} ReplayTick;

static volatile int s_init;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int s_active;
static unsigned s_ports_mask = 3u;
static volatile uint32_t s_tick;
static RecompLockstepPad s_pads[RECOMP_LOCKSTEP_PORTS];
static unsigned s_mask;
static RecompLockstepSampler s_sampler;

static Region s_regions[MAX_REGIONS];
static int s_region_count;
typedef struct Ignore { uint32_t lo, hi; } Ignore;
#define MAX_IGNORES 64
static Ignore s_ignores[MAX_IGNORES];      /* sorted by lo, not overlapping */
static int s_ignore_count;
static uint32_t *s_chunk_hash;
static unsigned s_chunk_count;

static FILE *s_trace;
static char s_trace_path[1024];
static uint32_t s_dumps[MAX_DUMPS];
static int s_dump_count;
static uint32_t s_end_tick;

static ReplayTick *s_replay;
static uint32_t s_replay_count;          /* ticks 1..count are in s_replay[tick - 1] */
static uint32_t *s_replay_chunks;        /* count * replay_chunk_count */
static unsigned s_replay_chunk_count;
static uint32_t s_mismatches, s_first_mismatch;
static int s_said_replay_end;
static int64_t s_epoch;
static FILE *s_calls;
static uint32_t s_calls_from, s_calls_to;
static volatile DWORD s_frame_thread;

#if defined(_MSC_VER)
#  define LOCKSTEP_TLS __declspec(thread)
#else
#  define LOCKSTEP_TLS __thread
#endif
static LOCKSTEP_TLS uint32_t t_time_tick;
static LOCKSTEP_TLS uint64_t t_time_reads, t_time_last;

/* ── helpers ─────────────────────────────────────────────────────── */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void pad_to_bytes(const RecompLockstepPad *p, uint8_t b[18])
{
    int i;
    b[0] = (uint8_t)p->buttons; b[1] = (uint8_t)(p->buttons >> 8);
    for (i = 0; i < 8; i++)
        b[2 + i] = p->analog[i];
    b[10] = (uint8_t)p->thumb_lx; b[11] = (uint8_t)((uint16_t)p->thumb_lx >> 8);
    b[12] = (uint8_t)p->thumb_ly; b[13] = (uint8_t)((uint16_t)p->thumb_ly >> 8);
    b[14] = (uint8_t)p->thumb_rx; b[15] = (uint8_t)((uint16_t)p->thumb_rx >> 8);
    b[16] = (uint8_t)p->thumb_ry; b[17] = (uint8_t)((uint16_t)p->thumb_ry >> 8);
}

static void pad_from_bytes(RecompLockstepPad *p, const uint8_t b[18])
{
    int i;
    p->buttons = (uint16_t)(b[0] | (b[1] << 8));
    for (i = 0; i < 8; i++)
        p->analog[i] = b[2 + i];
    p->thumb_lx = (int16_t)(uint16_t)(b[10] | (b[11] << 8));
    p->thumb_ly = (int16_t)(uint16_t)(b[12] | (b[13] << 8));
    p->thumb_rx = (int16_t)(uint16_t)(b[14] | (b[15] << 8));
    p->thumb_ry = (int16_t)(uint16_t)(b[16] | (b[17] << 8));
}

/* FNV-1a over 8-byte words, then a final mix: fast enough to run over two
 * megabytes a frame, and only ever compared against itself. */
static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x100000001B3ull;
        p += 8;
        n -= 8;
    }
    while (n--) {
        h = (h ^ *p++) * 0x100000001B3ull;
    }
    return h;
}

static uint64_t mix64(uint64_t h)
{
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

static const uint8_t *guest(uint32_t va)
{
    return (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

/* ── the replay file ────────────────────────────────────────────── */

static void replay_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char *line;
    size_t cap = 1 << 16;
    uint32_t cap_ticks = 0;

    if (!f) {
        fprintf(stderr, "[LOCKSTEP] cannot open the recording %s: no input will be replayed\n", path);
        return;
    }
    line = (char *)malloc(cap);
    while (line && fgets(line, (int)cap, f)) {
        char *p = line, *q;
        uint32_t t;
        ReplayTick *rt;
        unsigned port;

        if (strncmp(p, "# epoch=", 8) == 0) {
            s_epoch = (int64_t)strtoll(p + 8, NULL, 10);
            continue;
        }
        if (strncmp(p, "t=", 2) != 0)
            continue;
        t = (uint32_t)strtoul(p + 2, &q, 10);
        if (t == 0 || t > 50000000u)
            continue;
        if (t > cap_ticks) {
            uint32_t n = cap_ticks ? cap_ticks : 4096;
            while (n < t)
                n *= 2;
            s_replay = (ReplayTick *)realloc(s_replay, (size_t)n * sizeof *s_replay);
            memset(s_replay + cap_ticks, 0, (size_t)(n - cap_ticks) * sizeof *s_replay);
            if (s_replay_chunk_count) {
                s_replay_chunks = (uint32_t *)realloc(s_replay_chunks,
                    (size_t)n * s_replay_chunk_count * sizeof(uint32_t));
                memset(s_replay_chunks + (size_t)cap_ticks * s_replay_chunk_count, 0,
                       (size_t)(n - cap_ticks) * s_replay_chunk_count * sizeof(uint32_t));
            }
            cap_ticks = n;
        }
        rt = &s_replay[t - 1];
        rt->present = 1;
        if (t > s_replay_count)
            s_replay_count = t;
        q = strstr(p, " m=");
        rt->mask = q ? (unsigned)strtoul(q + 3, NULL, 16) : 0u;
        q = strstr(p, " p=");
        if (q) {
            q += 3;
            for (port = 0; port < RECOMP_LOCKSTEP_PORTS; port++) {
                uint8_t b[18];
                int i;
                if (!(rt->mask & (1u << port)))
                    continue;
                for (i = 0; i < 18; i++) {
                    int hi = hexval(q[2 * i]), lo = hexval(q[2 * i + 1]);
                    if (hi < 0 || lo < 0)
                        break;
                    b[i] = (uint8_t)(hi << 4 | lo);
                }
                if (i == 18)
                    pad_from_bytes(&rt->pads[port], b);
                q += 36;
                if (*q == ',')
                    q++;
            }
        }
        q = strstr(p, " h=");
        if (q) {
            rt->hash = strtoull(q + 3, NULL, 16);
            rt->has_hash = 1;
        }
        q = strstr(p, " c=");
        if (q) {
            unsigned n = 0, i;
            q += 3;
            while (hexval(q[n]) >= 0)
                n++;
            n /= 8;
            if (!s_replay_chunk_count && n) {
                s_replay_chunk_count = n;
                s_replay_chunks = (uint32_t *)calloc((size_t)cap_ticks * n, sizeof(uint32_t));
            }
            if (n == s_replay_chunk_count && s_replay_chunks)
                for (i = 0; i < n; i++) {
                    char tmp[9];
                    memcpy(tmp, q + 8 * i, 8);
                    tmp[8] = 0;
                    s_replay_chunks[(size_t)(t - 1) * n + i] = (uint32_t)strtoul(tmp, NULL, 16);
                }
        }
    }
    free(line);
    fclose(f);
    /* A tick with no line holds the input of the tick before it. */
    {
        uint32_t t;
        for (t = 2; t <= s_replay_count; t++)
            if (!s_replay[t - 1].present) {
                s_replay[t - 1].mask = s_replay[t - 2].mask;
                memcpy(s_replay[t - 1].pads, s_replay[t - 2].pads, sizeof s_replay[t - 1].pads);
            }
    }
    fprintf(stderr, "[LOCKSTEP] replaying %u ticks of input from %s\n", s_replay_count, path);
}

/* ── init ───────────────────────────────────────────────────────── */

static void init(void)
{
    const char *on = getenv("RECOMP_LOCKSTEP");
    const char *replay = getenv("RECOMP_LOCKSTEP_REPLAY");
    const char *trace = getenv("RECOMP_LOCKSTEP_TRACE");
    const char *ports = getenv("RECOMP_LOCKSTEP_PORTS");
    const char *dump = getenv("RECOMP_LOCKSTEP_DUMP");
    const char *end = getenv("RECOMP_LOCKSTEP_END");

    s_active = (on && *on && strcmp(on, "0") != 0) || (replay && *replay);
    if (!s_active)
        return;
    if (ports && *ports) {
        long n = strtol(ports, NULL, 10);
        if (n < 1) n = 1;
        if (n > RECOMP_LOCKSTEP_PORTS) n = RECOMP_LOCKSTEP_PORTS;
        s_ports_mask = (1u << n) - 1u;
    }
    if (replay && *replay)
        replay_load(replay);
    {
        const char *e = getenv("RECOMP_LOCKSTEP_EPOCH");
        if (e && *e)
            s_epoch = (int64_t)strtoll(e, NULL, 10);
    }
    if (!s_epoch) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        /* Whole seconds: the clock is the session's, not this instant's. */
        s_epoch = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        s_epoch -= s_epoch % 10000000;
    }
    if (trace && *trace) {
        snprintf(s_trace_path, sizeof s_trace_path, "%s", trace);
        s_trace = fopen(trace, "w");
        if (!s_trace)
            fprintf(stderr, "[LOCKSTEP] cannot write the trace %s\n", trace);
        else
            fprintf(s_trace, "# epoch=%lld\n", (long long)s_epoch);
    }
    while (dump && *dump && s_dump_count < MAX_DUMPS) {
        char *e;
        unsigned long t = strtoul(dump, &e, 10);
        if (e == dump)
            break;
        s_dumps[s_dump_count++] = (uint32_t)t;
        dump = *e == ',' ? e + 1 : e;
    }
    if (end && *end)
        s_end_tick = (uint32_t)strtoul(end, NULL, 10);
    {
        const char *c = getenv("RECOMP_LOCKSTEP_CALLS");
        if (c && *c) {
            char *e;
            char path[1100];
            s_calls_from = (uint32_t)strtoul(c, &e, 10);
            s_calls_to = *e == '-' ? (uint32_t)strtoul(e + 1, NULL, 10) : s_calls_from;
            snprintf(path, sizeof path, "%s.calls", s_trace_path[0] ? s_trace_path : "lockstep");
            s_calls = fopen(path, "w");
        }
    }
    {
        const char *c = getenv("RECOMP_LOCKSTEP_CHUNK");
        unsigned long n = c && *c ? strtoul(c, NULL, 0) : 0;
        if (n >= 256 && n <= (1u << 24) && (n & (n - 1)) == 0)
            s_chunk = (uint32_t)n;
    }
    fprintf(stderr, "[LOCKSTEP] on: one step per frame, controllers latched per frame, "
                    "connected mask 0x%X from tick %u%s%s\n",
            s_ports_mask, ENUM_TICKS, s_trace ? ", trace " : "", s_trace ? s_trace_path : "");
    fflush(stderr);
}

/* Clock reads reach this from any thread, the first possibly before the
 * title's: one of them runs init, the others wait for it. */
static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    init();
    s_init = 1;
    return TRUE;
}

static void ensure_init(void)
{
    if (!s_init)
        InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
}

uint64_t recomp_lockstep_time_100ns(void)
{
    uint32_t tick = s_tick;
    uint64_t t;

    if (tick != t_time_tick) {
        t_time_tick = tick;
        t_time_reads = 0;
    }
    t_time_reads++;
    t = (uint64_t)tick * 10000000ull / 60ull + t_time_reads * 10ull;
    if (t <= t_time_last)
        t = t_time_last + 1;      /* a thread that read ahead of the tick stays monotonic */
    t_time_last = t;
    return t;
}

uint64_t recomp_lockstep_frame_ms(void)
{
    return (uint64_t)s_tick * 1000ull / 60ull;
}

int64_t recomp_lockstep_epoch(void)
{
    ensure_init();
    return s_epoch;
}

int recomp_lockstep_active(void)
{
    ensure_init();
    return s_active;
}

uint32_t recomp_lockstep_tick(void)
{
    return s_tick;
}

void recomp_lockstep_set_sampler(RecompLockstepSampler fn)
{
    s_sampler = fn;
}

void recomp_lockstep_add_region(uint32_t va, uint32_t size, const char *name)
{
    Region *r;
    unsigned chunks;

    if (s_region_count >= MAX_REGIONS || !size)
        return;
    r = &s_regions[s_region_count++];
    r->va = va;
    r->size = size;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    chunks = (size + CHUNK - 1) / CHUNK;
    s_chunk_hash = (uint32_t *)realloc(s_chunk_hash, (s_chunk_count + chunks) * sizeof(uint32_t));
    fprintf(stderr, "[LOCKSTEP] hashing %s: 0x%08X-0x%08X, chunks %u-%u\n", r->name,
            va, va + size, s_chunk_count, s_chunk_count + chunks - 1);
    s_chunk_count += chunks;
}

void recomp_lockstep_ignore(uint32_t va, uint32_t size)
{
    int i, j;

    if (s_ignore_count >= MAX_IGNORES || !size)
        return;
    for (i = 0; i < s_ignore_count && s_ignores[i].lo < va; i++)
        ;
    for (j = s_ignore_count; j > i; j--)
        s_ignores[j] = s_ignores[j - 1];
    s_ignores[i].lo = va;
    s_ignores[i].hi = va + size;
    s_ignore_count++;
}

/* One chunk's hash, leaving out the ignored bytes. */
static uint64_t hash_span(uint32_t lo, uint32_t hi)
{
    uint64_t h = 0xCBF29CE484222325ull;
    int i;

    for (i = 0; i < s_ignore_count && lo < hi; i++) {
        if (s_ignores[i].hi <= lo)
            continue;
        if (s_ignores[i].lo >= hi)
            break;
        if (s_ignores[i].lo > lo)
            h = hash_bytes(h, guest(lo), s_ignores[i].lo - lo);
        lo = s_ignores[i].hi;
    }
    if (lo < hi)
        h = hash_bytes(h, guest(lo), hi - lo);
    return mix64(h);
}

unsigned recomp_lockstep_connected_mask(void)
{
    if (!recomp_lockstep_active())
        return 0;
    if (s_replay && s_tick >= 1 && s_tick <= s_replay_count)
        return s_replay[s_tick - 1].mask;
    return s_tick >= ENUM_TICKS ? s_ports_mask : 0u;
}

int recomp_lockstep_pad(unsigned port, RecompLockstepPad *out)
{
    if (port >= RECOMP_LOCKSTEP_PORTS || !(s_mask & (1u << port))) {
        memset(out, 0, sizeof *out);
        return 0;
    }
    *out = s_pads[port];
    return 1;
}

/* Which chunk index, as "region+offset", for the mismatch report. */
static void chunk_where(unsigned c, char *buf, size_t n)
{
    int i;
    for (i = 0; i < s_region_count; i++) {
        unsigned k = (s_regions[i].size + CHUNK - 1) / CHUNK;
        if (c < k) {
            uint32_t lo = s_regions[i].va + c * CHUNK;
            uint32_t hi = lo + CHUNK;
            if (hi > s_regions[i].va + s_regions[i].size)
                hi = s_regions[i].va + s_regions[i].size;
            snprintf(buf, n, "%s 0x%08X-0x%08X", s_regions[i].name, lo, hi);
            return;
        }
        c -= k;
    }
    snprintf(buf, n, "chunk %u", c);
}

static uint64_t hash_state(void)
{
    uint64_t all = 0xCBF29CE484222325ull;
    unsigned c = 0;
    int i;

    for (i = 0; i < s_region_count; i++) {
        uint32_t off;
        for (off = 0; off < s_regions[i].size; off += CHUNK) {
            uint32_t n = s_regions[i].size - off;
            uint64_t h;
            if (n > CHUNK)
                n = CHUNK;
            h = hash_span(s_regions[i].va + off, s_regions[i].va + off + n);
            s_chunk_hash[c++] = (uint32_t)(h ^ (h >> 32));
            all = (all ^ h) * 0x100000001B3ull;
        }
    }
    return mix64(all);
}

static void write_dump(uint32_t tick)
{
    char path[1100];
    FILE *f;
    int i;

    snprintf(path, sizeof path, "%s.t%u.bin", s_trace_path[0] ? s_trace_path : "lockstep", tick);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[LOCKSTEP] cannot write %s\n", path);
        return;
    }
    fwrite("LSDUMP1\n", 1, 8, f);
    fwrite(&s_region_count, 4, 1, f);
    for (i = 0; i < s_region_count; i++) {
        fwrite(&s_regions[i].va, 4, 1, f);
        fwrite(&s_regions[i].size, 4, 1, f);
        fwrite(s_regions[i].name, 1, 32, f);
        fwrite(guest(s_regions[i].va), 1, s_regions[i].size, f);
    }
    fclose(f);
    fprintf(stderr, "[LOCKSTEP] tick %u: memory written to %s\n", tick, path);
}

static void check_replay(uint32_t tick, uint64_t h)
{
    const ReplayTick *rt;

    if (!s_replay || tick > s_replay_count)
        return;
    rt = &s_replay[tick - 1];
    if (!rt->has_hash)
        return;
    if (rt->hash == h) {
        if (tick % 600 == 0)
            fprintf(stderr, "[LOCKSTEP] tick %u: the same as the recording (%u ticks differed so far)\n",
                    tick, s_mismatches);
        return;
    }
    if (!s_mismatches++)
        s_first_mismatch = tick;
    if (s_mismatches <= 8) {
        unsigned c, shown = 0;
        fprintf(stderr, "[LOCKSTEP] tick %u: DIFFERS from the recording (hash %016llX, recorded %016llX)%s\n",
                tick, (unsigned long long)h, (unsigned long long)rt->hash,
                s_mismatches == 1 ? " -- the first tick that does" : "");
        if (s_replay_chunks && s_replay_chunk_count == s_chunk_count)
            for (c = 0; c < s_chunk_count && shown < 12; c++)
                if (s_replay_chunks[(size_t)(tick - 1) * s_chunk_count + c] != s_chunk_hash[c]) {
                    char where[96];
                    chunk_where(c, where, sizeof where);
                    fprintf(stderr, "[LOCKSTEP]   chunk %u differs: %s\n", c, where);
                    shown++;
                }
    }
    fflush(stderr);
}

/* ── Boundary hooks and the other guest threads ─────────────────── */

#define MAX_HOOKS 8
static void (*s_hooks[MAX_HOOKS])(void);
static int s_hook_count;

void recomp_lockstep_add_boundary_hook(void (*fn)(void))
{
    if (fn && s_hook_count < MAX_HOOKS)
        s_hooks[s_hook_count++] = fn;
}

enum { SEC_FREE, SEC_PARKED, SEC_WAITING, SEC_WANT, SEC_RUNNING };
#define MAX_SECONDARY 8
typedef struct Secondary {
    DWORD id;
    HANDLE turn;                  /* auto-reset: the frame thread grants a turn */
    volatile LONG state;
    volatile uint32_t wake_tick;
} Secondary;
static Secondary s_sec[MAX_SECONDARY];
static volatile LONG s_sec_count;
static HANDLE s_turn_done;        /* auto-reset: the thread with the turn gave it back */
static LOCKSTEP_TLS int t_sec = -1;  /* index in s_sec, or -2: not a secondary thread */
static int s_said_turn_timeout;

/* The calling thread's slot, registering it on first use. Only a thread
 * other than the frame's, once frames have started, is one. */
static Secondary *secondary_self(int register_it)
{
    LONG n;

    if (t_sec >= 0)
        return &s_sec[t_sec];
    if (t_sec == -2 || !s_tick || GetCurrentThreadId() == s_frame_thread)
        return NULL;
    if (!register_it)
        return NULL;
    n = InterlockedIncrement(&s_sec_count) - 1;
    if (n >= MAX_SECONDARY) {
        InterlockedDecrement(&s_sec_count);
        t_sec = -2;
        fprintf(stderr, "[LOCKSTEP] more than %d other guest threads: thread %lu runs "
                        "on the host's clock\n", MAX_SECONDARY, (unsigned long)GetCurrentThreadId());
        return NULL;
    }
    if (!s_turn_done)
        s_turn_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_sec[n].turn = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_sec[n].state = SEC_FREE;
    s_sec[n].id = GetCurrentThreadId();
    t_sec = (int)n;
    fprintf(stderr, "[LOCKSTEP] guest thread %lu runs only at frame boundaries from tick %u\n",
            (unsigned long)s_sec[n].id, s_tick);
    fflush(stderr);
    return &s_sec[n];
}

/* Leave the running state for `state`; hand the turn back if it was held. */
static void give_back(Secondary *s, LONG state)
{
    LONG prev = InterlockedExchange(&s->state, state);
    if (prev == SEC_RUNNING)
        SetEvent(s_turn_done);
}

int recomp_lockstep_secondary_delay(int64_t interval_100ns)
{
    Secondary *s;
    uint64_t now, wake;
    uint32_t tick;

    if (!recomp_lockstep_active() || !(s = secondary_self(1)))
        return 0;
    tick = s_tick;
    now = (uint64_t)tick * 10000000ull / 60ull;
    /* Negative is relative; an absolute time is on the session's clock. */
    if (interval_100ns < 0)
        wake = now + (uint64_t)(-interval_100ns);
    else if ((uint64_t)interval_100ns > (uint64_t)s_epoch)
        wake = (uint64_t)interval_100ns - (uint64_t)s_epoch;
    else
        wake = now;
    /* The first frame whose time has reached it, and never this one: a
     * Sleep(0) poll waits for the frame thread to have moved on. */
    s->wake_tick = (uint32_t)((wake * 60ull + 9999999ull) / 10000000ull);
    if (s->wake_tick <= tick)
        s->wake_tick = tick + 1;
    give_back(s, SEC_PARKED);
    WaitForSingleObject(s->turn, INFINITE);
    return 1;
}

int recomp_lockstep_secondary_block_begin(void)
{
    Secondary *s;

    if (!s_active || !(s = secondary_self(0)))
        return 0;
    give_back(s, SEC_WAITING);
    return 1;
}

void recomp_lockstep_secondary_block_end(void)
{
    Secondary *s = secondary_self(0);

    if (!s)
        return;
    InterlockedExchange(&s->state, SEC_WANT);
    WaitForSingleObject(s->turn, INFINITE);
}

/* The frame thread, at the boundary: each other guest thread that is due
 * runs until it blocks again, in the order they were first seen. */
static void run_secondaries(uint32_t tick)
{
    LONG i, n = s_sec_count;

    for (i = 0; i < n; i++) {
        Secondary *s = &s_sec[i];
        LONG st = s->state;
        int held;

        if (!((st == SEC_PARKED && s->wake_tick <= tick) || st == SEC_WANT))
            continue;
        InterlockedExchange(&s->state, SEC_RUNNING);
        SetEvent(s->turn);
        held = xbox_GuestLockDropForKernel(1, 0u);
        if (WaitForSingleObject(s_turn_done, 10000) != WAIT_OBJECT_0) {
            /* A thread that never blocks again would hold the game forever. */
            if (!s_said_turn_timeout++)
                fprintf(stderr, "[LOCKSTEP] tick %u: guest thread %lu kept its turn for 10 s; "
                                "the frame goes on without it\n", tick, (unsigned long)s->id);
        }
        xbox_GuestLockRestoreForKernel(held);
    }
}

int recomp_lockstep_noting(void)
{
    uint32_t t = s_tick;
    return s_calls && t >= s_calls_from && t <= s_calls_to &&
           GetCurrentThreadId() == s_frame_thread;
}

void recomp_lockstep_note(const char *what, uint32_t a, uint32_t b, uint32_t c)
{
    if (!recomp_lockstep_noting())
        return;
    fprintf(s_calls, "%u %s %08X %08X %08X\n", s_tick, what, a, b, c);
}

void recomp_lockstep_frame(void)
{
    uint32_t tick;
    uint64_t h = 0;
    unsigned port;
    int i;

    if (!recomp_lockstep_active())
        return;
    tick = ++s_tick;
    s_frame_thread = GetCurrentThreadId();
    if (s_calls && tick == s_calls_to + 1)
        fflush(s_calls);

    if (s_region_count) {
        h = hash_state();
        check_replay(tick, h);
    }
    for (i = 0; i < s_dump_count; i++)
        if (s_dumps[i] == tick)
            write_dump(tick);

    /* What other threads would otherwise do to the guest at any moment:
     * here, in order, on this thread, before this frame runs. */
    for (i = 0; i < s_hook_count; i++)
        s_hooks[i]();
    run_secondaries(tick);

    /* This frame's controllers. */
    s_mask = recomp_lockstep_connected_mask();
    for (port = 0; port < RECOMP_LOCKSTEP_PORTS; port++) {
        memset(&s_pads[port], 0, sizeof s_pads[port]);
        if (!(s_mask & (1u << port)))
            continue;
        if (s_replay) {
            if (tick <= s_replay_count)
                s_pads[port] = s_replay[tick - 1].pads[port];
        } else if (s_sampler) {
            s_sampler(port, &s_pads[port]);
        }
    }
    if (s_replay && tick == s_replay_count + 1 && !s_said_replay_end) {
        s_said_replay_end = 1;
        fprintf(stderr, "[LOCKSTEP] tick %u: the recording ends here (%u of its ticks differed, "
                        "the first at %u)\n", tick, s_mismatches, s_first_mismatch);
        fflush(stderr);
    }

    if (s_trace) {
        fprintf(s_trace, "t=%u m=%X p=", tick, s_mask);
        for (port = 0; port < RECOMP_LOCKSTEP_PORTS; port++) {
            uint8_t b[18];
            int k;
            if (!(s_mask & (1u << port)))
                continue;
            pad_to_bytes(&s_pads[port], b);
            for (k = 0; k < 18; k++)
                fprintf(s_trace, "%02X", b[k]);
            fputc(',', s_trace);
        }
        if (s_region_count) {
            unsigned c;
            fprintf(s_trace, " h=%016llX c=", (unsigned long long)h);
            for (c = 0; c < s_chunk_count; c++)
                fprintf(s_trace, "%08X", s_chunk_hash[c]);
        }
        fputc('\n', s_trace);
        fflush(s_trace);
    }

    if (s_end_tick && tick >= s_end_tick) {
        fprintf(stderr, "[LOCKSTEP] tick %u: ending the run (RECOMP_LOCKSTEP_END)\n", tick);
        if (s_replay && s_mismatches)
            fprintf(stderr, "[LOCKSTEP] %u tick(s) differed from the recording, the first at %u\n",
                    s_mismatches, s_first_mismatch);
        else if (s_replay)
            fprintf(stderr, "[LOCKSTEP] every tick was the same as the recording\n");
        fflush(stderr);
        if (s_trace)
            fclose(s_trace);
        _Exit(0);
    }
}
