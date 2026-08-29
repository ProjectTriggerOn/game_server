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
  // Lag compensation: the (fractional) server tick the client was VIEWING when
  // this command was generated (remote players render interp-delayed, so the
  // shooter aims at the past). Server rewinds hitboxes to this time on fire.
  uint32_t viewTick;   // Server tick being viewed; 0 = no data, do NOT rewind
  float viewTickFrac;  // [0,1) sub-tick interpolation fraction toward viewTick+1
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
  uint16_t kills;                  // Score: kills by this player
  uint16_t deaths;                 // Score: times this player died
  uint8_t  pad[2];                 // pad to 4-byte alignment (legacy 56-byte tail)
  // ---- Recoil (COD model, spec §1.1) — appended after the legacy tail so
  // every pre-existing wire offset is untouched (fireCounter stays @46,
  // deaths @52). Visual punch decays to zero; shotKick is the tiny real
  // trajectory component. Server-side integrated, broadcast here; client
  // predicts locally and reconciles against these values.
  float punchPitch;                // visual punch (rad, decays to 0)
  float punchYaw;                  // visual punch yaw (rad, decays to 0)
  float shotKickPitch;             // accumulated real kick (rad, tiny)
};

//-----------------------------------------------------------------------------
// Multi-player constants
//-----------------------------------------------------------------------------
static constexpr uint8_t MAX_PLAYERS = 10; // RED 5 + BLUE 5

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
// Match / scoring constants (shared between client and server)
//-----------------------------------------------------------------------------
namespace MatchConfig {
constexpr uint16_t SCORE_LIMIT    = 10;      // first team to this many kills wins
constexpr float    MATCH_DURATION = 60.0f;  // seconds (5:00) time limit
} // namespace MatchConfig

// Snapshot.matchState values. PLAYING == 0 so a zero-initialized snapshot reads
// as PLAYING (safe default).
namespace MatchState {
constexpr uint8_t PLAYING = 0;
constexpr uint8_t ENDED   = 1;
} // namespace MatchState

// Snapshot.winningTeam values beyond PlayerTeam::RED/BLUE.
namespace MatchTeam {
constexpr uint8_t DRAW = 2;
constexpr uint8_t NONE = 0xFF; // undecided (while PLAYING)
} // namespace MatchTeam

// Kill-event ring length carried in every snapshot. Over UNRELIABLE UDP a
// level-triggered counter can't convey discrete events, so the client replays
// (lastShownSeq, latestKillSeq] and dedups; entries older than this drop out of
// the ring (acceptable — already scrolled off the HUD feed).
static constexpr uint8_t KILL_FEED_SIZE = 8;

//-----------------------------------------------------------------------------
// Team IDs
//-----------------------------------------------------------------------------
namespace PlayerTeam {
constexpr uint8_t RED  = 0;
constexpr uint8_t BLUE = 1;
} // namespace PlayerTeam

//-----------------------------------------------------------------------------
// LastShotResult (Snapshot.lastShotResult)
//-----------------------------------------------------------------------------
namespace LastShotResult {
constexpr uint8_t MISS       = 0;
constexpr uint8_t HIT_WALL   = 1;  // v1: no visual (decals out of scope)
constexpr uint8_t HIT_PLAYER = 2;
constexpr uint8_t HIT_KILL   = 3;
} // namespace LastShotResult

