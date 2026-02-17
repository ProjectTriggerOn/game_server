//=============================================================================
// game_server.cpp
//
// Game Server implementation with fixed 32Hz tick rate.
// Supports multiple concurrent players with per-player state tracking.
//=============================================================================

#include "game_server.h"
#include "enet_server_network.h"
#include <cmath>
#include <cstdio>

GameServer::GameServer()
    : m_pNetwork(nullptr)
    , m_Accumulator(0.0)
    , m_ServerTime(0.0)
    , m_CurrentTick(0)
{
}

GameServer::~GameServer()
{
    Finalize();
}

void GameServer::Initialize(ENetServerNetwork* pNetwork)
{
    m_pNetwork = pNetwork;
    m_Accumulator = 0.0;
    m_ServerTime = 0.0;
    m_CurrentTick = 0;
    m_Players.clear();

    // Register ground collider (must match client CollisionWorld)
    m_Colliders.clear();
    m_Colliders.push_back({ {{ -128.0f, -1.0f, -128.0f }, { 128.0f, 0.0f, 128.0f }}, true });
    m_Colliders.push_back({ {{ 5.0f, 0.0f, 5.0f }, { 10.0f, 2.0f, 10.0f }}, true });  // Test platform
}

void GameServer::Finalize()
{
    m_pNetwork = nullptr;
    m_Players.clear();
}

//-----------------------------------------------------------------------------
// AssignTeam - Pick the team with fewer members (tie → RED)
//-----------------------------------------------------------------------------
uint8_t GameServer::AssignTeam() const
{
    int redCount = 0, blueCount = 0;
    for (const auto& [id, player] : m_Players)
    {
        if (player.teamId == PlayerTeam::RED)  redCount++;
        else                                    blueCount++;
    }
    return (blueCount < redCount) ? PlayerTeam::BLUE : PlayerTeam::RED;
}

//-----------------------------------------------------------------------------
// Spawn position per team + player ID
//-----------------------------------------------------------------------------
Float3 GameServer::GetSpawnPosition(uint8_t playerId, uint8_t teamId)
{
    float offsets[] = { 0.0f, 5.0f, -5.0f, 10.0f };
    float x = offsets[playerId % 4];
    if (teamId == PlayerTeam::BLUE)
        x = -x;  // Mirror for blue team
    float z = (teamId == PlayerTeam::RED) ? -20.0f : 20.0f;
    return { x, 0.0f, z };
}

//-----------------------------------------------------------------------------
// Player connect/disconnect handlers
//-----------------------------------------------------------------------------
void GameServer::OnPlayerConnected(uint8_t playerId)
{
    uint8_t team = AssignTeam();

    PlayerData data;
    data.teamId = team;
    data.state.tickId = m_CurrentTick;
    data.state.position = GetSpawnPosition(playerId, team);
    data.state.velocity = { 0.0f, 0.0f, 0.0f };
    data.state.yaw = 0.0f;
    data.state.pitch = 0.0f;
    data.state.stateFlags = NetStateFlags::IS_GROUNDED;
    data.lastInput = {};
    data.reloadTimer = 0.0;

    m_Players[playerId] = data;
    printf("[GameServer] Player %u (Team %s) spawned at (%.1f, %.1f, %.1f)\n",
        playerId, (team == PlayerTeam::RED) ? "RED" : "BLUE",
        data.state.position.x, data.state.position.y, data.state.position.z);
}

void GameServer::OnPlayerDisconnected(uint8_t playerId)
{
    m_Players.erase(playerId);
    printf("[GameServer] Player %u removed\n", playerId);
}

//-----------------------------------------------------------------------------
// Update - Called every loop iteration
//-----------------------------------------------------------------------------
void GameServer::Update(double deltaTime)
{
    if (!m_pNetwork) return;

    const double maxDelta = TICK_DURATION * 4.0;
    deltaTime = (deltaTime > maxDelta) ? maxDelta : deltaTime;

    m_Accumulator += deltaTime;

    while (m_Accumulator >= TICK_DURATION)
    {
        Tick();
        m_Accumulator -= TICK_DURATION;
    }
}

