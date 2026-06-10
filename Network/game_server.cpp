//=============================================================================
// game_server.cpp
//
// Game Server implementation with fixed 32Hz tick rate.
// Supports multiple concurrent players with per-player state tracking.
//=============================================================================

#include "game_server.h"
#include "enet_server_network.h"
#include "map_colliders.h"
#include "../server_log.h"
#include <cfloat>
#include <cmath>
#include <cstdlib>

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

    // Register colliders from shared map data (must match client)
    m_Colliders.clear();
    for (int i = 0; i < MAP_COLLIDER_COUNT; i++)
    {
        const MapColliderDef& def = MAP_COLLIDERS[i];
        ServerCollider sc;
        sc.aabb = {
            { def.minX, def.minY, def.minZ },
            { def.maxX, def.maxY, def.maxZ }
        };
        sc.isGround = def.isGround;
        m_Colliders.push_back(sc);
    }
}

void GameServer::Finalize()
{
    m_pNetwork = nullptr;
    m_Players.clear();
}

//-----------------------------------------------------------------------------
// AssignTeam - Balance teams, capped at MAX_TEAM_SIZE (MAX_PLAYERS / 2) each.
// A full team forces the new player onto the other side; otherwise the smaller
// team is chosen (tie → RED). With the ENet host capped at MAX_PLAYERS this
// keeps a 5v5 split.
//-----------------------------------------------------------------------------
uint8_t GameServer::AssignTeam() const
{
    constexpr int MAX_TEAM_SIZE = MAX_PLAYERS / 2;
    int redCount = 0, blueCount = 0;
    for (const auto& [id, player] : m_Players)
    {
        if (player.teamId == PlayerTeam::RED)  redCount++;
        else                                    blueCount++;
    }
    if (redCount >= MAX_TEAM_SIZE)  return PlayerTeam::BLUE;
    if (blueCount >= MAX_TEAM_SIZE) return PlayerTeam::RED;
    return (blueCount < redCount) ? PlayerTeam::BLUE : PlayerTeam::RED;
}

//-----------------------------------------------------------------------------
// Spawn position — team-based strips on opposite sides of the map.
//
// RED  spawns on the -Z side strip:  X in [-9, 9], Z in [-9, -6]
// BLUE spawns on the +Z side strip:  X in [-9, 9], Z in [ 6,  9]
// Both strips clear the four central blocks (which occupy Z in [-5,-2] and
// [2,5]; see map_colliders.h) and are wide enough to spread 5 players.
//-----------------------------------------------------------------------------
Float3 GameServer::GetSpawnPosition(uint8_t /*playerId*/, uint8_t teamId)
{
    const float minX = -9.0f, maxX = 9.0f;
    float minZ, maxZ;
    if (teamId == PlayerTeam::RED) { minZ = -9.0f; maxZ = -6.0f; }
    else                          { minZ =  6.0f; maxZ =  9.0f; }

    float x = minX + static_cast<float>(rand()) / RAND_MAX * (maxX - minX);
    float z = minZ + static_cast<float>(rand()) / RAND_MAX * (maxZ - minZ);
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
    data.state.lastProcessedInputTick = 0;
    data.state.position = GetSpawnPosition(playerId, team);
    data.state.velocity = { 0.0f, 0.0f, 0.0f };
    data.state.yaw = 0.0f;
    data.state.pitch = 0.0f;
    data.state.stateFlags = NetStateFlags::IS_GROUNDED;
    data.state.health = MAX_HEALTH;
    data.state.hitByPlayerId = 0xFF;
    data.state.fireCounter = 0;
    data.lastInput = {};
    data.reloadTimer = 0.0;
    data.health = MAX_HEALTH;
    data.respawnTimer = 0.0;
    data.fireTimer = 0.0;
    data.fireCounter = 0;
    data.ammo = WeaponConfig::MAG_SIZE;
    data.ammoReserve = WeaponConfig::MAX_RESERVE;
    data.state.ammo = WeaponConfig::MAG_SIZE;
    data.state.ammoReserve = WeaponConfig::MAX_RESERVE;

    m_Players[playerId] = data;
    SLOG_INFO("Player %u (Team %s) spawned at (%.1f, %.1f, %.1f)",
        playerId, (team == PlayerTeam::RED) ? "RED" : "BLUE",
        data.state.position.x, data.state.position.y, data.state.position.z);
}