//-----------------------------------------------------------------------------
// RecoilConfig — COD-model recoil (spec §1.1). MIRRORED in both repos, must
// stay in sync. fireCounter is the only determinism source: pattern index =
// (fireCounter-1) % PATTERN_LEN; no RNG. Punch is VISUAL (decays to zero);
// shotKick is the tiny real trajectory component; bloom (spread growth) is
// the main trajectory control, not the pattern.
//-----------------------------------------------------------------------------
namespace RecoilConfig {
constexpr uint16_t PATTERN_LEN = 30;              // one mag; idx = (fireCounter-1) % 30

struct WeaponSpec {
  float punchPitchDeg;      // per-shot punch rise at full envelope (deg)
  float punchYawDeg;        // per-shot yaw sway amplitude (deg), zigzag sign
  float realKickPitchDeg;   // real (non-decaying) kick per shot (deg)
  float spreadBaseDegHip;   // starting cone half-angle HIP (deg)
  float spreadBaseDegAds;   // starting cone half-angle ADS (deg)
  float bloomPerShotDeg;    // spread growth per shot (deg)
  float bloomMaxDeg;        // spread growth cap (deg)
  float decayHz;            // punch/bloom exponential decay rate (1/s)
  float adsPunchScale;      // punch scale while ADS (0.8 = steadier)
};

// RED: heavy rifle — big punch, 600 RPM (matches RED_RPM/RED_DAMAGE)
constexpr WeaponSpec RED_SPEC{
    /*punchPitchDeg    */ 0.55f,
    /*punchYawDeg      */ 0.30f,
    /*realKickPitchDeg */ 0.05f,
    /*spreadBaseDegHip */ 1.2f,
    /*spreadBaseDegAds */ 0.2f,
    /*bloomPerShotDeg  */ 0.15f,
    /*bloomMaxDeg      */ 1.5f,
    /*decayHz          */ 5.0f,
    /*adsPunchScale    */ 0.8f,
};
// BLUE: light rifle — smaller punch, 800 RPM (matches BLUE_RPM/BLUE_DAMAGE)
constexpr WeaponSpec BLUE_SPEC{
    /*punchPitchDeg    */ 0.35f,
    /*punchYawDeg      */ 0.20f,
    /*realKickPitchDeg */ 0.03f,
    /*spreadBaseDegHip */ 1.2f,
    /*spreadBaseDegAds */ 0.2f,
    /*bloomPerShotDeg  */ 0.15f,
    /*bloomMaxDeg      */ 1.5f,
    /*decayHz          */ 5.0f,
    /*adsPunchScale    */ 0.8f,
};

constexpr const WeaponSpec& SpecForTeam(uint8_t teamId) {
  return (teamId == PlayerTeam::RED) ? RED_SPEC : BLUE_SPEC;
}
} // namespace RecoilConfig

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
// Lag compensation constants (shared between client and server)
//-----------------------------------------------------------------------------
namespace LagCompConfig {
// Max ticks the server will rewind hitboxes for a shot (anti-cheat bound).
// 16 ticks = 500ms @ 32Hz — covers ~62.5ms client interp delay + high one-way
// latency. Client-reported viewTick older than this is clamped.
constexpr uint32_t MAX_REWIND_TICKS = 16;
} // namespace LagCompConfig

//-----------------------------------------------------------------------------
// KillFeedEntry - One discrete kill event in the Snapshot's kill-feed ring
//-----------------------------------------------------------------------------
struct KillFeedEntry {
  uint8_t killerId;    // playerId of the killer
  uint8_t victimId;    // playerId of the victim
  uint8_t killerTeam;  // PlayerTeam::RED or BLUE
  uint8_t victimTeam;  // PlayerTeam::RED or BLUE
};
static_assert(sizeof(KillFeedEntry) == 4, "KillFeedEntry must stay 4 bytes");

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
  uint8_t matchState;                               // MatchState::PLAYING / ENDED
  uint16_t redScore;                                // RED team total kills
  uint16_t blueScore;                               // BLUE team total kills
  float matchTimeRemaining;                         // Seconds left in match (clamps at 0)
  uint32_t latestKillSeq;                          // Total kills so far (kill-feed seq)
  uint8_t winningTeam;                             // PlayerTeam/MatchTeam (NONE while playing)
  uint8_t lastShotResult;                          // NEW: LastShotResult of this player's latest shot
  uint8_t lastShotSeqMod;                          // NEW: fireCounter & 0xFF when that shot resolved
  uint8_t padding_snap[1];                         // alignment (was [3]; lsr/lss took 2 bytes)
  KillFeedEntry recentKills[KILL_FEED_SIZE];        // Kill-event ring (newest = seq-1)
  RemotePlayerEntry remotePlayers[MAX_PLAYERS - 1]; // Other players' states
};

//-----------------------------------------------------------------------------
// Size guards for network serialization (memcpy)
// If these fire, struct layout changed and both client/server must be updated.
//-----------------------------------------------------------------------------
static_assert(sizeof(InputCmd) == 32,
              "InputCmd size changed - update network serialization");
static_assert(sizeof(NetPlayerState) == 68,
              "NetPlayerState size changed - update network serialization");
static_assert(sizeof(RemotePlayerEntry) == 72,
              "RemotePlayerEntry size changed - update network serialization");
// Snapshot = 104-byte header (fixed 96 + winningTeam/lsr/lss/pad 4 + ring 32)
// + RemotePlayerEntry array. 136 also keeps the 8-byte tail alignment
// (double serverTime) exact: 136 + 72*9 = 784, divisible by 8.
static_assert(sizeof(Snapshot) == 136 + 72 * (MAX_PLAYERS - 1),
              "Snapshot layout changed - update network serialization");
