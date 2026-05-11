//=============================================================================
// enet_server_network.cpp
//
// ENet-based server network implementation.
// Supports multiple concurrent clients with per-player routing.
//=============================================================================

#include "enet_server_network.h"
#include "net_packet.h"
#include "../server_log.h"
#include <cstring>
#include <algorithm>
#include <cmath>

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
        SLOG_ERROR("Failed to initialize ENet.");
        return;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = m_Port;

    // Create server host: max 4 clients, 2 channels
    m_pServer = enet_host_create(&address, MAX_PLAYERS, 2, 0, 0);
    if (!m_pServer)
    {
        SLOG_ERROR("Failed to create ENet server host on port %u.", m_Port);
        enet_deinitialize();
        return;
    }

    SLOG_INFO("Listening on port %u.", m_Port);
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
    m_PeerToPlayerId.clear();
    m_PlayerIdToPeer.clear();

    if (m_pServer)
    {
        enet_host_destroy(m_pServer);
        m_pServer = nullptr;
    }

    enet_deinitialize();
    SLOG_INFO("Shut down.");
}

//-----------------------------------------------------------------------------
// AllocatePlayerId - Find first free ID in [0, MAX_PLAYERS)
//-----------------------------------------------------------------------------
uint8_t ENetServerNetwork::AllocatePlayerId()
{
    for (uint8_t i = 0; i < MAX_PLAYERS; i++)
    {
        if (m_PlayerIdToPeer.find(i) == m_PlayerIdToPeer.end())
            return i;
    }
    return 0xFF; // Server full
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
            uint8_t playerId = AllocatePlayerId();
            if (playerId == 0xFF)
            {
                SLOG_WARN("Server full, rejecting connection.");
                enet_peer_disconnect_now(event.peer, 0);
                break;
            }

            m_ConnectedPeers.push_back(event.peer);
            m_PeerToPlayerId[event.peer] = playerId;
            m_PlayerIdToPeer[playerId] = event.peer;

            // Queue player connect event
            {
                std::lock_guard<std::mutex> lock(m_EventMutex);
                m_PlayerEventQueue.push({playerId, true});
            }

            SLOG_INFO("Client connected as Player %u from %x:%u. Total clients: %zu",
                playerId, event.peer->address.host, event.peer->address.port, m_ConnectedPeers.size());
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        {
            auto it = m_PeerToPlayerId.find(event.peer);
            if (it != m_PeerToPlayerId.end())
            {
                uint8_t playerId = it->second;
                m_PlayerIdToPeer.erase(playerId);
                m_PeerToPlayerId.erase(it);

                // Queue player disconnect event
                {
                    std::lock_guard<std::mutex> lock(m_EventMutex);
                    m_PlayerEventQueue.push({playerId, false});
                }

                m_ConnectedPeers.erase(
                    std::remove(m_ConnectedPeers.begin(), m_ConnectedPeers.end(), event.peer),
                    m_ConnectedPeers.end());
                SLOG_INFO("Player %u disconnected. Total clients: %zu",
                    playerId, m_ConnectedPeers.size());
                break;
            }

            m_ConnectedPeers.erase(
                std::remove(m_ConnectedPeers.begin(), m_ConnectedPeers.end(), event.peer),
                m_ConnectedPeers.end());
            SLOG_INFO("Unknown peer disconnected. Total clients: %zu", m_ConnectedPeers.size());
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

                    // Reject NaN/Inf in any float field — they would propagate through
                    // physics and corrupt every player's state, then be broadcast to all
                    // clients (1-player DoS attack on the whole match).
                    if (!std::isfinite(cmd.moveAxisX) ||
                        !std::isfinite(cmd.moveAxisY) ||
                        !std::isfinite(cmd.yaw) ||
                        !std::isfinite(cmd.pitch))
                    {
                        enet_packet_destroy(event.packet);
                        break;
                    }

                    // Tag with the sender's playerId
                    auto it = m_PeerToPlayerId.find(event.peer);
                    uint8_t playerId = (it != m_PeerToPlayerId.end()) ? it->second : 0xFF;

                    if (playerId != 0xFF)
                    {
                        std::lock_guard<std::mutex> lock(m_InputMutex);
                        m_TaggedInputQueue.push({cmd, playerId});
                    }
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
// ReceiveTaggedInput - Pop from tagged input queue (used by GameServer)
//-----------------------------------------------------------------------------
bool ENetServerNetwork::ReceiveTaggedInput(TaggedInput& out)
{
    std::lock_guard<std::mutex> lock(m_InputMutex);
    if (m_TaggedInputQueue.empty()) return false;

    out = m_TaggedInputQueue.front();
    m_TaggedInputQueue.pop();
    return true;
}

//-----------------------------------------------------------------------------
// PollPlayerEvent - Pop connect/disconnect events
//-----------------------------------------------------------------------------
bool ENetServerNetwork::PollPlayerEvent(PlayerEvent& out)
{
    std::lock_guard<std::mutex> lock(m_EventMutex);
    if (m_PlayerEventQueue.empty()) return false;

    out = m_PlayerEventQueue.front();
    m_PlayerEventQueue.pop();
    return true;
}

//-----------------------------------------------------------------------------
// GetConnectedPlayerIds
//-----------------------------------------------------------------------------
std::vector<uint8_t> ENetServerNetwork::GetConnectedPlayerIds() const
{
    std::vector<uint8_t> ids;
    ids.reserve(m_PlayerIdToPeer.size());
    for (const auto& pair : m_PlayerIdToPeer)
    {
        ids.push_back(pair.first);
    }
    return ids;
}

//-----------------------------------------------------------------------------
// SendSnapshotToPlayer - Send to a specific player by ID
//-----------------------------------------------------------------------------
void ENetServerNetwork::SendSnapshotToPlayer(uint8_t playerId, const Snapshot& snapshot)
{
    auto it = m_PlayerIdToPeer.find(playerId);
    if (it == m_PlayerIdToPeer.end()) return;

    uint8_t buffer[1 + sizeof(Snapshot)];
    buffer[0] = static_cast<uint8_t>(PacketType::SNAPSHOT);
    std::memcpy(buffer + 1, &snapshot, sizeof(Snapshot));

    ENetPacket* packet = enet_packet_create(
        buffer,
        sizeof(buffer),
        ENET_PACKET_FLAG_UNSEQUENCED
    );
    // enet_packet_create returns NULL on allocation failure — passing NULL
    // to enet_peer_send dereferences it and crashes the server.
    if (!packet) return;

    enet_peer_send(it->second, 1, packet);

    m_TotalSnapshotsSent++;
}

//-----------------------------------------------------------------------------
// SendSnapshot - Broadcast same snapshot to all (INetwork compatibility)
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
        if (!packet) continue;
        enet_peer_send(peer, 1, packet);
    }

    m_TotalSnapshotsSent++;
}

//-----------------------------------------------------------------------------
// ReceiveInputCmd - INetwork compatibility (ignores playerId)
//-----------------------------------------------------------------------------
bool ENetServerNetwork::ReceiveInputCmd(InputCmd& outCmd)
{
    std::lock_guard<std::mutex> lock(m_InputMutex);
    if (m_TaggedInputQueue.empty()) return false;

    outCmd = m_TaggedInputQueue.front().cmd;
    m_TaggedInputQueue.pop();
    return true;
}

size_t ENetServerNetwork::GetInputQueueSize() const
{
    std::lock_guard<std::mutex> lock(m_InputMutex);
    return m_TaggedInputQueue.size();
}

//-----------------------------------------------------------------------------
// No-ops on server side
//-----------------------------------------------------------------------------
void ENetServerNetwork::SendInputCmd(const InputCmd&) {}
bool ENetServerNetwork::ReceiveSnapshot(Snapshot&) { return false; }
size_t ENetServerNetwork::GetSnapshotQueueSize() const { return 0; }
