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
#include <cstring>

using namespace RecoilMath;  // recoil_math.h helpers (RecoilAdvance, RecoilTotalOffsets, ...)

// Per-shot lag-compensation log (verification aid — flip off once verified;
// at 600-800 RPM this is ~10-13 lines/s per firing player)
static constexpr bool LAGCOMP_LOG = true;

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

void GameServer::Initialize(ENetServerNetwork* pNetwork, const char* mapPath)
{
    m_pNetwork = pNetwork;
    m_Accumulator = 0.0;
    m_ServerTime = 0.0;
    m_CurrentTick = 0;
    m_Players.clear();

    // Fresh match, but NOT a live one: an empty server sits in WAITING until
    // MatchConfig::MIN_PLAYERS connect (see UpdateMatchFlow).
    m_RedScore = 0;
    m_BlueScore = 0;
    m_WinningTeam = MatchTeam::NONE;
    m_MatchTimeRemaining = MatchConfig::MATCH_DURATION;
    m_CountdownRemaining = 0.0;
    m_MatchState = MatchState::WAITING;
    m_KillSeq = 0;
    for (auto& e : m_RecentKills) e = {};

    // Register colliders from the runtime .map (must match the client's file).
    m_Colliders.clear();
    mapio::MapData md;
    bool loaded = (mapPath != nullptr) && mapio::Read(mapPath, md);
    if (loaded)
    {
        for (const auto& a : md.colliders)
        {
            ServerCollider sc;
            sc.aabb = { { a.minX, a.minY, a.minZ }, { a.maxX, a.maxY, a.maxZ } };
            sc.isGround = (a.isGround != 0);
            m_Colliders.push_back(sc);
        }
        std::memcpy(m_MapInfo.name, md.name, sizeof(m_MapInfo.name));
        m_MapInfo.checksum = mapio::CollisionChecksum(md);
        SLOG_INFO("Loaded map '%s': %zu colliders, checksum=%08x",
                  md.name, m_Colliders.size(), m_MapInfo.checksum);
    }
    else
    {
        // Dev fallback: compiled map. Its checksum will NOT match a client's
        // default.map (no spawns section) — the handshake will flag this, which
        // is the correct signal that the server is missing its --map file.
        SLOG_INFO("WARNING: --map missing/unreadable ('%s'); using compiled MAP_COLLIDERS",
                  mapPath ? mapPath : "(none)");
        mapio::MapData fb;
        for (int i = 0; i < MAP_COLLIDER_COUNT; i++)
        {
            const MapColliderDef& def = MAP_COLLIDERS[i];
            ServerCollider sc;
            sc.aabb = { { def.minX, def.minY, def.minZ }, { def.maxX, def.maxY, def.maxZ } };
            sc.isGround = def.isGround;
            m_Colliders.push_back(sc);
            fb.colliders.push_back(mapio::MapAABB{ def.minX, def.minY, def.minZ,
                                                   def.maxX, def.maxY, def.maxZ,
                                                   (uint8_t)(def.isGround ? 1 : 0), {0,0,0} });
        }
        std::strncpy(m_MapInfo.name, "compiled", sizeof(m_MapInfo.name) - 1);
        m_MapInfo.checksum = mapio::CollisionChecksum(fb);
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
    // Joining a FINISHED match is a rematch request.
    //
    // Normally the room has already emptied by now - a client leaves the moment
    // its match ends, so the result screen costs no slot - and the player-count
    // gate in UpdateMatchFlow has rearmed us. This covers the cases where it has
    // not: a client that leaves and rejoins within a single tick (
    // ProcessPlayerEvents drains the WHOLE event queue before the flow runs, so
    // the count never dips and the joiner would land straight back on the
    // result screen it just left - observed live), and any peer still frozen in
    // ENDED because it went away without saying so and its timeout has not
    // fired yet.
    //
    // Runs before the new player is inserted, so their state below is already
    // the fresh one and ResetMatch does not have to touch them.
    if (m_MatchState == MatchState::ENDED)
    {
        SLOG_INFO("Player %u joined a finished match - rearming", playerId);
        ResetMatch();
    }

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
    data.state.kills = 0;
    data.state.deaths = 0;
    data.state.ammo = WeaponConfig::MAG_SIZE;
    data.state.ammoReserve = WeaponConfig::MAX_RESERVE;
    data.lastInput = {};
    data.reloadTimer = 0.0;
    data.respawnTimer = 0.0;
    data.fireTimer = 0.0;

    m_Players[playerId] = data;

    // Tell the freshly-connected client which map we're simulating.
    m_pNetwork->SendMapInfoToPlayer(playerId, m_MapInfo);

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

    // 1b. Refill per-peer inbound rate-limit budgets for this tick (L1).
    m_pNetwork->RefillRecvBudgets();

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

    // 5. Process firing and combat for all players. Combat runs only while the
    //    match is live (IsMatchLive): WAITING, COUNTDOWN and ENDED all freeze
    //    firing and respawns. Movement is frozen alongside it in
    //    SimulatePhysics, so players really do hold their positions.
    // Clear every player's hit marker once, up front. A hit is written to the
    // VICTIM's snapshot below (see ProcessFiring), so the clear must run for
    // all players before any shots resolve — otherwise a later-iterated victim
    // (unordered_map order) would wipe the write before broadcast.
    for (auto& [id, player] : m_Players)
        player.state.hitByPlayerId = 0xFF;

    for (auto& [id, player] : m_Players)
    {
        if (IsMatchLive())
        {
            // Respawn timer
            if (player.state.stateFlags & NetStateFlags::IS_DEAD)
            {
                player.respawnTimer -= TICK_DURATION;
                if (player.respawnTimer <= 0.0)
                {
                    // Respawn
                    player.state.health = MAX_HEALTH;
                    player.state.stateFlags &= ~NetStateFlags::IS_DEAD;
                    player.state.stateFlags |= NetStateFlags::IS_GROUNDED;
                    player.state.position = GetSpawnPosition(id, player.teamId);
                    player.state.velocity = { 0.0f, 0.0f, 0.0f };
                    player.respawnTimer = 0.0;
                    player.state.ammo = WeaponConfig::MAG_SIZE;
                    player.state.ammoReserve = WeaponConfig::MAX_RESERVE;
                    // Reset reload state — a player killed mid-reload should wake up clean,
                    // not stuck with phantom IS_RELOADING blocking fire until the old timer expires.
                    player.reloadTimer = 0.0;
                    player.state.stateFlags &= ~(NetStateFlags::IS_RELOADING | NetStateFlags::IS_RELOAD_EMPTY);
                    // Reset edge-detection so any held button across death/respawn isn't mis-detected.
                    player.prevButtons = 0;
                    // Recoil reset (spec §7): shotKick never decays, so a
                    // fresh life must start from a clean pool — otherwise the
                    // pre-death accumulation permanently skews the WYSIWYG
                    // ray direction. same for the lastShot latch.
                    player.recoil = RecoilMath::RecoilState{};
                    player.lastShotResult = LastShotResult::MISS;
                    player.lastShotSeqMod = 0;
                    SLOG_INFO("Player %u respawned", id);
                }
            }
            else
            {
                ProcessFiring(player, id);
            }
        }

        // Recoil decay + broadcast (spec §4.3): one tick's worth of punch/
        // bloom recovery with the same exponential factor the client uses
        // per frame. Runs AFTER ProcessFiring, so a shot fired this tick
        // gets its punch applied and then one tick of decay — matching the
        // client's per-frame accumulate-then-decay order.
        RecoilAdvance(player.recoil, player.teamId, player.state.fireCounter,
                      /*ads=*/false, /*newlyFired=*/false,
                      static_cast<float>(TICK_DURATION), m_ServerTime);
        player.state.punchPitch     = player.recoil.punchPitch;
        player.state.punchYaw       = player.recoil.punchYaw;
        player.state.shotKickPitch  = player.recoil.shotKickPitch;
        // lastShotResult/lastShotSeqMod live on Snapshot (not NetPlayerState);
        // BroadcastSnapshots() writes them into the per-recipient header.
    }

    // 5. Update tick ID in all player states (and ack of last processed input),
    // and record lag-compensation history. Clients use lastProcessedInputTick
    // to look up the matching entry in their input-history ring buffer for
    // prediction reconciliation (RESIM). The history record captures exactly
    // the state BroadcastSnapshots() is about to send (incl. respawn teleports),
    // so a client-reported viewTick maps back to what that client rendered.
    for (auto& [id, player] : m_Players)
    {
        player.state.tickId = m_CurrentTick;
        player.state.lastProcessedInputTick = player.lastInput.tickId;

        PositionHistoryEntry& h =
            player.history[m_CurrentTick % PlayerData::POSITION_HISTORY_SIZE];
        h.tick = m_CurrentTick;
        h.position = player.state.position;
        h.alive = (player.state.stateFlags & NetStateFlags::IS_DEAD) == 0;
    }

    // 5c. Match flow: player-count gate, countdown, clock and win condition.
    UpdateMatchFlow();

    // 6. Broadcast per-player snapshots
    BroadcastSnapshots();
}

//-----------------------------------------------------------------------------
// UpdateMatchFlow - WAITING -> COUNTDOWN -> PLAYING -> ENDED
//
// A match never runs on an empty or solo server. Dropping below MIN_PLAYERS
// rearms from ANY phase, ENDED included: that is what lets a client leave a
// finished match and reconnect into a fresh one without restarting the process.
//-----------------------------------------------------------------------------
void GameServer::UpdateMatchFlow()
{
    const bool enoughPlayers = (m_Players.size() >= MatchConfig::MIN_PLAYERS);

    // Room emptied out mid-flow: throw away the partial match and wait again.
    // WAITING is already the rearmed state, so re-running ResetMatch there
    // would pointlessly re-teleport whoever is still connected every tick.
    if (!enoughPlayers && m_MatchState != MatchState::WAITING)
    {
        SLOG_INFO("Match abandoned (%zu/%zu players) - waiting",
            m_Players.size(), MatchConfig::MIN_PLAYERS);
        ResetMatch();
        return;
    }

    switch (m_MatchState)
    {
    case MatchState::WAITING:
        if (enoughPlayers)
        {
            m_MatchState = MatchState::COUNTDOWN;
            m_CountdownRemaining = MatchConfig::COUNTDOWN_DURATION;
            SLOG_INFO("Countdown started (%zu players)", m_Players.size());
        }
        break;

    case MatchState::COUNTDOWN:
        m_CountdownRemaining -= TICK_DURATION;
        if (m_CountdownRemaining <= 0.0)
        {
            // Rearm once more on the way in: the countdown is the last moment
            // a late joiner can arrive, and everyone must start the live match
            // from an identical clean state.
            ResetMatch();
            m_MatchState = MatchState::PLAYING;
            SLOG_INFO("Match started (%zu players)", m_Players.size());
        }
        break;

    case MatchState::PLAYING:
    {
        // Score limit OR time limit, whichever fires first. On end, latch the
        // winning team and freeze (see Tick step 5 and SimulatePhysics).
        m_MatchTimeRemaining -= TICK_DURATION;
        const bool scoreOut = (m_RedScore >= MatchConfig::SCORE_LIMIT) ||
                              (m_BlueScore >= MatchConfig::SCORE_LIMIT);
        const bool timeOut = (m_MatchTimeRemaining <= 0.0);
        if (scoreOut || timeOut)
        {
            if (m_MatchTimeRemaining < 0.0) m_MatchTimeRemaining = 0.0;
            m_MatchState = MatchState::ENDED;
            m_WinningTeam = (m_RedScore > m_BlueScore) ? PlayerTeam::RED
                          : (m_BlueScore > m_RedScore) ? PlayerTeam::BLUE
                          : MatchTeam::DRAW;
            // Drop latched intent flags so nobody freezes mid-fire / mid-ADS /
            // mid-reload for the whole result screen. ResetMatch does this for
            // the WAITING path by rebuilding stateFlags outright; ENDED keeps
            // the final positions, so it has to clear them in place.
            for (auto& [id, p] : m_Players)
            {
                p.state.stateFlags &= ~(NetStateFlags::IS_FIRING |
                                        NetStateFlags::IS_ADS |
                                        NetStateFlags::IS_RELOADING |
                                        NetStateFlags::IS_RELOAD_EMPTY);
                p.reloadTimer = 0.0;
            }
            SLOG_INFO("Match ended: RED %u BLUE %u winner=%u",
                m_RedScore, m_BlueScore, m_WinningTeam);
        }
        break;
    }

    case MatchState::ENDED:
    default:
        // Frozen on the result screen. Clients leave via their own NEXT MATCH
        // (disconnect + reconnect); the player-count gate above rearms us.
        break;
    }
}

//-----------------------------------------------------------------------------
// ResetMatch - rearm a fresh match and drop back to WAITING
//
// Clears match-wide score/clock/kill-feed AND every connected player's life
// state, so a rematch cannot inherit health, ammo, K/D or recoil from the
// previous round. Mirrors OnPlayerConnected's spawn block.
//-----------------------------------------------------------------------------
void GameServer::ResetMatch()
{
    m_RedScore = 0;
    m_BlueScore = 0;
    m_WinningTeam = MatchTeam::NONE;
    m_MatchTimeRemaining = MatchConfig::MATCH_DURATION;
    m_CountdownRemaining = 0.0;
    m_MatchState = MatchState::WAITING;
    m_KillSeq = 0;
    for (auto& e : m_RecentKills) e = {};

    for (auto& [id, player] : m_Players)
    {
        player.state.position = GetSpawnPosition(id, player.teamId);
        player.state.velocity = { 0.0f, 0.0f, 0.0f };
        player.state.stateFlags = NetStateFlags::IS_GROUNDED;
        player.state.health = MAX_HEALTH;
        player.state.hitByPlayerId = 0xFF;
        player.state.fireCounter = 0;
        player.state.kills = 0;
        player.state.deaths = 0;
        player.state.ammo = WeaponConfig::MAG_SIZE;
        player.state.ammoReserve = WeaponConfig::MAX_RESERVE;
        player.state.punchPitch = 0.0f;
        player.state.punchYaw = 0.0f;
        player.state.shotKickPitch = 0.0f;
        player.reloadTimer = 0.0;
        player.respawnTimer = 0.0;
        player.fireTimer = 0.0;
        player.prevButtons = 0;
        player.recoil = RecoilMath::RecoilState{};
        player.lastShotResult = LastShotResult::MISS;
        player.lastShotSeqMod = 0;
        // Deliberately NOT cleared: lastInput (the client keeps aiming through
        // the freeze, and yaw/pitch still drive the broadcast) and history
        // (lag-comp entries are keyed by tick and age out on their own; the
        // teleport guard already refuses to lerp across a respawn jump).
    }
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

    // Dead players, and EVERYONE while the match is not live (WAITING /
    // COUNTDOWN / ENDED): only the camera updates, every action is dropped.
    // Firing is already gated in Tick, but the intent flags are not: without
    // this a player holding the trigger through the countdown would broadcast
    // IS_FIRING and play a fire animation on every other client.
    if ((player.state.stateFlags & NetStateFlags::IS_DEAD) || !IsMatchLive())
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
        if (player.reloadTimer <= 0.0 && player.state.ammo < WeaponConfig::MAG_SIZE && player.state.ammoReserve > 0)
        {
            if (player.state.ammo == 0) {
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
            int needed = WeaponConfig::MAG_SIZE - player.state.ammo;
            int refill = (player.state.ammoReserve >= needed) ? needed : player.state.ammoReserve;
            player.state.ammo += static_cast<uint8_t>(refill);
            player.state.ammoReserve -= static_cast<uint8_t>(refill);
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
    // Outside PLAYING nobody moves: WAITING and COUNTDOWN hold everyone on
    // their spawn, ENDED holds the final positions. Physics still RUNS while
    // frozen (gravity + collision) so a player can't hang in the air over a
    // pending fall; only the input-driven part is suppressed.
    const bool frozen = !IsMatchLive();
    for (auto& [id, player] : m_Players)
    {
        // Skip dead players
        if (player.state.stateFlags & NetStateFlags::IS_DEAD)
            continue;
        SimulatePlayerPhysics(player, frozen);
    }
}

//-----------------------------------------------------------------------------
// SimulatePlayerPhysics - CS:GO / Valorant style movement for one player
//-----------------------------------------------------------------------------
void GameServer::SimulatePlayerPhysics(PlayerData& player, bool frozen)
{
    const float dt = static_cast<float>(TICK_DURATION);

    // From PhysicsConfig (net_common.h) — shared with the client's prediction
    // and the mock, and the divisor for the recoil spread's movement term, so
    // a local copy here could desync the cone.
    constexpr float MAX_WALK_SPEED = PhysicsConfig::MAX_WALK_SPEED;
    constexpr float MAX_RUN_SPEED  = PhysicsConfig::MAX_RUN_SPEED;
    constexpr float GROUND_ACCEL   = 50.0f;
    constexpr float AIR_ACCEL      = 2.0f;
    constexpr float GRAVITY        = 20.0f;
    constexpr float JUMP_VELOCITY  = 8.0f;

    NetPlayerState& state = player.state;
    // A frozen phase drops movement intent but keeps the look angles, which the
    // client still owns and broadcasts. Substituting a neutered command here
    // (rather than branching through the body) keeps gravity, collision and the
    // grounded/jumping flag transitions on exactly one code path.
    InputCmd frozenInput{};
    if (frozen)
    {
        frozenInput.tickId = player.lastInput.tickId;
        frozenInput.yaw = player.lastInput.yaw;
        frozenInput.pitch = player.lastInput.pitch;
        state.velocity.x = 0.0f;
        state.velocity.z = 0.0f;
    }
    const InputCmd& input = frozen ? frozenInput : player.lastInput;

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
        // Recoil shot result (hitmarker, spec §3.2): the RECIPIENT's own latest
        // shot. NetPlayerState carries no lastShot* (wire layout fixed in Task
        // 1.1), so only the local shot is signaled — exactly what the
        // hitmarker needs. Remote players' results are not broadcast.
        snapshot.lastShotResult = myData.lastShotResult;
        snapshot.lastShotSeqMod = myData.lastShotSeqMod;

        // Global match / scoring state (identical for every player's snapshot)
        snapshot.matchState         = m_MatchState;
        snapshot.winningTeam        = m_WinningTeam;
        snapshot.redScore           = m_RedScore;
        snapshot.blueScore          = m_BlueScore;
        // COUNTDOWN reuses this field as its own 3..0 clock (see MatchState in
        // net_common.h); the client keys off matchState to read it correctly.
        snapshot.matchTimeRemaining = static_cast<float>(
            (m_MatchState == MatchState::COUNTDOWN) ? m_CountdownRemaining
                                                    : m_MatchTimeRemaining);
        snapshot.latestKillSeq      = m_KillSeq;
        for (int k = 0; k < KILL_FEED_SIZE; ++k)
            snapshot.recentKills[k] = m_RecentKills[k];

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
    if (shooter.state.ammo == 0)
    {
        // Auto-reload: handles empty-fire after interrupted reload, or held FIRE on empty mag
        if (shooter.state.ammoReserve > 0 && shooter.reloadTimer <= 0.0) {
            shooter.reloadTimer = WeaponConfig::RELOAD_OUT_OF_AMMO_DURATION;
            shooter.state.stateFlags |= NetStateFlags::IS_RELOADING;
            shooter.state.stateFlags |= NetStateFlags::IS_RELOAD_EMPTY;
        }
        return;
    }

    // Consume ammo and increment fire counter
    shooter.state.ammo--;
    shooter.state.fireCounter++;

    // Auto-reload IMMEDIATELY when last bullet just fired
    if (shooter.state.ammo == 0 && shooter.state.ammoReserve > 0 && shooter.reloadTimer <= 0.0) {
        shooter.reloadTimer = WeaponConfig::RELOAD_OUT_OF_AMMO_DURATION;
        shooter.state.stateFlags |= NetStateFlags::IS_RELOADING;
        shooter.state.stateFlags |= NetStateFlags::IS_RELOAD_EMPTY;
    }

    // Advance recoil for THIS shot (spec §4.3): punch += envelope·adsScale,
    // shotKick += tiny real kick, bloom grows. fireCounter was already
    // incremented, so (fireCounter-1) indexes the shot just taken. dt=0:
    // decay is Tick()'s job (runs right after, before the snapshot).
    const bool ads = (shooter.state.stateFlags & NetStateFlags::IS_ADS) != 0;
    RecoilAdvance(shooter.recoil, shooter.teamId, shooter.state.fireCounter,
                  ads, /*newlyFired=*/true, /*dt=*/0.0f, m_ServerTime);

    // Cast ray from eye position
    Float3 eyePos = {
        shooter.state.position.x,
        shooter.state.position.y + 1.5f, // eye height (match client camera)
        shooter.state.position.z
    };
    // WYSIWYG (spec §2): bullets follow the punched view — the same punch
    // the client renders — plus this shot's deterministic bloom-cone offset
    // (fireCounter is the seed; no RNG on either side).
    float dPitch = 0.0f, dYaw = 0.0f;
    RecoilTotalOffsets(shooter.recoil, dPitch, dYaw);
    // Movement widens the cone (spec §1.1: HIP ×1.5, ADS ×1.3). Derived from
    // the authoritative velocity, which is also what the client's crosshair
    // reads back from the snapshot — both sides land on the same cone.
    const float moveFactor = MoveFactorFromVelocity(shooter.state.velocity.x,
                                                    shooter.state.velocity.z);
    const float spread = RecoilSpreadRadians(shooter.teamId, ads,
                                             shooter.recoil.bloomDeg, moveFactor);
    float coneDP = 0.0f, coneDY = 0.0f;
    RecoilConeOffset(spread, shooter.state.fireCounter, coneDP, coneDY);
    Float3 rayDir = ServerRaycast::DirectionFromYawPitch(
        shooter.state.yaw + dYaw + coneDY,
        shooter.state.pitch + dPitch + coneDP);

    // Lag compensation: clamp the client-reported view tick into the allowed
    // rewind window (anti-cheat bound — a hacked client claiming an ancient
    // tick gets the oldest allowed time, a future tick gets "now").
    uint32_t viewTick = shooter.lastInput.viewTick;
    float viewFrac = shooter.lastInput.viewTickFrac;
    if (viewTick != 0)
    {
        const uint32_t minTick = (m_CurrentTick > LagCompConfig::MAX_REWIND_TICKS)
            ? m_CurrentTick - LagCompConfig::MAX_REWIND_TICKS : 1u;
        if (viewTick < minTick) viewTick = minTick;
        if (viewTick > m_CurrentTick)
        {
            viewTick = m_CurrentTick;
            viewFrac = 0.0f;  // never extrapolate forward server-side
        }
        if (!(viewFrac >= 0.0f && viewFrac < 1.0f)) viewFrac = 0.0f;  // NaN-safe
    }

    // Test against all other alive players (no friendly fire), with target
    // hitboxes rewound to the time the shooter actually SAW them.
    // Also test against world geometry — player hit only counts if closer than
    // world (static geometry needs no rewind).
    uint8_t hitId = 0xFF;
    float hitDist = 0.0f;
    float worldDist = RaycastWorld(eyePos, rayDir);
    bool didHit = RaycastPlayers(eyePos, rayDir, shooterId, shooter.teamId,
                                 viewTick, viewFrac, hitId, hitDist)
                  && hitDist < worldDist;

    if (LAGCOMP_LOG && viewTick != 0)
    {
        const double rewindTicks =
            static_cast<double>(m_CurrentTick - viewTick) - viewFrac;
        SLOG_INFO("[LAGCOMP] shooter=%u view=%u+%.2f rewind=%.1ft (%.0fms) hit=%s id=%u dist=%.2f",
                  shooterId, viewTick, viewFrac,
                  rewindTicks, rewindTicks * TICK_DURATION * 1000.0,
                  didHit ? "yes" : "no",
                  didHit ? hitId : 0xFF,
                  didHit ? hitDist : 0.0f);
    }

    if (didHit)
    {
        // Apply damage
        auto hitIt = m_Players.find(hitId);
        if (hitIt != m_Players.end())
        {
            PlayerData& target = hitIt->second;
            if (target.state.health > damage)
            {
                target.state.health -= damage;
                shooter.lastShotResult = LastShotResult::HIT_PLAYER;
            }
            else
            {
                target.state.health = 0;
                target.state.stateFlags |= NetStateFlags::IS_DEAD;
                target.respawnTimer = RESPAWN_TIME;
                target.state.velocity = { 0.0f, 0.0f, 0.0f };
                SLOG_INFO("Player %u killed Player %u", shooterId, hitId);

                // --- scoring (friendly fire is off, so killer/victim are
                //     always on opposing teams) -------------------------------
                shooter.state.kills++;
                target.state.deaths++;
                if (shooter.teamId == PlayerTeam::RED) m_RedScore++;
                else                                   m_BlueScore++;

                // kill-feed ring write: slot = seq % size; latestKillSeq = m_KillSeq
                KillFeedEntry& kf = m_RecentKills[m_KillSeq % KILL_FEED_SIZE];
                kf.killerId   = shooterId;
                kf.victimId   = hitId;
                kf.killerTeam = shooter.teamId;
                kf.victimTeam = target.teamId;
                m_KillSeq++;
                shooter.lastShotResult = LastShotResult::HIT_KILL;
            }

            // Mark the VICTIM's snapshot with the attacker's id (hit marker /
            // damage-flash semantics: 0xFF = no hit, else the attacker id per
            // net_common.h). The victim reads their own snapshot and flashes.
            target.state.hitByPlayerId = shooterId;
        }
    }

    // Record the shot result for the attacker's own snapshot (hitmarker,
    // spec §3.2). A tick resolves at most one shot per player; the client
    // dedups stale snapshots by shot seq. HIT_PLAYER/HIT_KILL were already
    // written in the hit branches above — here we only stamp MISS on a clean
    // whiff, so the kill marker is never clobbered.
    if (!didHit)
        shooter.lastShotResult = LastShotResult::MISS;
    shooter.lastShotSeqMod = static_cast<uint8_t>(shooter.state.fireCounter & 0xFFu);
}

//-----------------------------------------------------------------------------
// GetRewoundPosition - Lag compensation: where was this player at the
// (fractional) tick the shooter was viewing?
//
// History for tick N is recorded at the END of Tick() N, so during the
// current tick's ProcessFiring the buffer holds ticks <= m_CurrentTick - 1
// and the current tick's post-physics position is the live state. All
// fallbacks therefore return the live position — which is also the correct
// "no compensation" behavior.
//-----------------------------------------------------------------------------
Float3 GameServer::GetRewoundPosition(const PlayerData& target,
                                      uint32_t viewTick, float viewFrac) const
{
    // 0 = "no data" sentinel; >= current tick = "no lag" → live position
    if (viewTick == 0 || viewTick >= m_CurrentTick)
        return target.state.position;

    const PositionHistoryEntry& a =
        target.history[viewTick % PlayerData::POSITION_HISTORY_SIZE];
    if (a.tick != viewTick)  // older than the buffer / never written
        return target.state.position;

    const Float3 posA = a.position;
    if (viewFrac <= 0.0f) return posA;

    // Endpoint B = entry for viewTick+1, or the live position when viewTick+1
    // is the in-flight current tick (physics ran, history not yet written).
    Float3 posB;
    bool haveB = false;
    if (viewTick + 1 == m_CurrentTick)
    {
        posB = target.state.position;
        haveB = true;
    }
    else
    {
        const PositionHistoryEntry& b =
            target.history[(viewTick + 1) % PlayerData::POSITION_HISTORY_SIZE];
        if (b.tick == viewTick + 1 && b.alive)
        {
            posB = b.position;
            haveB = true;
        }
    }
    if (!haveB) return posA;

    // Don't lerp across a respawn teleport — the midpoint never existed.
    const float dx = posB.x - posA.x;
    const float dy = posB.y - posA.y;
    const float dz = posB.z - posA.z;
    if (dx * dx + dy * dy + dz * dz >
        REWIND_TELEPORT_GUARD * REWIND_TELEPORT_GUARD)
        return posA;

    return { posA.x + dx * viewFrac,
             posA.y + dy * viewFrac,
             posA.z + dz * viewFrac };
}

//-----------------------------------------------------------------------------
// RaycastPlayers - Test ray against all alive enemy players
//
// Returns true if any enemy player is hit. outHitId/outDist are set to the
// closest hit. Players on excludeTeam are skipped (no friendly fire).
//
// Lag compensation: each target's capsule is rewound to viewTick+viewFrac
// (the time the shooter was viewing) before the ray test. viewTick == 0
// disables rewind (capsules at live positions). Damage is still applied to
// the live player by the caller.
//-----------------------------------------------------------------------------
bool GameServer::RaycastPlayers(const Float3& origin, const Float3& dir,
                                 uint8_t excludeId, uint8_t excludeTeam,
                                 uint32_t viewTick, float viewFrac,
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

        // Skip players who were dead at the time the shooter saw (their
        // rendered model was dead/hidden then — no shooting "ghosts")
        if (viewTick != 0 && viewTick < m_CurrentTick)
        {
            const PositionHistoryEntry& h =
                player.history[viewTick % PlayerData::POSITION_HISTORY_SIZE];
            if (h.tick == viewTick && !h.alive) continue;
        }

        const Float3 capsulePos = GetRewoundPosition(player, viewTick, viewFrac);

        float t = 0.0f;
        if (ServerRaycast::RayCapsule(origin, dir,
            capsulePos, PLAYER_HEIGHT, CAPSULE_RADIUS, t))
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
