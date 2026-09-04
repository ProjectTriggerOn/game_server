<p align="center">
  English | <a href="./README_JP.md">日本語</a>
</p>

# TriggerOn Server

Authoritative game server for TriggerOn — a multiplayer networked FPS. Runs on Linux, deployed via Docker.

## Features

- **Server-authoritative** physics, movement, and combat simulation
- **32 Hz fixed tick rate** with accumulator-based timestep
- **UDP networking** via ENet
- **Team-based combat** — RED vs BLUE with asymmetric weapon stats
- **Up to 10 players** concurrent (5v5: RED 5 + BLUE 5)
- **Lag compensation** — 64 ticks (2 s) of per-player position history; a hit ray is resolved against the world as the shooter saw it (`InputCmd.viewTick` + sub-tick fraction)
- **COD-model recoil** — visual punch, real kick, and bloom (spread growth) are advanced server-side and ride the snapshot. The pattern is derived from `fireCounter` alone (no RNG), so client and server independently compute the same trajectory; `recoil_math.h` is mirrored in the client and must stay in sync.
- **Match flow** — team kill scoring (first to 10), a 60 s time limit, an 8-entry kill-feed ring carried in every snapshot, and an end-of-match state with the winning team
- **Map loading** — reads the `.map` binary format shared with the client (`--map=`, falling back to the compiled-in geometry), and sends `MAP_INFO` on connect so a client that loaded a different map is caught by collision checksum
- **Inbound flood mitigation** — per-peer token bucket, an events-per-poll cap, and a host packet-size cap, with per-second receive / queue / tick-time reporting (thresholds in `Network/net_limits.h`)
- **Docker-ready** with multi-stage build

## Requirements

**Native build:**
- Linux (Debian/Ubuntu recommended)
- g++ with C++17 support
- make

**Docker build:**
- Docker

## Quick Start

```bash
make
./game_server --port=7777 --map=default.map
```

## Docker

```bash
# Build
docker build -t triggeron-server .

# Run
docker run -p 7777:7777/udp triggeron-server
```

The image ships `default.map` at `/app/default.map` and runs with `/app` as its working directory, so the default `--map=default.map` resolves. To simulate a different map, mount it over that path:

```bash
docker run -p 7777:7777/udp -v ./my.map:/app/default.map triggeron-server
```

### Docker Compose

To pull and run the published image from Docker Hub:

```yaml
# docker-compose.yml
services:
  game-server:
    image: pisto3/triggeron_game_server:latest
    ports:
      - "7777:7777/udp"
    restart: unless-stopped
```

```bash
docker compose up -d
```

## CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--port=XXXX` | 7777 | UDP port to listen on |
| `--map=PATH` | `default.map` | `.map` file to simulate, resolved relative to the working directory. If it cannot be read, the server falls back to the compiled-in map (`map_colliders.h`) — whose checksum will not match any client, so the map check will warn. |

## Server Parameters

### General

| Parameter | Value |
|-----------|-------|
| Tick rate | 32 Hz (31.25 ms/tick) |
| Max players | 10 (RED 5 + BLUE 5) |
| Position history (lag compensation) | 64 ticks (2 s) |

### Match

| Parameter | Value |
|-----------|-------|
| Score limit | 10 kills |
| Match duration | 60 s |
| Kill-feed ring | 8 entries per snapshot |

### Inbound limits (`Network/net_limits.h`)

| Parameter | Value |
|-----------|-------|
| Per-peer input rate | 32 packets/tick refill (1024/s), bucket depth 64 |
| Events per `PollEvents` | 4096 |
| Max packet size (host cap) | 1024 bytes — below the 1392-byte MTU, so nothing legitimate fragments |

## Architecture

The server follows a **server-authoritative** model:

- **Client → Server**: `InputCmd` (32 bytes) — player intent only (movement axes, yaw/pitch, button bitfield, viewed tick for lag compensation)
- **Server → Client**: `Snapshot` (784 bytes at 10 players) — authoritative world state (positions, velocities, health, flags, recoil, ammo, score, kill feed)
- **Server → Client**: `MapInfo` (68 bytes) — map name + collision checksum, sent once per connect

The server never trusts client positions. All movement, collision, and combat are simulated server-side from input commands.

Key components:
- `ENetServerNetwork` — UDP peer management, packet routing, per-peer rate limiting and receive statistics
- `GameServer` — Per-player state (`unordered_map<uint8_t, PlayerData>`), physics tick, combat resolution, lag compensation, match/score state
- `server_collision.h` — Header-only pure-math collision library (no DirectXMath)
- `server_raycast.h` — Weapon hit detection via raycasting
- `recoil_math.h` — Pure recoil math, mirrored byte-for-byte in the client

## Load Testing

`tools/flood_client.cpp` is a headless ENet client used to measure the server under a single-client flood: `--rate N` plays a normal-rate victim and reports the snapshot interval it observes, `--flood` sends input as fast as possible, `--junk` sends malformed packets. It is a test tool, not shipped code:

```bash
g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/flood_client.cpp \
    -o floodtest -LThirdParty/enet/lib -lenet -lpthread
./floodtest --rate 60 --secs 30 --label victim
./floodtest --flood --secs 30 --label attacker
```

## Project Structure

```
main.cpp                        Entry point, signal handling, main loop, status line
server_log.h                    Timestamped stdout logger
default.map                     Map shipped beside the binary (Docker: /app/default.map)
Network/
├── game_server.h/cpp           Core game logic, physics, combat, lag compensation, match state
├── enet_server_network.h/cpp   ENet UDP server implementation, inbound rate limiting
├── net_common.h                Protocol structs (shared with client)
├── net_packet.h                Packet type enum + MapInfo (shared with client)
├── net_limits.h                Flood-mitigation thresholds (server-only)
├── i_network.h                 Abstract network interface (shared with client)
├── map_io.h                    .map binary format (shared with client)
├── map_colliders.h             Compiled-in fallback map geometry (shared with client)
├── recoil_math.h               Recoil math (mirrored in the client)
├── server_collision.h          Collision math (server-only)
└── server_raycast.h            Raycast hit detection (server-only)
tools/
└── flood_client.cpp            Headless load/flood test client (not shipped)
ThirdParty/
└── enet/                       ENet networking library (built from source)
.github/workflows/              Docker image build & publish (main, dev)
```
