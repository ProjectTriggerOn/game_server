#pragma once
//=============================================================================
// i_network.h
//
// Abstract network interface for Server-Authoritative architecture.
// IMPORTANT: This file must be kept in sync with game_client/Network/i_network.h
//=============================================================================

#include "net_common.h"
#include <cstdint>

class INetwork
{
public:
    virtual ~INetwork() = default;

    virtual void Initialize() = 0;
    virtual void Finalize() = 0;

    // Client -> Server (Upstream)
    virtual void SendInputCmd(const InputCmd& cmd) = 0;
    virtual bool ReceiveInputCmd(InputCmd& outCmd) = 0;
    virtual size_t GetInputQueueSize() const = 0;

    // Server -> Client (Downstream)
    virtual void SendSnapshot(const Snapshot& snapshot) = 0;
    virtual bool ReceiveSnapshot(Snapshot& outSnapshot) = 0;
    virtual size_t GetSnapshotQueueSize() const = 0;

    // Statistics
    virtual uint32_t GetTotalInputsSent() const = 0;
    virtual uint32_t GetTotalSnapshotsSent() const = 0;
};
