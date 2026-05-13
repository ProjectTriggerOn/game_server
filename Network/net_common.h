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
constexpr uint32_t IS_RELOAD_EMPTY = 1 << 5;
constexpr uint32_t IS_DEAD = 1 << 6;
constexpr uint32_t IS_INSPECTING = 1 << 7;
} // namespace NetStateFlags

//-----------------------------------------------------------------------------
// NetPlayerState - Authoritative player state computed by server
//-----------------------------------------------------------------------------
struct NetPlayerState {
  uint32_t tickId;                 // Server tick when this state was computed
  uint32_t lastProcessedInputTick; // Last cmd.tickId server processed for this player
                                   //   (0 = none yet) — client uses this to look up
                                   //   the input-history entry that produced this state.
  Float3 position;                 // World position (server authoritative)
  Float3 velocity;                 // Current velocity
  float yaw;                       // Camera yaw
  float pitch;                     // Camera pitch
  uint32_t stateFlags;             // Bitfield of StateFlags
  uint8_t  health;                 // 0-200, server authoritative
  uint8_t  hitByPlayerId;          // 0xFF = no hit, else attacker ID
  uint16_t fireCounter;            // Server-tracked fire count
  uint8_t  ammo;                   // Magazine ammo (0-30)
  uint8_t  ammoReserve;            // Reserve ammo (0-90)
  uint8_t  pad[2];                 // pad to 4-byte alignment
};

//-----------------------------------------------------------------------------
// Multi-player constants
//-----------------------------------------------------------------------------
static constexpr uint8_t MAX_PLAYERS = 4;

//-----------------------------------------------------------------------------
// Weapon / Ammo constants (shared between client and server)
//-----------------------------------------------------------------------------
namespace WeaponConfig {
constexpr uint8_t MAG_SIZE = 30;
constexpr uint8_t MAX_RESERVE = 90;
// Reload duration = FBX animation length × 0.9 (visual completion point)
// Measured from red_arm003: reload_ammo_left=2.133s, reload_out_of_ammo=3.000s
constexpr double RELOAD_DURATION = 1.9197;             // index 8 (reload_ammo_left)
constexpr double RELOAD_OUT_OF_AMMO_DURATION = 2.7000; // index 9 (reload_out_of_ammo)
} // namespace WeaponConfig

//-----------------------------------------------------------------------------
// Team IDs
//-----------------------------------------------------------------------------
namespace PlayerTeam {
constexpr uint8_t RED  = 0;
constexpr uint8_t BLUE = 1;
} // namespace PlayerTeam

//-----------------------------------------------------------------------------
// Physics constants (must match exactly between client prediction and server)
//-----------------------------------------------------------------------------
namespace PhysicsConfig {
// Air strafe speed allowed slightly above ground max to enable bunny-hop momentum.
constexpr float AIR_STRAFE_SPEED_MULT = 1.2f;
// Minimum upward component of collision normal (ny) for a surface to count as ground.
constexpr float GROUND_NORMAL_THRESHOLD = 0.7f;
// Generic small epsilon used by collision/raycast math.
constexpr float EPSILON = 1e-8f;
} // namespace PhysicsConfig

//-----------------------------------------------------------------------------
// RemotePlayerEntry - Identifies a remote player's state in a Snapshot
//-----------------------------------------------------------------------------
struct RemotePlayerEntry {
  uint8_t playerId;
  uint8_t teamId;              // PlayerTeam::RED or BLUE
  uint8_t padding[2];          // align to 4 bytes
  NetPlayerState state;
};

//-----------------------------------------------------------------------------
// Snapshot - Server to Client (Downstream)
//
// Contains all authoritative state the client needs.
//   localPlayer    — your own state (for client-side prediction correction)
//   remotePlayers  — other connected players' states (for RemotePlayer rendering)
//-----------------------------------------------------------------------------
struct Snapshot {
  uint32_t tickId;                                  // Server tick this snapshot represents
  double serverTime;                                // Server time
  NetPlayerState localPlayer;                       // Your authoritative state
  uint8_t localPlayerId;                            // Your player ID
  uint8_t remotePlayerCount;                        // Number of valid entries in remotePlayers[]
  uint8_t localPlayerTeam;                          // Your team (PlayerTeam::RED or BLUE)
  uint8_t padding_snap[1];                          // Alignment
  RemotePlayerEntry remotePlayers[MAX_PLAYERS - 1]; // Other players' states
};

//-----------------------------------------------------------------------------
// Size guards for network serialization (memcpy)
// If these fire, struct layout changed and both client/server must be updated.
//-----------------------------------------------------------------------------
static_assert(sizeof(InputCmd) == 24,
              "InputCmd size changed - update network serialization");
static_assert(sizeof(NetPlayerState) == 52,
              "NetPlayerState size changed - update network serialization");
static_assert(sizeof(RemotePlayerEntry) == 56,
              "RemotePlayerEntry size changed - update network serialization");
