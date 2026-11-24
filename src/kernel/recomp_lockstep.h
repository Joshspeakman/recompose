/*
 * recomp_lockstep.h -- one game computed identically on every machine.
 *
 * Online co-op for a game that only ever had split screen works the way
 * emulator netplay does: every machine runs the whole game, and they agree
 * on nothing but the controllers. Each frame, every copy reads the same pads
 * and steps the game by the same amount, so every copy computes the same
 * game. That holds only if the frame is the unit of time everywhere, so this
 * module owns three things:
 *
 *   - the frame counter (a tick). The title calls recomp_lockstep_frame() at
 *     its own frame boundary; a title that steps by elapsed vblanks reads
 *     recomp_lockstep_tick() instead, so every frame is exactly one step.
 *   - the controllers. At each tick the pads of every connected port are
 *     latched (from this machine's devices, from a recording, or later from
 *     the other machines), and the title reads that latch for the whole
 *     frame. Which ports are connected, and from which tick, is fixed too.
 *   - the check. A trace file gets each tick's input and a hash of the game
 *     memory the title names (recomp_lockstep_add_region), per 4 KB chunk,
 *     so two runs can be compared tick by tick and the first chunk that
 *     differs found.
 *
 * Switches (all off by default; lockstep is on when any of the first two is set):
 *   RECOMP_LOCKSTEP=1             fixed step and latched controllers, input from
 *                                 this machine (pads, keyboard, input scripts)
 *   RECOMP_LOCKSTEP_REPLAY=<file> input from a trace written before; its hashes
 *                                 are checked against this run's, tick by tick
 *   RECOMP_LOCKSTEP_TRACE=<file>  write this run's trace
 *   RECOMP_LOCKSTEP_PORTS=<n>     controllers 1..n connected (default 2)
 *   RECOMP_LOCKSTEP_DUMP=<t>[,<t>...]  write the hashed memory at those ticks
 *                                 to <trace>.t<tick>.bin (py -m tools.lockstep dump)
 *   RECOMP_LOCKSTEP_END=<t>       end the process at that tick
 *   RECOMP_LOCKSTEP_CALLS=<a>-<b> log the frame thread's kernel calls and
 *                                 noted HLE answers in ticks a..b (see below)
 *   RECOMP_LOCKSTEP_CHUNK=<bytes> the trace's hash granularity (default 4096;
 *                                 two traces compare only at the same size)
 *   RECOMP_LOCKSTEP_NET=host|join online, between machines: recomp_lockstep_net.c
 *   RECOMP_LOCKSTEP_EPOCH=<n>     the session's wall clock at tick 0 (FILETIME,
 *                                 decimal); a recording carries its own
 */
#ifndef XBOXRECOMP_LOCKSTEP_H
#define XBOXRECOMP_LOCKSTEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_LOCKSTEP_PORTS 4

/* One controller as the title reads it: the XINPUT_GAMEPAD fields. */
typedef struct RecompLockstepPad {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} RecompLockstepPad;

/* Reads this machine's device for a port (the input layer registers it). */
typedef void (*RecompLockstepSampler)(unsigned port, RecompLockstepPad *out);

int recomp_lockstep_active(void);
/* The session begins: online, a joiner meets its host here (the host's
 * clock, its saves, the settings checked) and a host opens its port. The
 * title calls it once the kernel and the paths are up, before its entry
 * point; without that call it happens at the first frame. */
void recomp_lockstep_start(void);
void recomp_lockstep_frame(void);
uint32_t recomp_lockstep_tick(void);
/* Ports connected now: none until the console's USB would have found them
 * (half a second of ticks), then the session's. */
