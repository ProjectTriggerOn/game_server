//=============================================================================
// game_server.cpp
//
// Game Server implementation with fixed 32Hz tick rate.
// Uses accumulator pattern for frame-rate independent simulation.
//
// Physics parameters match game_client MockServer for consistency.
//=============================================================================

#include "game_server.h"
#include "i_network.h"
#include <cmath>

GameServer::GameServer()
    : m_pNetwork(nullptr)
    , m_Accumulator(0.0)
    , m_ServerTime(0.0)
    , m_CurrentTick(0)
    , m_PlayerState{}
    , m_LastInputCmd{}
{
}

GameServer::~GameServer()
{
    Finalize();
}

void GameServer::Initialize(INetwork* pNetwork)
{
    m_pNetwork = pNetwork;
    m_Accumulator = 0.0;
    m_ServerTime = 0.0;
    m_CurrentTick = 0;

    // Initialize player state
    m_PlayerState.tickId = 0;
    m_PlayerState.position = { 0.0f, 0.0f, -20.0f };
    m_PlayerState.velocity = { 0.0f, 0.0f, 0.0f };
    m_PlayerState.yaw = 0.0f;
    m_PlayerState.pitch = 0.0f;
    m_PlayerState.stateFlags = NetStateFlags::IS_GROUNDED;

    m_LastInputCmd = {};
}

void GameServer::Finalize()
{
    m_pNetwork = nullptr;
}

//-----------------------------------------------------------------------------
// Update - Called every loop iteration
//
// Accumulator pattern: ensures exactly TICK_RATE ticks per second
//-----------------------------------------------------------------------------
void GameServer::Update(double deltaTime)
{
    if (!m_pNetwork) return;

    // Clamp deltaTime to prevent spiral of death on spikes
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

    // 1. Consume all pending input commands
    InputCmd cmd;
    while (m_pNetwork->ReceiveInputCmd(cmd))
    {
        ProcessInputCmd(cmd);
    }

    // 2. Simulate physics for this tick
    SimulatePhysics();

    // 3. Update tick ID in state
    m_PlayerState.tickId = m_CurrentTick;

    // 4. Broadcast snapshot to client
    BroadcastSnapshot();
}

//-----------------------------------------------------------------------------
// ProcessInputCmd - Handle input from client
//-----------------------------------------------------------------------------
void GameServer::ProcessInputCmd(const InputCmd& cmd)
{
    m_LastInputCmd = cmd;

    m_PlayerState.yaw = cmd.yaw;
    m_PlayerState.pitch = cmd.pitch;

    uint32_t flags = m_PlayerState.stateFlags;

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
        // Start reload latch — keep IS_RELOADING active for duration
        if (m_ReloadTimer <= 0.0)
            m_ReloadTimer = RELOAD_DURATION;
    }
    
    // Reload latch timer
    if (m_ReloadTimer > 0.0)
    {
        flags |= NetStateFlags::IS_RELOADING;
        m_ReloadTimer -= TICK_DURATION;
        if (m_ReloadTimer <= 0.0)
        {
            m_ReloadTimer = 0.0;
            flags &= ~NetStateFlags::IS_RELOADING;
        }
    }
    else
    {
        flags &= ~NetStateFlags::IS_RELOADING;
    }

    m_PlayerState.stateFlags = flags;
}

