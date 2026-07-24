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

//-----------------------------------------------------------------------------
// Tagged input: InputCmd associated with the player who sent it
//-----------------------------------------------------------------------------
struct TaggedInput {
    InputCmd cmd;
    uint8_t playerId;
};

//-----------------------------------------------------------------------------
// Player connect/disconnect event
//-----------------------------------------------------------------------------
struct PlayerEvent {
    uint8_t playerId;
    bool connected;
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

    // Peer <-> PlayerId mapping
    std::unordered_map<ENetPeer*, uint8_t> m_PeerToPlayerId;
    std::unordered_map<uint8_t, ENetPeer*> m_PlayerIdToPeer;

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
