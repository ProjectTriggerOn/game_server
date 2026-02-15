#pragma once
//=============================================================================
// enet_server_network.h
//
// ENet-based server network implementation.
// Listens for client connections and exchanges InputCmd/Snapshot packets.
// Supports multiple concurrent clients with per-player routing.
//=============================================================================

#include "i_network.h"
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

    //-------------------------------------------------------------------------
    // Multi-player: tagged input, per-peer send, player events
    //-------------------------------------------------------------------------
    bool ReceiveTaggedInput(TaggedInput& out);
    void SendSnapshotToPlayer(uint8_t playerId, const Snapshot& snapshot);
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
};