//-----------------------------------------------------------------------------
// SimulatePhysics - Server-side authoritative physics simulation
//
// CS:GO / Valorant Style Movement:
//   - Ground: Snappy, instant response, no sliding
//   - Air: Momentum preservation, limited air control
//-----------------------------------------------------------------------------
void GameServer::SimulatePhysics()
{
    const float dt = static_cast<float>(TICK_DURATION);

    // Movement parameters (must match game_client for prediction consistency)
    constexpr float MAX_WALK_SPEED = 5.0f;
    constexpr float MAX_RUN_SPEED  = 8.0f;
    constexpr float GROUND_ACCEL   = 50.0f;
    constexpr float AIR_ACCEL      = 2.0f;
    constexpr float GRAVITY        = 20.0f;
    constexpr float JUMP_VELOCITY  = 8.0f;

    bool isGrounded = (m_PlayerState.stateFlags & NetStateFlags::IS_GROUNDED) != 0;

    // Calculate target velocity from input
    float yaw = m_LastInputCmd.yaw;
    float frontX = sinf(yaw);
    float frontZ = cosf(yaw);
    float rightX = frontZ;
    float rightZ = -frontX;

    float moveX = m_LastInputCmd.moveAxisX * rightX + m_LastInputCmd.moveAxisY * frontX;
    float moveZ = m_LastInputCmd.moveAxisX * rightZ + m_LastInputCmd.moveAxisY * frontZ;

    float moveMag = sqrtf(moveX * moveX + moveZ * moveZ);
    if (moveMag > 1.0f)
    {
        moveX /= moveMag;
        moveZ /= moveMag;
        moveMag = 1.0f;
    }

    float maxSpeed = (m_LastInputCmd.buttons & InputButtons::SPRINT) ? MAX_RUN_SPEED : MAX_WALK_SPEED;
    float targetVelX = moveX * maxSpeed;
    float targetVelZ = moveZ * maxSpeed;

    if (isGrounded)
    {
        // Ground: snappy, high acceleration
        float accelStep = GROUND_ACCEL * dt;

        float diffX = targetVelX - m_PlayerState.velocity.x;
        if (fabsf(diffX) <= accelStep)
            m_PlayerState.velocity.x = targetVelX;
        else
            m_PlayerState.velocity.x += (diffX > 0 ? accelStep : -accelStep);

        float diffZ = targetVelZ - m_PlayerState.velocity.z;
        if (fabsf(diffZ) <= accelStep)
            m_PlayerState.velocity.z = targetVelZ;
        else
            m_PlayerState.velocity.z += (diffZ > 0 ? accelStep : -accelStep);

        // Jump
        if (m_LastInputCmd.buttons & InputButtons::JUMP)
        {
            m_PlayerState.velocity.y = JUMP_VELOCITY;
            m_PlayerState.stateFlags &= ~NetStateFlags::IS_GROUNDED;
            m_PlayerState.stateFlags |= NetStateFlags::IS_JUMPING;
            isGrounded = false;
        }
    }
    else
    {
        // Air: momentum preservation, limited control
        float airStep = AIR_ACCEL * dt;

        if (moveMag > 0.01f)
        {
            m_PlayerState.velocity.x += moveX * airStep;
            m_PlayerState.velocity.z += moveZ * airStep;

            float horizSpeed = sqrtf(m_PlayerState.velocity.x * m_PlayerState.velocity.x +
                                     m_PlayerState.velocity.z * m_PlayerState.velocity.z);
            if (horizSpeed > maxSpeed * 1.2f)
            {
                float scale = (maxSpeed * 1.2f) / horizSpeed;
                m_PlayerState.velocity.x *= scale;
                m_PlayerState.velocity.z *= scale;
            }
        }
    }

    // Gravity
    if (!isGrounded)
    {
        m_PlayerState.velocity.y -= GRAVITY * dt;
    }

    // Apply velocity to position
    m_PlayerState.position.x += m_PlayerState.velocity.x * dt;
    m_PlayerState.position.z += m_PlayerState.velocity.z * dt;
    m_PlayerState.position.y += m_PlayerState.velocity.y * dt;

    // Floor collision (y = 0)
    if (m_PlayerState.position.y <= 0.0f)
    {
        m_PlayerState.position.y = 0.0f;
        m_PlayerState.velocity.y = 0.0f;
        m_PlayerState.stateFlags |= NetStateFlags::IS_GROUNDED;
        m_PlayerState.stateFlags &= ~NetStateFlags::IS_JUMPING;
    }
}

//-----------------------------------------------------------------------------
// BroadcastSnapshot - Send authoritative state to all clients
//-----------------------------------------------------------------------------
void GameServer::BroadcastSnapshot()
{
    if (!m_pNetwork) return;

    Snapshot snapshot;
    snapshot.tickId = m_CurrentTick;
    snapshot.serverTime = m_ServerTime;
    snapshot.localPlayer = m_PlayerState;

    m_pNetwork->SendSnapshot(snapshot);
}
