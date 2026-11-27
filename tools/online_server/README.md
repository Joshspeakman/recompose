# The online server

A server every player's copy connects to, for System Link and online co-op, so nobody
opens a port on their router. One person runs it; everyone types its address and the
same session code in the launcher (Play Together > Connect: Through an online server).

- **System Link**: the server is the network switch a hosting copy otherwise is. Every
  copy joins it; the game decides who hosts the match. Up to 16 consoles a session.
- **Online co-op**: the server pairs the host and the joiner by their session code and
  passes their messages between them.

The session code names the session and keeps strangers out of it: at least four
characters, the same for everyone in the game. The server holds no accounts and keeps
nothing on disk; a session ends when its players leave or go quiet for 30 seconds.

## On Fly.io

Fly.io runs it on one small machine. At Fly's prices (fly.io/docs/about/pricing) that is
about $4.20 a month: a shared-cpu-1x machine with 256 MB, $2.19, and the dedicated IPv4
address UDP needs, $2. Fly's free plan gives $5 of use a month once a card is added.

1. Make an account at fly.io.
2. Install flyctl, Fly's command line (fly.io/docs/flyctl/install):

   | | |
   | --- | --- |
   | Windows | `pwsh -Command "iwr https://fly.io/install.ps1 -useb \| iex"` |
   | macOS | `brew install flyctl`, or `curl -L https://fly.io/install.sh \| sh` |
   | Linux | `curl -L https://fly.io/install.sh \| sh` |

3. In this folder, run the setup:

   | | |
   | --- | --- |
   | Windows | `powershell -ExecutionPolicy Bypass -File deploy.ps1` |
   | macOS, Linux | `sh deploy.sh` |

   It signs in to Fly (a browser opens), asks a name for the server (it becomes
   `name.fly.dev`) and the region nearest the players, makes the app, gives it a
   dedicated IPv4 address, writes `fly.toml` and deploys. At the end it prints the
   address to type in the launcher. Run it again after an update; nothing is made twice.

4. In the launcher, everyone sets Connect to Through an online server, the Server address
   to `name.fly.dev`, and the same Session code.

To remove the server and its cost: `flyctl apps destroy <name>`.

### By hand

The setup runs these, with your name and region:

```
flyctl auth login
flyctl apps create --name <name> --org personal
(fly.toml from fly.toml.template, with the name and region in it)
flyctl ips allocate-v4 -a <name>
flyctl deploy -a <name> -c fly.toml --ha=false --remote-only
```

What Fly needs for UDP, which the template and `Dockerfile` already do: a dedicated IPv4
address (UDP does not reach the shared one), the server receiving on the address
`fly-global-services`, the same port outside and inside (27630), and one machine
(`--ha=false`): the sessions live in its memory.

## Anywhere else

Any machine your friends can reach on UDP port 27630 works, with Python 3:

```
python3 server.py [--port 27630] [--bind 0.0.0.0]
```

`--sim-delay-ms` and `--sim-loss` hold back and drop messages, to try a game at internet
distances on one PC. `--report-every` sets how often it logs its traffic (300 seconds).

## The messages

System Link uses the runtime's own session format (`src/kernel/xbox_net_udp.c`): the
server answers it as a hosting copy's switch does. Co-op registers first:

```
'X' 'R' 1 REGISTER   role (1 host, 2 join), nonce (8), code length, code
'X' 'R' 1 CHALLENGE  nonce (8): register again with it
'X' 'R' 1 OK         role, the other player there (1) or not (0)
'X' 'R' 1 ERROR      why, as text
```

then sends its own messages (`src/kernel/recomp_lockstep_net.c`) to the server, which
passes them to the other player. A registration only counts once its address has sent
back the number the server gave it, so a forged one cannot point the server's traffic at
someone else. The largest message a TimeSplitters 2 System Link match sent through it
was 310 bytes; co-op's are under 1.1 KB.
