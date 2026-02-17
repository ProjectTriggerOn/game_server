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
    // Per-player data
    //-------------------------------------------------------------------------
    struct PlayerData {
        NetPlayerState state;
        InputCmd lastInput{};
        double reloadTimer = 0.0;
        uint8_t teamId = PlayerTeam::RED;
        // Combat
        uint8_t health = 200;
        double respawnTimer = 0.0;
        double fireTimer = 0.0;
        uint16_t fireCounter = 0;
    };

    void Tick();
    void ProcessPlayerEvents();
    void ProcessInputCmd(const InputCmd& cmd, uint8_t playerId);
    void ProcessFiring(PlayerData& shooter, uint8_t shooterId);
    bool RaycastPlayers(const Float3& origin, const Float3& dir,
                        uint8_t excludeId, uint8_t excludeTeam,
                        uint8_t& outHitId, float& outDist);
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

    // Collision world for gravity
    std::vector<ServerCollider> m_Colliders;

    // Player collision parameters (must match client)
    static constexpr float PLAYER_HEIGHT = 2.0f;
    static constexpr float CAPSULE_RADIUS = 0.5f;

    // Weapon parameters per team
    static constexpr double RED_RPM = 600.0;
    static constexpr uint8_t RED_DAMAGE = 34;
    static constexpr double BLUE_RPM = 800.0;
    static constexpr uint8_t BLUE_DAMAGE = 25;
    static constexpr uint8_t MAX_HEALTH = 200;
    static constexpr double RESPAWN_TIME = 2.0;
};
