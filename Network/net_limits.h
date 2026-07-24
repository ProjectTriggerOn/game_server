#pragma once
//=============================================================================
// net_limits.h
//
// Tunable limits for inbound single-client-flood mitigation, kept in ONE place
// so thresholds are easy to adjust. See the Phase 2 design for full derivation.
//=============================================================================

#include <cstdint>

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
}