void GameServer::OnPlayerDisconnected(uint8_t playerId)
{
    m_Players.erase(playerId);
    SLOG_INFO("Player %u removed", playerId);
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

    // 2. Update reload timers once per tick (BEFORE input so newly-started reloads
    //    this tick last exactly RELOAD_DURATION rather than RELOAD_DURATION - TICK_DURATION)
    for (auto& [id, player] : m_Players)
    {
        UpdatePlayerReloadTimer(player);
    }

    // 3. Consume all pending input commands (routed by playerId)
    TaggedInput taggedInput;
    while (m_pNetwork->ReceiveTaggedInput(taggedInput))
    {
        ProcessInputCmd(taggedInput.cmd, taggedInput.playerId);
    }

    // 4. Simulate physics for all players
    SimulatePhysics();

    // 5. Process firing and combat for all players
    for (auto& [id, player] : m_Players)
    {
        // Clear hit marker each tick
        player.state.hitByPlayerId = 0xFF;

        // Respawn timer
        if (player.state.stateFlags & NetStateFlags::IS_DEAD)
        {
            player.respawnTimer -= TICK_DURATION;
            if (player.respawnTimer <= 0.0)
            {
                // Respawn
                player.health = MAX_HEALTH;
                player.state.stateFlags &= ~NetStateFlags::IS_DEAD;
                player.state.stateFlags |= NetStateFlags::IS_GROUNDED;
                player.state.position = GetSpawnPosition(id, player.teamId);
                player.state.velocity = { 0.0f, 0.0f, 0.0f };
                player.respawnTimer = 0.0;
                player.ammo = WeaponConfig::MAG_SIZE;
                player.ammoReserve = WeaponConfig::MAX_RESERVE;
                // Reset reload state — a player killed mid-reload should wake up clean,
                // not stuck with phantom IS_RELOADING blocking fire until the old timer expires.
                player.reloadTimer = 0.0;
                player.state.stateFlags &= ~(NetStateFlags::IS_RELOADING | NetStateFlags::IS_RELOAD_EMPTY);
                // Reset edge-detection so any held button across death/respawn isn't mis-detected.
                player.prevButtons = 0;
                SLOG_INFO("Player %u respawned", id);
            }
        }
        else
        {
            ProcessFiring(player, id);
        }

        // Sync combat data to state
        player.state.health = player.health;
        player.state.fireCounter = player.fireCounter;
        player.state.ammo = player.ammo;
        player.state.ammoReserve = player.ammoReserve;
    }

    // 5. Update tick ID in all player states (and ack of last processed input).
    // Clients use lastProcessedInputTick to look up the matching entry in their
    // input-history ring buffer for prediction reconciliation (RESIM).
    for (auto& [id, player] : m_Players)
    {
        player.state.tickId = m_CurrentTick;
        player.state.lastProcessedInputTick = player.lastInput.tickId;
    }

    // 6. Broadcast per-player snapshots
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
    uint32_t prevButtons = player.prevButtons;
    player.prevButtons = cmd.buttons;
    uint32_t newlyPressed = cmd.buttons & ~prevButtons;  // 0→1 edges
    player.lastInput = cmd;

    // Dead players: only update camera, skip all actions
    if (player.state.stateFlags & NetStateFlags::IS_DEAD)
    {
        player.state.yaw = cmd.yaw;
        player.state.pitch = cmd.pitch;
        return;
    }

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
        if (player.reloadTimer <= 0.0 && player.ammo < WeaponConfig::MAG_SIZE && player.ammoReserve > 0)
        {
            if (player.ammo == 0) {
                player.reloadTimer = WeaponConfig::RELOAD_OUT_OF_AMMO_DURATION;
                flags |= NetStateFlags::IS_RELOAD_EMPTY;
            } else {
                player.reloadTimer = WeaponConfig::RELOAD_DURATION;
            }
            // Set IS_RELOADING immediately so ProcessFiring's gate blocks fire this tick.
            // UpdatePlayerReloadTimer already ran for this tick, so this won't be decremented
            // until next tick — giving the reload exactly RELOAD_DURATION before completion.
            flags |= NetStateFlags::IS_RELOADING;
        }
    }

    // Interrupt reload on trigger-edges (FIRE/ADS/SPRINT/JUMP)
    constexpr uint32_t INTERRUPT_MASK =
        InputButtons::FIRE | InputButtons::ADS |
        InputButtons::SPRINT | InputButtons::JUMP;
    if (player.reloadTimer > 0.0 && (newlyPressed & INTERRUPT_MASK))
    {
        player.reloadTimer = 0.0;
        flags &= ~NetStateFlags::IS_RELOADING;
        flags &= ~NetStateFlags::IS_RELOAD_EMPTY;
        // No ammo refill on interrupt
    }

    // Inspect: set when INSPECT pressed, clear on any action input
    bool hasActionInput = (cmd.buttons & (InputButtons::FIRE | InputButtons::ADS | InputButtons::RELOAD | InputButtons::JUMP | InputButtons::SPRINT)) != 0
        || fabsf(cmd.moveAxisX) > 0.01f || fabsf(cmd.moveAxisY) > 0.01f;

    if (cmd.buttons & InputButtons::INSPECT)
    {
        flags |= NetStateFlags::IS_INSPECTING;
    }
    else if ((flags & NetStateFlags::IS_INSPECTING) && hasActionInput)
    {
        flags &= ~NetStateFlags::IS_INSPECTING;
    }

    player.state.stateFlags = flags;
}