//-----------------------------------------------------------------------------
// Tick - Fixed rate game logic (32Hz)
//-----------------------------------------------------------------------------
void GameServer::Tick()
{
    m_CurrentTick++;
    m_ServerTime += TICK_DURATION;

    // 1. Process player connect/disconnect events
    ProcessPlayerEvents();

    // 2. Consume all pending input commands (routed by playerId)
    TaggedInput taggedInput;
    while (m_pNetwork->ReceiveTaggedInput(taggedInput))
    {
        ProcessInputCmd(taggedInput.cmd, taggedInput.playerId);
    }

    // 3. Simulate physics for all players
    SimulatePhysics();

    // 4. Update tick ID in all player states
    for (auto& [id, player] : m_Players)
    {
        player.state.tickId = m_CurrentTick;
    }

    // 5. Broadcast per-player snapshots
    BroadcastSnapshots();
}

//-----------------------------------------------------------------------------
// ProcessPlayerEvents - Handle connect/disconnect from network layer
//-----------------------------------------------------------------------------
void GameServer::ProcessPlayerEvents()
{
    PlayerEvent evt;
    while (m_pNetwork->PollPlayerEvent(evt))
    {
        if (evt.connected)
            OnPlayerConnected(evt.playerId);
        else
            OnPlayerDisconnected(evt.playerId);
    }
}

//-----------------------------------------------------------------------------
// ProcessInputCmd - Handle input from a specific player
//-----------------------------------------------------------------------------
void GameServer::ProcessInputCmd(const InputCmd& cmd, uint8_t playerId)
{
    auto it = m_Players.find(playerId);
    if (it == m_Players.end()) return;

    PlayerData& player = it->second;
    player.lastInput = cmd;

    player.state.yaw = cmd.yaw;
    player.state.pitch = cmd.pitch;

    uint32_t flags = player.state.stateFlags;

    if (cmd.buttons & InputButtons::FIRE)
        flags |= NetStateFlags::IS_FIRING;
    else
        flags &= ~NetStateFlags::IS_FIRING;

    if (cmd.buttons & InputButtons::ADS)
        flags |= NetStateFlags::IS_ADS;
    else
        flags &= ~NetStateFlags::IS_ADS;

    if (cmd.buttons & InputButtons::RELOAD)
    {
        if (player.reloadTimer <= 0.0)
            player.reloadTimer = 10.0; // RELOAD_DURATION
    }

    if (player.reloadTimer > 0.0)
    {
        flags |= NetStateFlags::IS_RELOADING;
        player.reloadTimer -= TICK_DURATION;
        if (player.reloadTimer <= 0.0)
        {
            player.reloadTimer = 0.0;
            flags &= ~NetStateFlags::IS_RELOADING;
        }
    }
    else
    {
        flags &= ~NetStateFlags::IS_RELOADING;
    }

    player.state.stateFlags = flags;
}

//-----------------------------------------------------------------------------
// SimulatePhysics - Run physics for all players
//-----------------------------------------------------------------------------
void GameServer::SimulatePhysics()
{
    for (auto& [id, player] : m_Players)
    {
        SimulatePlayerPhysics(player);
    }
}

