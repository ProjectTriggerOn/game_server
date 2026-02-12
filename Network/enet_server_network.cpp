//=============================================================================
// enet_server_network.cpp
//
// ENet-based server network implementation.
//=============================================================================

#include "enet_server_network.h"
#include "net_packet.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

ENetServerNetwork::ENetServerNetwork()
    : m_pServer(nullptr)
    , m_Port(7777)
    , m_TotalSnapshotsSent(0)
{
}

ENetServerNetwork::~ENetServerNetwork()
{
    Finalize();
}

void ENetServerNetwork::SetPort(uint16_t port)
{
    m_Port = port;
}

void ENetServerNetwork::Initialize()
{
    if (enet_initialize() != 0)
    {
        printf("[Server] Failed to initialize ENet.\n");
        return;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = m_Port;

    // Create server host: max 4 clients, 2 channels
    m_pServer = enet_host_create(&address, 4, 2, 0, 0);
    if (!m_pServer)
    {
        printf("[Server] Failed to create ENet server host on port %u.\n", m_Port);
        enet_deinitialize();
        return;
    }

    printf("[Server] Listening on port %u.\n", m_Port);
    m_TotalSnapshotsSent = 0;
}

void ENetServerNetwork::Finalize()
{
    // Disconnect all peers
    for (ENetPeer* peer : m_ConnectedPeers)
    {
        enet_peer_disconnect_now(peer, 0);
    }
    m_ConnectedPeers.clear();

    if (m_pServer)
    {
        enet_host_destroy(m_pServer);
        m_pServer = nullptr;
    }

    enet_deinitialize();
    printf("[Server] Shut down.\n");
}

//-----------------------------------------------------------------------------
// PollEvents - Must be called every iteration of the server loop
//-----------------------------------------------------------------------------
void ENetServerNetwork::PollEvents()
{
    if (!m_pServer) return;

    ENetEvent event;
    while (enet_host_service(m_pServer, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
        {
            m_ConnectedPeers.push_back(event.peer);
            printf("[Server] Client connected from %x:%u. Total clients: %zu\n",
                event.peer->address.host, event.peer->address.port, m_ConnectedPeers.size());
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        {
            m_ConnectedPeers.erase(
                std::remove(m_ConnectedPeers.begin(), m_ConnectedPeers.end(), event.peer),
                m_ConnectedPeers.end());
            printf("[Server] Client disconnected. Total clients: %zu\n", m_ConnectedPeers.size());
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE:
        {
            if (event.packet->dataLength >= 1)
            {
                PacketType type = static_cast<PacketType>(event.packet->data[0]);

                if (type == PacketType::INPUT_CMD &&
                    event.packet->dataLength == 1 + sizeof(InputCmd))
                {
                    InputCmd cmd;
                    std::memcpy(&cmd, event.packet->data + 1, sizeof(InputCmd));

                    std::lock_guard<std::mutex> lock(m_InputMutex);
                    m_InputQueue.push(cmd);
                }
            }
            enet_packet_destroy(event.packet);
            break;
        }

        default:
            break;
        }
    }
}

//-----------------------------------------------------------------------------
// SendSnapshot - Broadcast to all connected clients (unreliable)
//-----------------------------------------------------------------------------
void ENetServerNetwork::SendSnapshot(const Snapshot& snapshot)
{
    if (m_ConnectedPeers.empty()) return;

    uint8_t buffer[1 + sizeof(Snapshot)];
    buffer[0] = static_cast<uint8_t>(PacketType::SNAPSHOT);
    std::memcpy(buffer + 1, &snapshot, sizeof(Snapshot));

    for (ENetPeer* peer : m_ConnectedPeers)
    {
        ENetPacket* packet = enet_packet_create(
            buffer,
            sizeof(buffer),
            ENET_PACKET_FLAG_UNSEQUENCED
        );
        enet_peer_send(peer, 1, packet);
    }

    m_TotalSnapshotsSent++;
}

//-----------------------------------------------------------------------------
// ReceiveInputCmd - Pop from internal queue
//-----------------------------------------------------------------------------
bool ENetServerNetwork::ReceiveInputCmd(InputCmd& outCmd)
{
    std::lock_guard<std::mutex> lock(m_InputMutex);
    if (m_InputQueue.empty()) return false;

    outCmd = m_InputQueue.front();
    m_InputQueue.pop();
    return true;
}

size_t ENetServerNetwork::GetInputQueueSize() const
{
    std::lock_guard<std::mutex> lock(m_InputMutex);
    return m_InputQueue.size();
}

//-----------------------------------------------------------------------------
// No-ops on server side
//-----------------------------------------------------------------------------
void ENetServerNetwork::SendInputCmd(const InputCmd&) {}
bool ENetServerNetwork::ReceiveSnapshot(Snapshot&) { return false; }
size_t ENetServerNetwork::GetSnapshotQueueSize() const { return 0; }
