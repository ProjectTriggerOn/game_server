#pragma once
//=============================================================================
// game_server.h
//
// Game Server with fixed-tick (32Hz) game logic.
// Supports multiple concurrent players with per-player state tracking.
//
// Architecture:
//   - Server runs at fixed 32Hz tick rate
//   - Consumes InputCmd from each connected client
//   - Produces authoritative PlayerState per player
//   - Broadcasts per-player Snapshot (your state + others' states)
//=============================================================================

#include "net_common.h"
#include "server_collision.h"
#include "server_raycast.h"
#include <unordered_map>
#include <cstddef>

class ENetServerNetwork;

class GameServer
{
public:
    //-------------------------------------------------------------------------
    // Constants
    //-------------------------------------------------------------------------
    static constexpr double TICK_RATE = 32.0;
    static constexpr double TICK_DURATION = 1.0 / TICK_RATE;

    GameServer();
    ~GameServer();

    void Initialize(ENetServerNetwork* pNetwork);
    void Finalize();

    //-------------------------------------------------------------------------
    // Called every loop iteration - uses accumulator for fixed tick
    //-------------------------------------------------------------------------
    void Update(double deltaTime);

    //-------------------------------------------------------------------------
    // Getters for debug / monitoring
    //-------------------------------------------------------------------------
    uint32_t GetCurrentTick() const { return m_CurrentTick; }
    double GetAccumulator() const { return m_Accumulator; }
    double GetServerTime() const { return m_ServerTime; }
    size_t GetPlayerCount() const { return m_Players.size(); }

private:
    //-------------------------------------------------------------------------
    // Lag compensation: one historical record per tick (ring buffer slot)
    //-------------------------------------------------------------------------
    struct PositionHistoryEntry {
        uint32_t tick = 0;       // 0 = slot never written (m_CurrentTick starts at 1)
        Float3 position{};
        bool alive = false;      // !(IS_DEAD) at record time
    };

    //-------------------------------------------------------------------------
    // Per-player data
    //-------------------------------------------------------------------------
    struct PlayerData {
        NetPlayerState state;
        InputCmd lastInput{};
        uint32_t prevButtons = 0;  // Previous cmd.buttons for edge detection
        double reloadTimer = 0.0;
        uint8_t teamId = PlayerTeam::RED;
        // Combat
        uint8_t health = 200;
        double respawnTimer = 0.0;
        double fireTimer = 0.0;
        uint16_t fireCounter = 0;
        // Score (mirrored into state.kills/deaths each tick)
        uint16_t kills = 0;
        uint16_t deaths = 0;
        // Ammo
        uint8_t ammo = WeaponConfig::MAG_SIZE;
        uint8_t ammoReserve = WeaponConfig::MAX_RESERVE;
        // Lag compensation: per-tick position history, written at the end of
        // Tick() — exactly the state BroadcastSnapshots() sends (including
        // respawn teleports). Indexed by tick % POSITION_HISTORY_SIZE.
        static constexpr size_t POSITION_HISTORY_SIZE = 64;  // 2s @ 32Hz (> MAX_REWIND_TICKS)
        PositionHistoryEntry history[POSITION_HISTORY_SIZE]{};
    };

    void Tick();
    void ProcessPlayerEvents();
    void ProcessInputCmd(const InputCmd& cmd, uint8_t playerId);
    void UpdatePlayerReloadTimer(PlayerData& player);
    void ProcessFiring(PlayerData& shooter, uint8_t shooterId);
    bool RaycastPlayers(const Float3& origin, const Float3& dir,
                        uint8_t excludeId, uint8_t excludeTeam,
                        uint32_t viewTick, float viewFrac,  // 0 = no rewind
                        uint8_t& outHitId, float& outDist);
    Float3 GetRewoundPosition(const PlayerData& target,
                              uint32_t viewTick, float viewFrac) const;
    float RaycastWorld(const Float3& origin, const Float3& dir);
    void SimulatePlayerPhysics(PlayerData& player);
    void SimulatePhysics();
    void BroadcastSnapshots();

    void OnPlayerConnected(uint8_t playerId);
    void OnPlayerDisconnected(uint8_t playerId);

    uint8_t AssignTeam() const;
    static Float3 GetSpawnPosition(uint8_t playerId, uint8_t teamId);

private:
    ENetServerNetwork* m_pNetwork;

    // Timing
    double m_Accumulator;
    double m_ServerTime;
    uint32_t m_CurrentTick;

    // Per-player game state
    std::unordered_map<uint8_t, PlayerData> m_Players;

    // Match / scoring state (broadcast in every Snapshot header)
    uint8_t  m_MatchState = MatchState::PLAYING;
    uint16_t m_RedScore = 0;
    uint16_t m_BlueScore = 0;
    uint8_t  m_WinningTeam = MatchTeam::NONE;
    double   m_MatchTimeRemaining = MatchConfig::MATCH_DURATION;
    uint32_t m_KillSeq = 0;  // total kills; also wire latestKillSeq
    KillFeedEntry m_RecentKills[KILL_FEED_SIZE] = {};

    // Collision world for gravity
    std::vector<ServerCollider> m_Colliders;

    // Player collision parameters (must match client)
    static constexpr float PLAYER_HEIGHT = 1.6f;
    static constexpr float CAPSULE_RADIUS = 0.3f;

    // Lag compensation: never lerp history across a jump larger than this —
    // it is a respawn teleport, not movement (max legit move/tick ≈ 0.3m).
    static constexpr float REWIND_TELEPORT_GUARD = 2.0f;

    // Weapon parameters per team
    static constexpr double RED_RPM = 600.0;
    static constexpr uint8_t RED_DAMAGE = 34;
    static constexpr double BLUE_RPM = 800.0;
    static constexpr uint8_t BLUE_DAMAGE = 25;
    static constexpr uint8_t MAX_HEALTH = 200;
    static constexpr double RESPAWN_TIME = 2.0;
};