//-----------------------------------------------------------------------------
// SimulatePlayerPhysics - CS:GO / Valorant style movement for one player
//-----------------------------------------------------------------------------
void GameServer::SimulatePlayerPhysics(PlayerData& player)
{
    const float dt = static_cast<float>(TICK_DURATION);

    constexpr float MAX_WALK_SPEED = 5.0f;
    constexpr float MAX_RUN_SPEED  = 8.0f;
    constexpr float GROUND_ACCEL   = 50.0f;
    constexpr float AIR_ACCEL      = 2.0f;
    constexpr float GRAVITY        = 20.0f;
    constexpr float JUMP_VELOCITY  = 8.0f;

    NetPlayerState& state = player.state;
    const InputCmd& input = player.lastInput;

    bool isGrounded = (state.stateFlags & NetStateFlags::IS_GROUNDED) != 0;

    float yaw = input.yaw;
    float frontX = sinf(yaw);
    float frontZ = cosf(yaw);
    float rightX = frontZ;
    float rightZ = -frontX;

    float moveX = input.moveAxisX * rightX + input.moveAxisY * frontX;
    float moveZ = input.moveAxisX * rightZ + input.moveAxisY * frontZ;

    float moveMag = sqrtf(moveX * moveX + moveZ * moveZ);
    if (moveMag > 1.0f)
    {
        moveX /= moveMag;
        moveZ /= moveMag;
        moveMag = 1.0f;
    }

    float maxSpeed = (input.buttons & InputButtons::SPRINT) ? MAX_RUN_SPEED : MAX_WALK_SPEED;
    float targetVelX = moveX * maxSpeed;
    float targetVelZ = moveZ * maxSpeed;

    if (isGrounded)
    {
        float accelStep = GROUND_ACCEL * dt;

        float diffX = targetVelX - state.velocity.x;
        if (fabsf(diffX) <= accelStep)
            state.velocity.x = targetVelX;
        else
            state.velocity.x += (diffX > 0 ? accelStep : -accelStep);

        float diffZ = targetVelZ - state.velocity.z;
        if (fabsf(diffZ) <= accelStep)
            state.velocity.z = targetVelZ;
        else
            state.velocity.z += (diffZ > 0 ? accelStep : -accelStep);

        if (input.buttons & InputButtons::JUMP)
        {
            state.velocity.y = JUMP_VELOCITY;
            state.stateFlags &= ~NetStateFlags::IS_GROUNDED;
            state.stateFlags |= NetStateFlags::IS_JUMPING;
            isGrounded = false;
        }
    }
    else
    {
        float airStep = AIR_ACCEL * dt;

        if (moveMag > 0.01f)
        {
            state.velocity.x += moveX * airStep;
            state.velocity.z += moveZ * airStep;

            float horizSpeed = sqrtf(state.velocity.x * state.velocity.x +
                                     state.velocity.z * state.velocity.z);
            if (horizSpeed > maxSpeed * 1.2f)
            {
                float scale = (maxSpeed * 1.2f) / horizSpeed;
                state.velocity.x *= scale;
                state.velocity.z *= scale;
            }
        }
    }

    if (!isGrounded)
    {
        state.velocity.y -= GRAVITY * dt;
    }

    state.position.x += state.velocity.x * dt;
    state.position.z += state.velocity.z * dt;
    state.position.y += state.velocity.y * dt;

    // Collision Detection (Capsule vs World AABBs)
    if (!m_Colliders.empty())
    {
        auto result = ServerCollision::ResolveCapsule(
            m_Colliders, state.position,
            PLAYER_HEIGHT, CAPSULE_RADIUS, state.velocity);
        state.position = result.position;
        state.velocity = result.velocity;
        if (result.isGrounded)
        {
            state.stateFlags |= NetStateFlags::IS_GROUNDED;
            state.stateFlags &= ~NetStateFlags::IS_JUMPING;
        }
        else
        {
            state.stateFlags &= ~NetStateFlags::IS_GROUNDED;
        }
    }
    else
    {
        // Fallback: simple floor at y=0
        if (state.position.y <= 0.0f)
        {
            state.position.y = 0.0f;
            state.velocity.y = 0.0f;
            state.stateFlags |= NetStateFlags::IS_GROUNDED;
            state.stateFlags &= ~NetStateFlags::IS_JUMPING;
        }
    }
}

//-----------------------------------------------------------------------------
// BroadcastSnapshots - Send per-player snapshots
//
// Each player receives a Snapshot where:
//   localPlayer = their own state
//   remotePlayers[] = all other players' states
//-----------------------------------------------------------------------------
void GameServer::BroadcastSnapshots()
{
    if (!m_pNetwork || m_Players.empty()) return;

    for (const auto& [myId, myData] : m_Players)
    {
        Snapshot snapshot = {};
        snapshot.tickId = m_CurrentTick;
        snapshot.serverTime = m_ServerTime;
        snapshot.localPlayer = myData.state;
        snapshot.localPlayerId = myId;
        snapshot.localPlayerTeam = myData.teamId;

        // Fill remote players (everyone except me)
        uint8_t remoteCount = 0;
        for (const auto& [otherId, otherData] : m_Players)
        {
            if (otherId == myId) continue;
            if (remoteCount >= MAX_PLAYERS - 1) break;

            snapshot.remotePlayers[remoteCount].playerId = otherId;
            snapshot.remotePlayers[remoteCount].teamId = otherData.teamId;
            snapshot.remotePlayers[remoteCount].state = otherData.state;
            remoteCount++;
        }
        snapshot.remotePlayerCount = remoteCount;

        m_pNetwork->SendSnapshotToPlayer(myId, snapshot);
    }
}
