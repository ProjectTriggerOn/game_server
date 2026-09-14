#pragma once
//=============================================================================
// enet_server_network.h
//
// ENet-based server network implementation.
// Listens for client connections and exchanges InputCmd/Snapshot packets.
// Supports multiple concurrent clients with per-player routing.
//=============================================================================

#include "i_network.h"
#include "net_packet.h"
#include "net_limits.h"
#include <enet/enet.h>
#include <queue>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>

//-----------------------------------------------------------------------------
// Tagged input: InputCmd associated with the player who sent it
//-----------------------------------------------------------------------------
struct TaggedInput {
    InputCmd cmd;
    uint8_t playerId;
};

//-----------------------------------------------------------------------------
// What happened to a peer.
//
// CONNECTED and JOINED are deliberately separate. A client opens its ENet peer
// once when the process starts and holds it until the process exits; that says
// nothing about whether a person is playing. JOINED is the player themselves
// arriving (PacketType::JOIN_REQUEST), and it is the only one that puts them in
// the world. Collapsing the two is what let two clients idling on the title
// screen reach MatchConfig::MIN_PLAYERS and run a match to its end with an
// empty map.
//-----------------------------------------------------------------------------
enum class PlayerEventType : uint8_t {
    CONNECTED,      // transport peer established - a spectator, not a player
    JOINED,         // asked to enter the match room
    DISCONNECTED,   // peer gone (whether or not it ever joined)
};

struct PlayerEvent {
    uint8_t playerId;
    PlayerEventType type;
};

class ENetServerNetwork : public INetwork
{
public:
    ENetServerNetwork();
    ~ENetServerNetwork() override;

    //-------------------------------------------------------------------------
    // Configuration (call before Initialize)
    //-------------------------------------------------------------------------
    void SetPort(uint16_t port);

    //-------------------------------------------------------------------------
    // INetwork interface
    //-------------------------------------------------------------------------
    void Initialize() override;
    void Finalize() override;

    // Client -> Server (Upstream)
    void SendInputCmd(const InputCmd& cmd) override;       // No-op on server
    bool ReceiveInputCmd(InputCmd& outCmd) override;
    size_t GetInputQueueSize() const override;

    // Server -> Client (Downstream)
    void SendSnapshot(const Snapshot& snapshot) override;   // Broadcasts to all peers
    bool ReceiveSnapshot(Snapshot& outSnapshot) override;   // No-op on server
    size_t GetSnapshotQueueSize() const override;

    // Statistics
    uint32_t GetTotalInputsSent() const override { return 0; }
    uint32_t GetTotalSnapshotsSent() const override { return m_TotalSnapshotsSent; }

    //-------------------------------------------------------------------------
    // ENet-specific
    //-------------------------------------------------------------------------
    void PollEvents();

    // Did Initialize() actually take the port? INetwork::Initialize is void and
    // shared with the client, so the one failure that matters is reported here
    // instead: without this check a server whose bind lost to a stale container
    // or a second launch ran its whole loop at 32Hz with no socket, printing
    // "Running." and "Clients: 0" forever. It looked healthy from the console
    // and from a supervisor (exit 0) while being unreachable.
    bool IsListening() const { return m_pServer != nullptr; }

    bool HasConnectedClient() const { return !m_ConnectedPeers.empty(); }
    size_t GetConnectedClientCount() const { return m_ConnectedPeers.size(); }

    // Observability: log per-peer receive counts + input-queue high-water for the
    // current window, then reset them. Call once per status report (~1s).
    void ReportRecvStatsAndReset();

    // L1: refill every peer's inbound token bucket. Call exactly once per server tick.
    void RefillRecvBudgets();

    //-------------------------------------------------------------------------
    // Multi-player: tagged input, per-peer send, player events
    //-------------------------------------------------------------------------
    bool ReceiveTaggedInput(TaggedInput& out);
    void SendSnapshotToPlayer(uint8_t playerId, const Snapshot& snapshot);
    void SendMapInfoToPlayer(uint8_t playerId, const MapInfo& info);
    bool PollPlayerEvent(PlayerEvent& out);
    std::vector<uint8_t> GetConnectedPlayerIds() const;

private:
    uint8_t AllocatePlayerId();

private:
    ENetHost* m_pServer;
    std::vector<ENetPeer*> m_ConnectedPeers;

    uint16_t m_Port;

    // Peer <-> PlayerId mapping. A playerId is reserved on CONNECT so the peer
    // has a stable name in the logs and can be sent MAP_INFO; being in here is
    // NOT the same as being in the match (see m_JoinedPeers).
    std::unordered_map<ENetPeer*, uint8_t> m_PeerToPlayerId;
    std::unordered_map<uint8_t, ENetPeer*> m_PlayerIdToPeer;

    // Peers that have sent JOIN_REQUEST. Gates the join at the source so a peer
    // repeating the packet produces exactly one JOINED event per connection;
    // GameServer::OnPlayerJoined guards the same thing against m_Players.
    std::unordered_set<ENetPeer*> m_JoinedPeers;

    // Incoming tagged input queue (filled by PollEvents, consumed by ReceiveTaggedInput)
    std::queue<TaggedInput> m_TaggedInputQueue;
    mutable std::mutex m_InputMutex;

    // Player connect/disconnect event queue
    std::queue<PlayerEvent> m_PlayerEventQueue;
    mutable std::mutex m_EventMutex;

    // Statistics
    uint32_t m_TotalSnapshotsSent;

    // Observability: per-peer received-packet counts + input-queue high-water for
    // the current report window (~1s). Diagnostic only — nothing is dropped here.
    // Single-threaded server, so these need no lock (see PollEvents / main loop).
    std::unordered_map<ENetPeer*, uint32_t> m_PeerRecvCount;
    size_t m_InputQueueHighWater;

    // L1: per-peer inbound token bucket. One token spent per received packet;
    // when empty the peer is over budget and the packet is dropped before parse.
    // Refilled from the server tick (RefillRecvBudgets). 'dropped' accumulates
    // per report window for the throttle warning.
    struct PeerBudget {
        float    tokens  = NetLimits::INPUT_BUCKET_DEPTH;  // start full (new peer not throttled)
        uint32_t dropped = 0;
    };
    std::unordered_map<ENetPeer*, PeerBudget> m_PeerBudget;
};
