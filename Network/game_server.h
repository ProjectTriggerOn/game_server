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
    };

    void Tick();
    void ProcessPlayerEvents();
    void ProcessInputCmd(const InputCmd& cmd, uint8_t playerId);
    void SimulatePlayerPhysics(PlayerData& player);
    void SimulatePhysics();
    void BroadcastSnapshots();

    void OnPlayerConnected(uint8_t playerId);
    void OnPlayerDisconnected(uint8_t playerId);

    static Float3 GetSpawnPosition(uint8_t playerId);

private:
    ENetServerNetwork* m_pNetwork;

    // Timing
    double m_Accumulator;
    double m_ServerTime;
    uint32_t m_CurrentTick;

    // Per-player game state
    std::unordered_map<uint8_t, PlayerData> m_Players;
};