unsigned recomp_lockstep_connected_mask(void);
/* This tick's latched pad for a port; 0 if the port is not connected. */
int recomp_lockstep_pad(unsigned port, RecompLockstepPad *out);
void recomp_lockstep_set_sampler(RecompLockstepSampler fn);
/* Guest memory that is game state, hashed into the trace each tick. */
void recomp_lockstep_add_region(uint32_t va, uint32_t size, const char *name);
/* Bytes inside a region that are not game state and change on their own
 * (a counter a host thread bumps, a timer kept only for a debug display):
 * left out of the hash so the trace shows what does matter. */
void recomp_lockstep_ignore(uint32_t va, uint32_t size);

/* Guest time under lockstep, in 100 ns units since the session began: the
 * tick (a frame is 1/60 s), plus 1 us for each read the calling thread has
 * made in this frame, so a guest loop that waits for time to pass still
 * ends, and the frame's own thread reads the same values on every machine.
 * Every clock the kernel shows the guest (rdtsc, KeQueryPerformanceCounter,
 * KeQueryInterruptTime, KeQuerySystemTime, KeTickCount) comes from this
 * while lockstep is on: TimeSplitters 2 seeds its random numbers from the
 * time of day, and any clock left on the host's time would make two
 * machines play two different games. */
uint64_t recomp_lockstep_time_100ns(void);
/* The tick as milliseconds (tick * 1000 / 60), with nothing added per read:
 * for a model two threads advance (the DirectSound play state, which the
 * game thread asks about and the audio thread plays from), so both see one
 * time within a frame. */
uint64_t recomp_lockstep_frame_ms(void);
/* RECOMP_LOCKSTEP_CALLS=<from>-<to>: within those ticks, what the frame's
 * own thread was told -- every kernel call's ordinal, caller and result, and
 * what the replaced XDK functions that note themselves answered -- one line
 * each, to <trace>.calls. Two runs of one recording that part ways are
 * compared there: the first line that differs is the answer that differed.
 * recomp_lockstep_noting() is the cheap test; note() writes one line. */
int recomp_lockstep_noting(void);
void recomp_lockstep_note(const char *what, uint32_t a, uint32_t b, uint32_t c);

/* Work that must happen at the frame boundary, on the frame's own thread,
 * because the guest sees it: a sound stream's packets finishing, a buffer's
 * notification positions passing. Called each tick before the other guest
 * threads get their turn. */
void recomp_lockstep_add_boundary_hook(void (*fn)(void));

/* The title's other guest threads (TimeSplitters 2's music streamer) run on
 * the host's clock and would touch game state at a different moment on
 * every machine. Under lockstep they run only at frame boundaries, one at a
 * time, while the frame's thread waits: a Sleep on one parks it until the
 * frame whose time has reached its wake time, and any other blocking call
 * gives up its turn and waits for the next boundary when it returns.
 * secondary_delay returns 1 when it parked the caller (the kernel then skips
 * the host sleep), 0 when the caller is not such a thread. block_begin
 * returns 1 when block_end must follow the call. */
int recomp_lockstep_secondary_delay(int64_t interval_100ns);
int recomp_lockstep_secondary_block_begin(void);
void recomp_lockstep_secondary_block_end(void);

/* An online joiner's E: (the hard disk's data partition): a folder holding
 * the host's saves for this title, which the session handed over before the
 * game started. NULL when this copy is not one. */
const char *recomp_lockstep_hdd_dir(void);
/* A setting that changes the game itself, not only how it is shown (the
 * widescreen camera widens the title's own view): every machine in a
 * session must have the same, and a joiner whose differs is refused, told
 * which. The title adds its own before it starts; the toolkit adds
 * widescreen. */
void recomp_lockstep_add_setting(const char *name, const char *value);

/* What build this is, as the title names it (its sources' commits): an
 * online session refuses a machine whose build differs, since a different
 * build computes a different game. Called before the title starts. */
void recomp_lockstep_set_build_id(const char *id);

/* The session's wall clock at tick 0, as a FILETIME: the recording's, the
 * host machine's, or this machine's when it is alone. */
int64_t recomp_lockstep_epoch(void);

#ifdef __cplusplus
}
#endif

#endif
