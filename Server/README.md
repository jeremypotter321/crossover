# Crossover dedicated server

A headless server for the Crossover multiplayer mod, so a session no longer needs one
player to host, port-forward, and stay online for everyone else.

**The mod does not need to change.** The server takes over the exact role the hosting
player's game already performs — handing out network ids, placing joiners, syncing the
roster, relaying movement — while owning no avatar itself. Players just press
<kbd>NUMPAD 2</kbd> and connect to the server instead of to each other.

---

## Why there is a tunnel

Railway has no inbound UDP. It offers an HTTP router and a raw **TCP** proxy, and that is
all. SLikeNet's `RakPeerInterface` — the transport the mod is built on — is UDP-only, and
its `TCPInterface`/`PacketizedTCP` classes are a separate, non-interoperable stack. So a
SLikeNet server cannot be reached on Railway directly.

The server therefore ships its own bridge. Whole UDP datagrams are carried over one TCP
connection per player, and unwrapped back into UDP inside the container:

```
Fable.exe + EgoMP.dll
        │ UDP 127.0.0.1:60000
        ▼
crossover-tunnel  (player's machine)
        │ TCP
        ▼
Railway TCP Proxy ──▶ TunnelServer ──▶ one loopback UDP socket per player
                                                    │
                                                    ▼
                                            RelayServer (SLikeNet)
```

Every player reaches SLikeNet as `127.0.0.1` on a distinct ephemeral port. That is safe:
RakNet keys peers on the full IP:port tuple, and its same-IP connection-frequency limiter
is off by default (`RakPeer.cpp:312`).

