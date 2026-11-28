"""The online server: one UDP port that players' copies connect to, so nobody has to open
a port on their own router. Run on Fly.io (see README.md here), or on any machine friends
can reach.

    python3 server.py [--port 27630] [--bind 0.0.0.0] [--sim-delay-ms 0] [--sim-loss 0]

It carries both of the runtime's ways to play together, told apart by their first bytes:

System Link ('X' 'L' 1 ...): the server is the network switch that a hosting copy is
    otherwise (src/kernel/xbox_net_udp.c, host mode), with the same messages, for any
    number of sessions at once, each named by its join code. Every copy joins it
    (syslink = join, or host: with online_server set both join); the game itself decides
    who hosts the match. Broadcasts go to everyone else in the session, a frame for one
    console to that console once the switch has seen where its Ethernet address is.

Online co-op ('X' 'R' ...  then 'X' 'L' 'S' ...): the server pairs a host and a joiner
    by their session code (src/kernel/recomp_lockstep_net.c, online_server set) and
    passes their own messages between them as they are. Each registers first, and is
    sent a number it must send back, so a forged registration cannot point the server's
    traffic at someone else's address.

    'X' 'R' 1 REGISTER  role (1 host, 2 join), nonce (8), code length, code
    'X' 'R' 1 CHALLENGE nonce (8)               register again with this nonce
    'X' 'R' 1 OK        role, the other player's there (1) or not (0)
    'X' 'R' 1 ERROR     why, as text

A session code is required in both (at least four characters): the server is open to
anyone who knows its address, and the code is what keeps strangers out of a session.
Standard library only.
"""
import argparse
import heapq
import os
import select
import socket
import sys
import time

DEFAULT_PORT = 27630

# System Link: xbox_net_udp.c's session format.
SL_VERSION = 1
SL_HELLO, SL_WELCOME, SL_REFUSE, SL_FRAME, SL_PING, SL_PONG, SL_BYE = range(1, 8)
REFUSE_CODE, REFUSE_FULL, REFUSE_VERSION, REFUSE_SAME_CONSOLE, REFUSE_NOT_JOINED = range(1, 6)
MAX_CONSOLES = 16                   # XBOX_NET_MAX_CONSOLES
FRAME_MIN, FRAME_MAX = 14, 1514     # an Ethernet frame without its checksum

# Co-op: the relay's own envelope.
R_VERSION = 1
R_REGISTER, R_CHALLENGE, R_OK, R_ERROR = range(1, 5)
ROLE_HOST, ROLE_JOIN = 1, 2

SILENT_S = 30.0                     # no word for this long: gone
MAX_SESSIONS = 512
MAX_ENDPOINTS = 2048
MIN_CODE = 4
REPLACE_S = 5                       # a place quiet this long goes to whoever registers for it:
                                    # a live copy is heard at least every 2 s (it registers again)
LS_REFUSE, LS_BYE = 3, 5            # co-op's own messages ('X' 'L' 'S' version type) the server notes


def log(msg):
    print(time.strftime('%Y-%m-%d %H:%M:%S'), msg, flush=True)


def addr_text(a):
    return '%s:%d' % a


