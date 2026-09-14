#pragma once
//=============================================================================
// net_packet.h
//
// Packet type identifiers for network serialization.
// IMPORTANT: This file must be kept in sync with game_client/Network/net_packet.h
//=============================================================================

#include <cstdint>

enum class PacketType : uint8_t
{
    INPUT_CMD    = 1,   // Client -> Server (contains InputCmd)
    SNAPSHOT     = 2,   // Server -> Client (contains Snapshot)
    MAP_INFO     = 3,   // Server -> Client (contains MapInfo, sent once on connect)
    JOIN_REQUEST = 4,   // Client -> Server (no payload, see below)
};

// JOIN_REQUEST is the line between a transport connection and a match
// participant. An ENet peer is opened once when the client process starts and
// lives until it exits; that says nothing about whether anyone is playing. The
// server therefore treats a fresh peer as a spectator - no spawn, no snapshots,
// no weight against MatchConfig::MIN_PLAYERS - until this one byte arrives.
// Without it, two clients sitting on the title screen were enough to start a
// match and run it to its 60-second end with nobody in the world.
//
// Payloadless on purpose: the server already knows who the sender is (its peer)
// and there is nothing yet for a client to ask for. It is sent RELIABLE - a
// dropped join would strand the player in a room they believe they are in.

// Sent once per connect so the client can verify it loaded the same map the
// server simulates (collision-section FNV-1a checksum). Pure POD, size-guarded.
struct MapInfo
{
    char     name[64];
    uint32_t checksum;
};
static_assert(sizeof(MapInfo) == 68, "MapInfo layout");
