#pragma once
//=============================================================================
// net_common.h
//
// Network data structures for Server-Authoritative architecture.
// POD (Plain Old Data) structures for network communication.
//
// IMPORTANT: This file must be kept in sync with
// game_client/Network/net_common.h
//
// Data Flow:
//   Client -> Server: InputCmd (input intent only, NO position/velocity)
//   Server -> Client: Snapshot (authoritative state)
//=============================================================================

#include <cstdint>

#ifdef _WIN32
#include <DirectXMath.h>
using Float3 = DirectX::XMFLOAT3;
#else
struct Float3 {
    float x, y, z;
};
#endif


//-----------------------------------------------------------------------------
// Button Flags (bitfield for InputCmd.buttons)
//-----------------------------------------------------------------------------
namespace InputButtons {
constexpr uint32_t NONE = 0;
constexpr uint32_t JUMP = 1 << 0;
constexpr uint32_t FIRE = 1 << 1;
constexpr uint32_t ADS = 1 << 2; // Aim Down Sights
constexpr uint32_t RELOAD = 1 << 3;
constexpr uint32_t INSPECT = 1 << 4;
constexpr uint32_t SPRINT = 1 << 5;
} // namespace InputButtons

//-----------------------------------------------------------------------------
// InputCmd - Client to Server (Upstream)
//
// Contains ONLY player intent. Never includes position/velocity.
// Server will simulate movement based on these inputs.
//-----------------------------------------------------------------------------
struct InputCmd {
  uint32_t tickId;  // Target server tick for this command
  float moveAxisX;  // Horizontal movement: -1.0 (A) to 1.0 (D)
  float moveAxisY;  // Forward movement: -1.0 (S) to 1.0 (W)
  float yaw;        // Camera horizontal angle (radians)
  float pitch;      // Camera vertical angle (radians)
  uint32_t buttons; // Bitfield of InputButtons
};

//-----------------------------------------------------------------------------
// NetStateFlags (bitfield for NetPlayerState.stateFlags)
//-----------------------------------------------------------------------------
namespace NetStateFlags {
constexpr uint32_t NONE = 0;
constexpr uint32_t IS_JUMPING = 1 << 0;
constexpr uint32_t IS_GROUNDED = 1 << 1;
constexpr uint32_t IS_FIRING = 1 << 2;
constexpr uint32_t IS_ADS = 1 << 3;
constexpr uint32_t IS_RELOADING = 1 << 4;
constexpr uint32_t IS_RELOADING_EMPTY = 1 << 5;
} // namespace NetStateFlags

//-----------------------------------------------------------------------------
// NetPlayerState - Authoritative player state computed by server
//-----------------------------------------------------------------------------
struct NetPlayerState {
  uint32_t tickId;            // Server tick when this state was computed
  Float3 position; // World position (server authoritative)
  Float3 velocity; // Current velocity
  float yaw;                  // Camera yaw
  float pitch;                // Camera pitch
  uint32_t stateFlags;        // Bitfield of StateFlags
};

//-----------------------------------------------------------------------------
// Snapshot - Server to Client (Downstream)
//
// Contains all authoritative state the client needs.
// Client uses this to correct prediction errors.
//-----------------------------------------------------------------------------
struct Snapshot {
  uint32_t tickId;            // Server tick this snapshot represents
  double serverTime;          // Server time for interpolation
  NetPlayerState localPlayer; // Local player's authoritative state
};

//-----------------------------------------------------------------------------
// Size guards for network serialization (memcpy)
// If these fire, struct layout changed and both client/server must be updated.
//-----------------------------------------------------------------------------
static_assert(sizeof(InputCmd) == 24,
              "InputCmd size changed - update network serialization");
static_assert(sizeof(NetPlayerState) == 40,
              "NetPlayerState size changed - update network serialization");
