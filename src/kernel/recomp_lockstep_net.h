/*
 * recomp_lockstep_net.h -- the network half of lockstep, used only by
 * recomp_lockstep.c. What it does and its switches: recomp_lockstep_net.c.
 */
#ifndef XBOXRECOMP_LOCKSTEP_NET_H
#define XBOXRECOMP_LOCKSTEP_NET_H

#include <stdint.h>
#include "recomp_lockstep.h"

/* RECOMP_LOCKSTEP_NET=host|join: open the session. A joiner waits here for
 * the host's answer and takes the host's epoch into *epoch. 0 when not
 * asked for or it could not start (no online play then). */
int lsnet_init(int64_t *epoch);
/* The controller this machine plays (0 for the host, 1 for the joiner). */
unsigned lsnet_local_port(void);
uint32_t lsnet_delay(void);
/* One frame: this machine's controller now (it lands `delay` frames on),
 * this frame's checkpoint hash if it has one, and back every player's
 * controller for this frame, waiting for the other machine's if need be. */
void lsnet_frame(uint32_t tick, const RecompLockstepPad *local_now, int have_checkpoint,
                 uint64_t hash, RecompLockstepPad *pads_out);
void lsnet_leave(void);
/* A joiner's session folder, holding the host's saves: its E:. */
const char *lsnet_session_dir(void);
void lsnet_report(void);

#endif
