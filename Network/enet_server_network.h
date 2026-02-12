#pragma once
//=============================================================================
// enet_server_network.h
//
// ENet-based server network implementation.
// Listens for client connections and exchanges InputCmd/Snapshot packets.
//=============================================================================

#include "i_network.h"
#include <enet/enet.h>
#include <queue>
#include <mutex>
#include <vector>

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

private:
    ENetHost* m_pServer;
    std::vector<ENetPeer*> m_ConnectedPeers;

    uint16_t m_Port;

    // Incoming InputCmd queue (filled by PollEvents, consumed by ReceiveInputCmd)
    std::queue<InputCmd> m_InputQueue;
    mutable std::mutex m_InputMutex;

    // Statistics
    uint32_t m_TotalSnapshotsSent;
};