class Server:
    def __init__(self, sock, sim_delay_ms=0, sim_loss=0):
        self.sock = sock
        self.sim_delay = sim_delay_ms / 1000.0
        self.sim_loss = sim_loss
        self.queue = []                       # (due, seq, data, addr): the simulated internet
        self.seq = 0
        # System Link: code -> {addr: {'mac': bytes|None, 'seen': t}}; addr -> code
        self.links = {}
        self.link_of = {}
        # Co-op: code -> {'host': addr|None, 'join': addr|None}; addr -> (code, role, seen)
        self.pairs = {}
        self.pair_of = {}
        self.nonces = {}                      # addr -> nonce sent it
        self.stats = {'in': 0, 'out': 0, 'biggest': 0, 'over1200': 0, 'since': time.time()}
        self.report_every = 300

    # ---- sending, through the simulation when there is one
    def send(self, data, addr):
        self.stats['out'] += 1
        if self.sim_loss and int.from_bytes(os.urandom(2), 'little') % 100 < self.sim_loss:
            return
        if self.sim_delay:
            self.seq += 1
            heapq.heappush(self.queue, (time.monotonic() + self.sim_delay, self.seq, data, addr))
            return
        try:
            self.sock.sendto(data, addr)
        except OSError:
            pass

    def flush(self):
        now = time.monotonic()
        while self.queue and self.queue[0][0] <= now:
            _, _, data, addr = heapq.heappop(self.queue)
            try:
                self.sock.sendto(data, addr)
            except OSError:
                pass

    # ---- System Link
    def sl_send(self, addr, kind, body=b''):
        self.send(bytes((0x58, 0x4C, SL_VERSION, kind)) + body, addr)

    def sl_receive(self, b, addr, now):
        kind = b[3]
        session = self.link_of.get(addr)
        if kind == SL_HELLO:
            if b[2] != SL_VERSION:
                self.sl_send(addr, SL_REFUSE, bytes((REFUSE_VERSION,)))
                return
            if len(b) < 5:
                return
            cl = b[4]
            if cl > 63 or len(b) < 5 + cl + 6:
                return
            code = b[5:5 + cl].decode('utf-8', 'replace').lower()
            mac = bytes(b[5 + cl:5 + cl + 6])
            if len(code) < MIN_CODE:
                self.sl_send(addr, SL_REFUSE, bytes((REFUSE_CODE,)))
                return
            if session is not None and session != code:
                self.sl_leave(addr, session)
            members = self.links.get(code)
            if members is None:
                if len(self.links) + len(self.pairs) >= MAX_SESSIONS:
                    self.sl_send(addr, SL_REFUSE, bytes((REFUSE_FULL,)))
                    return
                members = self.links[code] = {}
            if addr not in members:
                # The same console back from a new address (its copy restarted) takes its
                # old place, as the hosting copy's switch does.
                for other, m in list(members.items()):
                    if m['mac'] == mac:
                        del members[other]
                        self.link_of.pop(other, None)
                if len(members) >= MAX_CONSOLES:
                    self.sl_send(addr, SL_REFUSE, bytes((REFUSE_FULL,)))
                    return
                members[addr] = {'mac': mac, 'seen': now}
                self.link_of[addr] = code
                log('System Link %s: a console joined from %s, %d in' %
                    (self.code_text(code), addr_text(addr), len(members)))
            members[addr]['seen'] = now
            self.sl_send(addr, SL_WELCOME, bytes((len(members),)))
            return
        if session is None:
            if kind in (SL_FRAME, SL_PING):
                self.sl_send(addr, SL_REFUSE, bytes((REFUSE_NOT_JOINED,)))
            return
        members = self.links[session]
        me = members[addr]
        me['seen'] = now
        if kind == SL_FRAME:
            frame = b[4:]
            if not FRAME_MIN <= len(frame) <= FRAME_MAX:
                return
            me['mac'] = bytes(frame[6:12])          # the switch learns where an address is
            dest = frame[0:6]
            others = [a for a in members if a != addr]
            if not dest[0] & 1:
                known = [a for a in others if members[a]['mac'] == bytes(dest)]
                if known:
                    others = known[:1]
            for a in others:
                self.send(b, a)                     # the same message: header and frame
        elif kind == SL_PING:
            self.sl_send(addr, SL_PONG, bytes(b[4:]))
        elif kind == SL_BYE:
            self.sl_leave(addr, session)

    def sl_leave(self, addr, code):
        members = self.links.get(code)
        if members and addr in members:
            del members[addr]
            self.link_of.pop(addr, None)
            log('System Link %s: the console at %s left, %d in' %
                (self.code_text(code), addr_text(addr), len(members)))
            if not members:
                del self.links[code]

    # ---- co-op
    def r_send(self, addr, kind, body=b''):
        self.send(bytes((0x58, 0x52, R_VERSION, kind)) + body, addr)

    def r_receive(self, b, addr, now):
        if b[2] != R_VERSION:
            self.r_send(addr, R_ERROR, b'the server speaks another version: update the '
                                       b'server or the game')
            return
        if b[3] != R_REGISTER or len(b) < 4 + 1 + 8 + 1:
            return
        role, nonce, cl = b[4], bytes(b[5:13]), b[13]
        if role not in (ROLE_HOST, ROLE_JOIN) or len(b) < 14 + cl or cl > 63:
            return
        code = b[14:14 + cl].decode('utf-8', 'replace').lower()
        if len(code) < MIN_CODE:
            self.r_send(addr, R_ERROR, b'a session code of at least four characters is needed')
            return
        # The address has to answer first: it is sent a number, and only a registration
        # that carries it back counts.
        if self.nonces.get(addr) != nonce:
            if addr not in self.nonces and len(self.nonces) >= MAX_ENDPOINTS:
                return
            self.nonces[addr] = os.urandom(8)
            self.r_send(addr, R_CHALLENGE, self.nonces[addr])
            return
        key = 'host' if role == ROLE_HOST else 'join'
        other = 'join' if role == ROLE_HOST else 'host'
        old = self.pair_of.get(addr)
        if old and (old[0] != code or old[1] != key):
            self.pair_leave(addr)
        pair = self.pairs.get(code)
        if pair is None:
            if role == ROLE_JOIN:
                self.r_send(addr, R_ERROR, b'no host is waiting with that session code on '
                                           b'this server yet')
                return
            if len(self.links) + len(self.pairs) >= MAX_SESSIONS:
                self.r_send(addr, R_ERROR, b'the server is full')
                return
            pair = self.pairs[code] = {'host': None, 'join': None}
        if pair[key] is not None and pair[key] != addr:
            seen = self.pair_of.get(pair[key], (None, None, 0))[2]
            if now - seen < REPLACE_S:
                self.r_send(addr, R_ERROR, b'a host is already waiting with that session code'
                            if role == ROLE_HOST else b'the session already has two players')
                return
            self.pair_of.pop(pair[key], None)
        if pair[key] != addr:
            log('co-op %s: the %s registered from %s' % (self.code_text(code), key, addr_text(addr)))
        pair[key] = addr
        self.pair_of[addr] = (code, key, now)
        self.r_send(addr, R_OK, bytes((role, 1 if pair[other] else 0)))

    def coop_forward(self, b, addr, now):
        mine = self.pair_of.get(addr)
        if not mine:
            return
        code, key, seen = mine
        if now - seen < SILENT_S:
            self.pair_of[addr] = (code, key, now)
        pair = self.pairs.get(code)
        to = pair and pair['join' if key == 'host' else 'host']
        if to:
            self.send(b, to)
        # A copy saying BYE is going; a joiner the host refused is going: their place is
        # free at once, for whoever registers next. The registration stays until the next
        # housekeeping (within a second), so the BYE a copy sends twice still reaches the
        # other side.
        if len(b) >= 5 and b[4] == LS_BYE:
            self.pair_stale(addr)
        elif len(b) >= 5 and b[4] == LS_REFUSE and key == 'host' and to:
            self.pair_stale(to)

    def pair_stale(self, addr):
        mine = self.pair_of.get(addr)
        if mine:
            self.pair_of[addr] = (mine[0], mine[1], time.monotonic() - SILENT_S - 1.0)

    def pair_leave(self, addr):
        mine = self.pair_of.pop(addr, None)
        if not mine:
            return
        code, key, _ = mine
        pair = self.pairs.get(code)
        if pair and pair[key] == addr:
            pair[key] = None
            log('co-op %s: the %s at %s left' % (self.code_text(code), key, addr_text(addr)))
            if not pair['host'] and not pair['join']:
                del self.pairs[code]

    # ---- the rest
    @staticmethod
    def code_text(code):
        return '"%s..."' % code[:2]          # never the whole code in a log

    def receive(self, b, addr):
        now = time.monotonic()
        self.stats['in'] += 1
        if len(b) > self.stats['biggest']:
            self.stats['biggest'] = len(b)
        if len(b) > 1200:
            self.stats['over1200'] += 1
        if len(b) < 4 or b[0] != 0x58:
            return
        if b[1] == 0x4C and b[2] == 0x53:        # 'X' 'L' 'S': co-op's own messages
            self.coop_forward(b, addr, now)
        elif b[1] == 0x4C:                       # 'X' 'L': System Link
            self.sl_receive(b, addr, now)
        elif b[1] == 0x52:                       # 'X' 'R': co-op's registration
            self.r_receive(b, addr, now)

    def housekeeping(self):
        now = time.monotonic()
        for code in list(self.links):
            for addr, m in list(self.links[code].items()):
                if now - m['seen'] > SILENT_S:
                    self.sl_leave(addr, code)
        for addr, (code, key, seen) in list(self.pair_of.items()):
            if now - seen > SILENT_S:
                self.pair_leave(addr)
        live = set(self.pair_of)
        for addr in list(self.nonces):
            if addr not in live and len(self.nonces) > MAX_ENDPOINTS // 2:
                del self.nonces[addr]
        if time.time() - self.stats['since'] >= self.report_every:
            log('%d sessions; %d messages in, %d out; the largest %d bytes, %d over 1200' %
                (len(self.links) + len(self.pairs), self.stats['in'], self.stats['out'],
                 self.stats['biggest'], self.stats['over1200']))
            self.stats.update({'in': 0, 'out': 0, 'since': time.time()})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=int(os.environ.get('PORT', DEFAULT_PORT)))
    ap.add_argument('--bind', default=os.environ.get('BIND', '0.0.0.0'),
                    help='the address to receive on (on Fly.io: fly-global-services)')
    ap.add_argument('--sim-delay-ms', type=int, default=0, help='testing: hold every message back')
    ap.add_argument('--sim-loss', type=int, default=0, help='testing: drop this percentage')
    ap.add_argument('--report-every', type=int, default=300, help='seconds between traffic reports')
    a = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, 'SIO_UDP_CONNRESET'):
        sock.ioctl(socket.SIO_UDP_CONNRESET, False)    # Windows: a closed port is not an error here
    sock.bind((a.bind, a.port))
    log('the online server, on UDP %s:%d%s' % (a.bind, a.port,
        ' (simulating %d ms and %d%% loss)' % (a.sim_delay_ms, a.sim_loss)
        if a.sim_delay_ms or a.sim_loss else ''))
    s = Server(sock, a.sim_delay_ms, a.sim_loss)
    s.report_every = max(5, a.report_every)
    last_house = time.monotonic()
    while True:
        timeout = 1.0
        if s.queue:
            timeout = max(0.0, min(timeout, s.queue[0][0] - time.monotonic()))
        r, _, _ = select.select([sock], [], [], timeout)
        if r:
            for _ in range(256):                  # what has arrived, before looking up again
                try:
                    b, addr = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                except OSError:
                    break
                s.receive(b, addr)
                sock.setblocking(False)
            sock.setblocking(True)
        s.flush()
        if time.monotonic() - last_house >= 1.0:
            s.housekeeping()
            last_house = time.monotonic()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
