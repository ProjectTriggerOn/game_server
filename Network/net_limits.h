#pragma once
//=============================================================================
// net_limits.h
//
// Tunable limits for inbound single-client-flood mitigation, kept in ONE place
// so thresholds are easy to adjust. See the Phase 2 design for full derivation.
//=============================================================================

#include <cstdint>
#include <cstddef>

namespace NetLimits
{
    //-------------------------------------------------------------------------
    // L1 - per-peer inbound InputCmd token bucket (see enet_server_network.cpp).
    //
    // Budget = REFILL_PER_TICK * TICK_RATE(32) = 1024 packets/sec/peer.
    // Derivation: the client sends exactly one InputCmd per render frame, so its
    // send rate == frame rate. A refresh-locked client tops out at ~500/s (500Hz
    // panel), so 1024/s clears every realistic legit client (~2x margin) yet sits
    // ~10x+ below a genuine flood (measured at 2.6M/s). Depth 64 (~2 ticks) lets
    // a short frame/jitter burst through while a sustained stream is clamped to
    // the refill rate. Floats so the refill divides cleanly if TICK_RATE changes.
    //-------------------------------------------------------------------------
    constexpr float INPUT_BUCKET_REFILL_PER_TICK = 32.0f;
    constexpr float INPUT_BUCKET_DEPTH           = 64.0f;

    //-------------------------------------------------------------------------
    // L3 - cap on events processed per PollEvents call (backstop). Bounds the
    // worst-case time of a single receive drain so no peer can stretch one loop
    // iteration unbounded; the remainder waits in ENet's/kernel's buffer for the
    // next call (~1ms later). Well above any legit per-call load (10 players is
    // ~tens of events/call), so it only bites under a flood. It bounds CPU per
    // call, NOT the socket buffer — under an extreme flood the undrained excess
    // is dropped by the kernel, not counted here (see Phase 2 L3 notes).
    //-------------------------------------------------------------------------
    constexpr int MAX_EVENTS_PER_POLL = 4096;

    //-------------------------------------------------------------------------
    // L2 - host maximumPacketSize cap (ENet default is 32MB). It applies to
    // packets SENT OR RECEIVED, so it must be >= the largest packet the server
    // sends (a Snapshot, 1 + sizeof(Snapshot)) or enet_peer_send would silently
    // reject snapshots — a static_assert in enet_server_network.cpp guards this.
    // Kept below the MTU (1392) so nothing legit ever fragments, which bounds the
    // fragment-reassembly buffer an attacker could otherwise force ENet to
    // allocate (up to 32MB; see Phase 2 R5). Inbound enforcement lives in ENet's
    // prebuilt lib, so it is not verifiable from source here.
    //-------------------------------------------------------------------------
    constexpr size_t MAX_PACKET_BYTES = 1024;
}
