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
- **Up to 4 players** concurrent
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
./game_server --port=7777
```

## Docker

```bash
# Build
docker build -t triggeron-server .

# Run
docker run -p 7777:7777/udp triggeron-server
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

## Server Parameters

### General

| Parameter | Value |
|-----------|-------|
| Tick rate | 32 Hz (31.25 ms/tick) |
| Max players | 4 |

## Architecture

The server follows a **server-authoritative** model:

- **Client → Server**: `InputCmd` (24 bytes) — player intent only (movement axes, yaw/pitch, button bitfield)
- **Server → Client**: `Snapshot` — authoritative world state (positions, velocities, health, flags)

The server never trusts client positions. All movement, collision, and combat are simulated server-side from input commands.

Key components:
- `ENetServerNetwork` — UDP peer management, packet routing
- `GameServer` — Per-player state (`unordered_map<uint8_t, PlayerData>`), physics tick, combat resolution
- `server_collision.h` — Header-only pure-math collision library (no DirectXMath)
- `server_raycast.h` — Weapon hit detection via raycasting

## Project Structure

```
main.cpp                        Entry point, signal handling, main loop
Network/
├── game_server.h/cpp           Core game logic, physics, combat
├── enet_server_network.h/cpp   ENet UDP server implementation
├── net_common.h                Protocol structs (shared with client)
├── net_packet.h                Packet type enum (shared with client)
├── i_network.h                 Abstract network interface (shared with client)
├── map_colliders.h             Map geometry (shared with client)
├── server_collision.h          Collision math (server-only)
└── server_raycast.h            Raycast hit detection (server-only)
ThirdParty/
├── enet/                       ENet networking library
└── toml++/                     TOML config parser (header-only)
```

