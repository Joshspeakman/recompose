# Lockstep: one game on every machine (online co-op)

A game that only ever had split screen can be played online the way
emulator netplay plays it: every machine runs the whole game, and only the
controllers cross the network. Each frame every copy reads the same pads and
steps the game by the same amount, so every copy computes the same game. The
copies never exchange game state; they prove they still agree by comparing a
hash of it every 60 frames.

That only works if nothing but the controllers can make two copies differ.
In a recompilation the game runs as native code beside a host that has its
own clocks, threads, GPU and sound device, so most of this module is about
taking those out of the game's reach.

Code: `src/kernel/recomp_lockstep.h` / `.c` (the frame, the clocks, the
check), `src/kernel/recomp_lockstep_net.c` (the network), and small hooks
where the kernel and the HLE answer the game. All of it is off unless
`RECOMP_LOCKSTEP`, `RECOMP_LOCKSTEP_REPLAY` or `RECOMP_LOCKSTEP_NET` is set,
or the title's settings file says `coop = host|join`.

## What a title provides

- **Its frame boundary.** The title calls `recomp_lockstep_frame()` once a
  frame, at the top of its own frame (TimeSplitters 2: its frame clock,
  `ts2_frame_clock` in its `src/recomp_manual.c`).
- **One step per frame.** A title that steps by elapsed time or vblanks reads
  `recomp_lockstep_tick()` instead (TS2 steps by the vblanks since the last
  frame, 1 to 3; its vblank counter answers the tick).
