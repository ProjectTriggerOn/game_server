#pragma once
//=============================================================================
// game_server.h
//
// Game Server with fixed-tick (32Hz) game logic.
// Uses accumulator pattern to decouple from update loop rate.
//
// Architecture:
//   - Server runs at fixed 32Hz tick rate
//   - Consumes InputCmd from client
//   - Produces authoritative PlayerState
//   - Broadcasts Snapshot to client
//=============================================================================

#include "net_common.h"

class INetwork;

class GameServer
{
public:
    //-------------------------------------------------------------------------
    // Constants
    //-------------------------------------------------------------------------
    static constexpr double TICK_RATE = 32.0;                    // 32 ticks per second
    static constexpr double TICK_DURATION = 1.0 / TICK_RATE;     // ~31.25ms per tick

    GameServer();
    ~GameServer();

    void Initialize(INetwork* pNetwork);
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
    const NetPlayerState& GetPlayerState() const { return m_PlayerState; }

private:
    void Tick();
    void ProcessInputCmd(const InputCmd& cmd);
    void SimulatePhysics();
    void BroadcastSnapshot();

private:
    INetwork* m_pNetwork;

    // Timing
    double m_Accumulator;
    double m_ServerTime;
    uint32_t m_CurrentTick;

    // Game State (Server Authoritative)
    NetPlayerState m_PlayerState;
    InputCmd m_LastInputCmd;
    
    // Reload latch timer — keeps IS_RELOADING active for full animation duration
    static constexpr double RELOAD_DURATION = 10.0;  // seconds
    double m_ReloadTimer = 0.0;
};
