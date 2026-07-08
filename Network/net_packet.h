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
    INPUT_CMD = 1,   // Client -> Server (contains InputCmd)
    SNAPSHOT  = 2,   // Server -> Client (contains Snapshot)
    MAP_INFO  = 3,   // Server -> Client (contains MapInfo, sent once on connect)
};

// Sent once per connect so the client can verify it loaded the same map the
// server simulates (collision-section FNV-1a checksum). Pure POD, size-guarded.
struct MapInfo
{
    char     name[64];
    uint32_t checksum;
};
static_assert(sizeof(MapInfo) == 68, "MapInfo layout");