The cost is honest: TCP means head-of-line blocking, so a lost packet briefly stalls the
movement stream behind it instead of being skipped. At Fable's pace and a handful of
players this is not noticeable, but if you want the best possible sync, host on something
with real UDP (see [Hosting without Railway](#hosting-without-railway)) — the same binary
covers both, and players then need no tunnel at all.

---

## Deploying to Railway

1. **New service** in a Railway project, pointed at this repository.
2. **Settings → Root Directory: `Server`.** Without this the Docker build has the wrong
   context and `COPY CMakeLists.txt` fails.
3. **Settings → Networking → TCP Proxy.** Set the application port to **60001**. Railway
   returns a public host and port, e.g. `shuttle.proxy.rlwy.net:15140` — that pair is what
   you give to players.
4. **Settings → Replicas: 1.** The session lives in the server's memory, and Railway's TCP
   proxy load-balances across replicas — two replicas means two disjoint worlds.
5. Leave **App Sleeping off**, or the server will nap between sessions.
6. *(Optional)* Attach a **volume at `/data`** so the rendezvous point survives redeploys.
7. *(Optional)* Add an HTTP domain to reach the status page; it listens on `PORT`.

Deploying with the CLI instead:

```sh
cd Server
railway link          # pick or create the project/service
railway up            # builds the Dockerfile and deploys
```

### Environment variables

| Variable | Default | What it does |
| --- | --- | --- |
| `CROSSOVER_TUNNEL_PORT` | `RAILWAY_TCP_APPLICATION_PORT`, else `60001` | TCP port the tunnel listens on — the one Railway proxies |
| `CROSSOVER_STATUS_PORT` | `PORT`, else `8080` | HTTP status + `/health` |
| `CROSSOVER_GAME_PORT` | `60000` | Internal UDP port for the relay. Only change this for direct-UDP hosting |
| `CROSSOVER_TUNNEL` | `true` | `false` disables the bridge and exposes the relay as plain UDP |
| `CROSSOVER_MAX_PLAYERS` | `8` | Player slots |
| `CROSSOVER_SPAWN_X/Y/Z` | `0` | Where the **first** player into an empty server is placed — see below |
| `CROSSOVER_SPAWN_MODE` | `anchor` | `anchor` puts joiners on the player already in-world; `fixed` always uses the spawn above |
| `CROSSOVER_STATE_FILE` | `/data/crossover-spawn.txt` | Remembered rendezvous point. Harmless if no volume is mounted |
| `CROSSOVER_TICK_HZ` | `60` | Relay poll rate |
| `CROSSOVER_VERBOSE` | `false` | Log unrecognised message ids |

### One thing you have to set by hand: the spawn point

When a player joins, the mod teleports them to a position the host chooses — normally the
host player's own location. A dedicated server has no avatar, so it needs a world
coordinate to send, and there is no way to ask the client for one first: the client does
not report its position until *after* it has been assigned an id and moved.

So:

* Joiners after the first land on whoever is already in-world. That needs no configuration
  and matches how the mod behaves today.
* The **first** player into an empty server goes to `CROSSOVER_SPAWN_X/Y/Z`, which
  defaults to `(0, 0, 0)` — almost certainly not somewhere you want to be.

To fix it once: join, walk to a sensible meeting point, and read the position out of the
server log or `GET /` (`rendezvous`). Put that into `CROSSOVER_SPAWN_X/Y/Z`. If a volume is
mounted at `/data` the server remembers it on its own and a redeploy keeps the spot.

---

## For players

Download `crossover-tunnel` for your platform (or build it — see below), then:

```sh
crossover-tunnel <server-host> <server-port>
# e.g. crossover-tunnel shuttle.proxy.rlwy.net 15140
```

Leave that window open. In game — **in the world, not at the main menu** — press
<kbd>NUMPAD 2</kbd> and enter:

```
IP:   127.0.0.1
Port: 60000
```

The mod's prompts appear on the terminal that launched `EgoMP.exe`, not in a window of
their own. Nothing else changes; <kbd>NUMPAD 3</kbd> still disconnects.

Do **not** press <kbd>NUMPAD 1</kbd> — that starts a local listen-server, which is the
thing the dedicated server replaces.

---

## Status endpoint

```
GET /health   ->  ok
GET /         ->  {"status":"running","players":2,"maxPlayers":8,
                   "transport":"tcp-tunnel","tunnelConnections":2,
                   "rendezvous":{...},"roster":[...]}
```

---

## Hosting without Railway

On anything with real UDP ingress (a VPS, Fly.io, a machine at home), skip the bridge and
let the mod talk to SLikeNet directly — better sync, and players need no tunnel:

```sh
docker run -d -p 60000:60000/udp -e CROSSOVER_TUNNEL=false \
  -e CROSSOVER_SPAWN_X=... -e CROSSOVER_SPAWN_Y=... -e CROSSOVER_SPAWN_Z=... \
  crossover-server
```

Players then connect straight to the server's address on port 60000.

---

## Building and testing

```sh
# Server image (linux/amd64 — the deployment target)
docker build --platform linux/amd64 -t crossover-server Server/

# Player-side tunnel: native binary + a static 32-bit Windows exe
Server/tunnel-client/build.sh          # -> Server/dist/

# Full integration test: 33 checks over both transports
docker run --rm --platform linux/amd64 --entrypoint bash \
  -v "$PWD/Server/test:/test:ro" crossover-server /test/run-integration-test.sh
```

`test/TestClient.cpp` is a headless stand-in for Fable + `EgoMP.dll`. It reproduces the
client half of `NetMainGameComponent` and `NetPlayerManager` exactly, which is what makes
it possible to verify the server on a machine that cannot build the mod.

### SLikeNet is pinned, deliberately

The build pins **`v.0.1.3`** (`RAKNET_VERSION 4.082`, `RAKNET_PROTOCOL_VERSION 6`) — the
version the mod's prebuilt `SLikeNet_LibStatic_Release_Win32.lib` was built from. Any other
protocol version and every client bounces with `ID_INCOMPATIBLE_PROTOCOL_VERSION`. Note that
upstream `main` is *ahead* of that tag and renames the CMake target, so do not assume HEAD
and the tag are interchangeable.

### The encoding trap

SLikeNet ships with `__BITSTREAM_NATIVE_END` undefined, so `BitStream::Write<T>` reverses
every value wider than a byte — the wire is **big-endian**. Worse, it reverses
`sizeof(T)` bytes *as one block*, so a 12-byte vector comes out as **BE(z), BE(y), BE(x)**:
the field order flips too. Writing three separate floats would compile, run, and put every
player in the wrong place. `RelayServer::ProtocolSelfTest()` asserts this at startup and
refuses to run if it ever stops holding. See `include/Protocol.h`.

---

## Known limitations

* **No authentication.** The mod's client calls `peer->Connect(ip, port, nullptr, 0)` with
  no password, so the server cannot require one without a mod change. Anyone with the
  address can join. Treat the TCP-proxy host:port as a secret, or add
  `SetIncomingPassword` on both sides later.
* **Positions only.** The server relays what the mod sends: spawn, movement, rotation. It
  does not sync combat, inventory, quests, or world state, because the mod does not send
  them.
* **Trusted clients.** Network ids are server-assigned and a client cannot impersonate
  another, but a client's reported position is taken at face value — as it is in the mod.
* **One session.** A single world per deployment; there is no lobby or room concept.
* **The dev test client does not run on Apple Silicon.** SLikeNet 0.1.3 predates arm64
  macOS and traps at startup there. Run the integration test in Docker, as above.