//-----------------------------------------------------------------------------
// UpdatePlayerReloadTimer - Decrement reload timer once per tick
// MUST be called exactly once per Tick(), BEFORE InputCmd processing — this
// ensures reloads started in ProcessInputCmd last exactly RELOAD_DURATION
// (no same-tick decrement) and matches ProcessFiring's auto-reload duration.
//-----------------------------------------------------------------------------
void GameServer::UpdatePlayerReloadTimer(PlayerData& player)
{
    uint32_t& flags = player.state.stateFlags;

    if (player.reloadTimer > 0.0)
    {
        flags |= NetStateFlags::IS_RELOADING;
        player.reloadTimer -= TICK_DURATION;
        if (player.reloadTimer <= 0.0)
        {
            player.reloadTimer = 0.0;
            flags &= ~NetStateFlags::IS_RELOADING;
            flags &= ~NetStateFlags::IS_RELOAD_EMPTY;

            // Refill magazine from reserve
            int needed = WeaponConfig::MAG_SIZE - player.ammo;
            int refill = (player.ammoReserve >= needed) ? needed : player.ammoReserve;
            player.ammo += static_cast<uint8_t>(refill);
            player.ammoReserve -= static_cast<uint8_t>(refill);
        }
    }
    else
    {
        flags &= ~NetStateFlags::IS_RELOADING;
    }
}

//-----------------------------------------------------------------------------
// SimulatePhysics - Run physics for all players
//-----------------------------------------------------------------------------
void GameServer::SimulatePhysics()
{
    for (auto& [id, player] : m_Players)
    {
        // Skip dead players
        if (player.state.stateFlags & NetStateFlags::IS_DEAD)
            continue;
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
    bool wasGroundedAtStart = isGrounded;

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
            const float airCap = maxSpeed * PhysicsConfig::AIR_STRAFE_SPEED_MULT;
            if (horizSpeed > airCap)
            {
                float scale = airCap / horizSpeed;
                state.velocity.x *= scale;
                state.velocity.z *= scale;
            }
        }
    }

    if (!wasGroundedAtStart)
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

