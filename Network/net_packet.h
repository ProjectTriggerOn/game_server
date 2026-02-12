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
};