- **The state to hash.** `recomp_lockstep_add_region(va, size, name)` for its
  globals, and `recomp_lockstep_ignore()` for bytes in them that host threads
  write and the game only reads for timing (TS2: the vblank counter, the
  music streamer's own state).
- **Its build and settings.** `recomp_lockstep_set_build_id()` (TS2 stamps both
  repositories' commits, `cmake/build_id.cmake`) and
  `recomp_lockstep_add_setting()` for anything that changes the game itself;
  a joiner whose build or settings differ is refused, told which.
- **The start.** `recomp_lockstep_start()` after the kernel and paths are up,
  before the entry point: a joiner meets its host there.
- **Each player's own screen** (optional, as GoldenEye's online co-op does
  it). Every machine runs every player's pass, so the layout must change the
  same way on all of them: the title lays every player's view out as the
  whole screen (TS2: its viewport-rectangle function, given the whole frame
  when a player's view is laid out), and makes it a session setting. Then
  `xbox_D3D8SetDrawFilter(keep)` leaves out of the host's drawing every draw
  and clear made while `keep()` answers 0, that is during another player's
  pass; the game's own D3D code still runs, so nothing it sees changes.
  `xbox_D3D8SetPresentCrop` is the cheaper fallback: it shows one part of a
  split-screen frame, at the shape the game drew it.

## What the toolkit pins down

| Source of difference | Under lockstep |
| --- | --- |
| Frame length | One step per frame everywhere; a slow machine slows the game rather than skipping ahead |
| Controllers | Latched once a frame for every connected port; which ports, from which frame (30), fixed |
| Clocks | `rdtsc`, `KeQueryPerformanceCounter`, `KeQueryInterruptTime`, `KeQuerySystemTime`, `KeTickCount` count frames from the session's epoch (1 us more per read within a frame, per thread); time zone UTC; file times the epoch. TS2 seeds its random numbers from the time of day |
| Sound | DirectSound's play state on frame time (whether a sound has finished feeds TS2's voice pool); stream packets and notifications complete at the frame boundary, never from the host device's position |
| Other guest threads | Run only at frame boundaries, one at a time, while the frame's thread waits; a Sleep is counted in frames. TS2's music streamer is one, and its level start waited on it |
| GPU answers | Visibility tests answered at once with a fixed count (`RECOMP_HLE_D3D8_VISIBLE_PIXELS`, default 0) |
| Floating point | `-ffp-contract=off` on lifted code; transcendentals from `src/math` (musl); x87 NaNs as the x87 makes them on every host (`RECOMP_X87` in `templates/runtime/recomp_types.h`): a NaN made from numbers is the negative indefinite on ARM too, and of two NaNs the one with the larger significand, then the positive one |
| Saves | The joiner plays from the host's saves (E:'s `UDATA\<id>`, `TDATA\<id>`), handed over before its game starts, in `<save dir>/coop-session/<id>` |
| Directory order | Listings on macOS and Linux sorted as NTFS sorts them |
| Start-up | The timer thread's stack and TIB are taken before it starts, so its allocations cannot race the title's (a joiner's handshake changed the timing on a Mac and shifted every heap address) |

Each row was a real difference between two runs of the same input, found
with the tools below.

## Finding a difference

A **trace** (`RECOMP_LOCKSTEP_TRACE=<file>`) holds each frame's controllers
and a hash of the named state per 4 KB page. A **replay**
(`RECOMP_LOCKSTEP_REPLAY=<trace>`) feeds a trace's controllers back and says
at which frame the state first differs, and in which pages. Then:

```bash
py -3 -m tools.lockstep trace a.trace b.trace --base <first region's va>
#   the first frame each page differs, and for how many frames:
#   a page that differs for a few frames and recovers is usually a host thread
#   writing; one that differs from a frame on is the game going another way
RECOMP_LOCKSTEP_DUMP=<frame>          # both runs: the named memory at that frame
py -3 -m tools.lockstep dump a.trace.t<frame>.bin b.trace.t<frame>.bin
#   the words that differ, as guest addresses
RECOMP_LOCKSTEP_CALLS=<from>-<to>     # the frame thread's kernel calls and answers
RECOMP_WATCH_WRITE=<va>               # who writes the word (memory-watchpoints.md)
```

Two copies replaying one trace in parallel (more load, more timing variety)
find timing leaks fastest; a recording replayed on another system finds
compiler and architecture differences. `tools.lockstep compact` shrinks a
trace to the frames whose input changes plus a hash every 60 frames, small
enough to keep in a repository as a regression test (TS2:
`tests/lockstep/`).

## The network

`RECOMP_LOCKSTEP_NET=host` listens (UDP 27620, `RECOMP_LOCKSTEP_PORT`);
`=join` with `RECOMP_LOCKSTEP_PEER=<address>`. Or the settings file:
`coop = host|join`, `coop_port`, and the System Link tab's address
(`syslink_host`); the launcher's System Link tab writes them for a title
built with `recomp_add_launcher(... COOP)`. The host plays controller 1 and
the joiner controller 2, each with its own machine's controller 1. A
controller lands some frames after it is read, on every machine: the joiner
pings the host as it joins and starts the session with the delay that covers
half the round trip plus two frames (3 on a LAN, 9 at 100 ms each way), and
the host's first frame waits for that; `coop_delay` / `RECOMP_LOCKSTEP_DELAY`
fixes it instead. Every message repeats what the other side has not
acknowledged, so a lost packet costs a resend.
`RECOMP_LOCKSTEP_SIM_DELAY_MS` and `_LOSS` try a slow, lossy line on one
machine. The window's title says what the session is doing
(`recomp_lockstep_set_status_sink`): a game waiting on the network draws
nothing. A frame starts only when both
controllers for it are in. The host asks the router for its port (UPnP)
unless `syslink_upnp = 0`; a joiner needs nothing opened.

With `RECOMP_LOCKSTEP_REPLAY` as well, each machine plays its own
controller's column of the recording: a session between two machines then
reproduces a recorded game, checked against the recording and against each
other.

## Measured (23 November 2026, TimeSplitters 2 USA)

A recording of a fresh two-player Story Co-op game, menus into Siberia with
both players moving and firing, 8,000 frames:

- the same at every frame on Windows (Direct3D 11 and Vulkan), Linux (clang,
  WSL) and macOS (Apple Silicon, Metal), replayed offline;
- online between two copies on one PC, Windows host and Linux joiner (with
  the host's saved profile handed over), and Windows host and Mac joiner over
  a LAN: see the title's `docs/multiplayer.md` for the latest.

## Not done

- A desync is reported, not repaired: there is no state transfer.
- More than two players.