//-----------------------------------------------------------------------------
// ProcessFiring - Handle fire rate and hitscan for a player
//-----------------------------------------------------------------------------
void GameServer::ProcessFiring(PlayerData& shooter, uint8_t shooterId)
{
    // Block firing while reloading or inspecting
    if (shooter.state.stateFlags & (NetStateFlags::IS_RELOADING | NetStateFlags::IS_INSPECTING))
    {
        shooter.fireTimer = 0.0;
        return;
    }

    bool isFiring = (shooter.lastInput.buttons & InputButtons::FIRE) != 0;

    if (!isFiring)
    {
        shooter.fireTimer = 0.0;
        return;
    }

    // Determine RPM and damage based on team
    double rpm = (shooter.teamId == PlayerTeam::RED) ? RED_RPM : BLUE_RPM;
    uint8_t damage = (shooter.teamId == PlayerTeam::RED) ? RED_DAMAGE : BLUE_DAMAGE;
    double fireInterval = 60.0 / rpm;

    // First shot fires immediately; subsequent shots at RPM interval
    bool shouldFire = false;
    if (shooter.fireTimer <= 0.0)
    {
        // First press — fire immediately
        shouldFire = true;
        shooter.fireTimer = fireInterval;
    }
    else
    {
        shooter.fireTimer -= TICK_DURATION;
        if (shooter.fireTimer <= 0.0)
        {
            shouldFire = true;
            shooter.fireTimer += fireInterval;
        }
    }

    if (!shouldFire) return;

    // Check ammo
    if (shooter.ammo == 0)
    {
        // Auto-reload: handles empty-fire after interrupted reload, or held FIRE on empty mag
        if (shooter.ammoReserve > 0 && shooter.reloadTimer <= 0.0) {
            shooter.reloadTimer = WeaponConfig::RELOAD_OUT_OF_AMMO_DURATION;
            shooter.state.stateFlags |= NetStateFlags::IS_RELOADING;
            shooter.state.stateFlags |= NetStateFlags::IS_RELOAD_EMPTY;
        }
        return;
    }

    // Consume ammo and increment fire counter
    shooter.ammo--;
    shooter.state.ammo = shooter.ammo;
    shooter.fireCounter++;

    // Auto-reload IMMEDIATELY when last bullet just fired
    if (shooter.ammo == 0 && shooter.ammoReserve > 0 && shooter.reloadTimer <= 0.0) {
        shooter.reloadTimer = WeaponConfig::RELOAD_OUT_OF_AMMO_DURATION;
        shooter.state.stateFlags |= NetStateFlags::IS_RELOADING;
        shooter.state.stateFlags |= NetStateFlags::IS_RELOAD_EMPTY;
    }

    // Cast ray from eye position
    Float3 eyePos = {
        shooter.state.position.x,
        shooter.state.position.y + 1.5f, // eye height (match client camera)
        shooter.state.position.z
    };
    Float3 rayDir = ServerRaycast::DirectionFromYawPitch(
        shooter.state.yaw, shooter.state.pitch);

    // Test against all other alive players (no friendly fire)
    // Also test against world geometry — player hit only counts if closer than world
    uint8_t hitId = 0xFF;
    float hitDist = 0.0f;
    float worldDist = RaycastWorld(eyePos, rayDir);
    if (RaycastPlayers(eyePos, rayDir, shooterId, shooter.teamId, hitId, hitDist)
        && hitDist < worldDist)
    {
        // Apply damage
        auto hitIt = m_Players.find(hitId);
        if (hitIt != m_Players.end())
        {
            PlayerData& target = hitIt->second;
            if (target.health > damage)
            {
                target.health -= damage;
            }
            else
            {
                target.health = 0;
                target.state.stateFlags |= NetStateFlags::IS_DEAD;
                target.respawnTimer = RESPAWN_TIME;
                target.state.velocity = { 0.0f, 0.0f, 0.0f };
                SLOG_INFO("Player %u killed Player %u", shooterId, hitId);
            }

            // Record hit for attacker's hit marker
            shooter.state.hitByPlayerId = hitId;
        }
    }
}

//-----------------------------------------------------------------------------
// RaycastPlayers - Test ray against all alive enemy players
//
// Returns true if any enemy player is hit. outHitId/outDist are set to the
// closest hit. Players on excludeTeam are skipped (no friendly fire).
//-----------------------------------------------------------------------------
bool GameServer::RaycastPlayers(const Float3& origin, const Float3& dir,
                                 uint8_t excludeId, uint8_t excludeTeam,
                                 uint8_t& outHitId, float& outDist)
{
    bool anyHit = false;
    float closestT = FLT_MAX;

    for (const auto& [id, player] : m_Players)
    {
        // Skip self
        if (id == excludeId) continue;
        // Skip same team (no friendly fire)
        if (player.teamId == excludeTeam) continue;
        // Skip dead
        if (player.state.stateFlags & NetStateFlags::IS_DEAD) continue;

        float t = 0.0f;
        if (ServerRaycast::RayCapsule(origin, dir,
            player.state.position, PLAYER_HEIGHT, CAPSULE_RADIUS, t))
        {
            if (t < closestT)
            {
                closestT = t;
                outHitId = id;
                anyHit = true;
            }
        }
    }

    if (anyHit)
        outDist = closestT;

    return anyHit;
}

//-----------------------------------------------------------------------------
// RaycastWorld - Test ray against all map AABB colliders
//
// Returns the closest hit distance, or a very large value if no hit.
//-----------------------------------------------------------------------------
float GameServer::RaycastWorld(const Float3& origin, const Float3& dir)
{
    float closest = FLT_MAX;

    for (const auto& col : m_Colliders)
    {
        float t = 0.0f;
        if (ServerRaycast::RayAABB(origin, dir,
            col.aabb.min, col.aabb.max, t))
        {
            if (t < closest)
                closest = t;
        }
    }

    return closest;
}
