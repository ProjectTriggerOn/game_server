//=============================================================================
// recoil_math.h — COD-model recoil: shared pure math.
// MIRRORED: game_client/Network/recoil_math.h — MUST BE KEPT IN SYNC
// (same discipline as net_common.h; no wire version negotiation exists).
// Pure functions, no state, no I/O. Angles in radians unless Deg suffix.
// Deterministic: pattern idx = (fireCounter-1) % PATTERN_LEN; no RNG —
// server and client independently compute the same trajectory.
// punch = visual-only (decays to zero, never touches player yaw/pitch);
// shotKick = tiny real component; bloom = spread growth (main control).
//=============================================================================
#pragma once

#include "net_common.h"

#include <cmath>
#include <cstdint>

namespace RecoilMath {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

//-----------------------------------------------------------------------------
// RecoilState — per-player integration state (server: in PlayerData;
// client: in PlayerFps). punch/shotKick in radians, bloom in degrees.
//-----------------------------------------------------------------------------
struct RecoilState {
    float punchPitch    = 0.0f;  // rad — visual, decays to zero
    float punchYaw      = 0.0f;  // rad — visual, decays to zero
    float shotKickPitch = 0.0f;  // rad — real, accumulates (not decayed)
    float shotKickYaw   = 0.0f;  // rad — real, accumulates (not decayed)
    float bloomDeg      = 0.0f;  // deg — spread growth, decays with punch
};

//-----------------------------------------------------------------------------
// Hash01 — deterministic u16 → [0,1). Knuth multiplicative + finalize mix.
// Same value on both sides for the same input: this is the "RNG".
//-----------------------------------------------------------------------------
inline float Hash01(uint16_t v)
{
    uint32_t x = v * 2654435761u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    return static_cast<float>(x & 0x00FFFFFFu) / 16777216.0f; // [0,1)
}

//-----------------------------------------------------------------------------
// PunchEnvelope — ramp for the first shots of a burst (0.6 → 1.0 by shot 5),
// so single taps kick less than sustained fire (spec §5.1).
//-----------------------------------------------------------------------------
inline float PunchEnvelope(uint16_t burstIdx)
{
    // burstIdx = (fireCounter-1) % PATTERN_LEN — the envelope follows the
    // pattern index, so it also cycles after a full mag.
    static constexpr float ENV[4] = { 0.6f, 0.75f, 0.9f, 1.0f };
    return ENV[burstIdx < 4 ? burstIdx : 3];
}

//-----------------------------------------------------------------------------
// RecoilSpreadRadians — current aim-cone half-angle for a shot.
//   moveFactor: 0 = still, 1 = full run. Movement multiplies the BASE
//   (HIP ×1.5, ADS ×1.3 per spec §1.1); bloom is added unscaled.
//-----------------------------------------------------------------------------
// Single overload, NO default argument and NO 3-arg sibling — a default value
// plus a same-name prefix overload is ambiguous for a 3-arg call in C++ (the
// client hit C2668). Both call sites pass moveFactor explicitly.
inline float RecoilSpreadRadians(uint8_t teamId, bool ads, float bloomDeg,
                                 float moveFactor)
{
    const RecoilConfig::WeaponSpec& w = RecoilConfig::SpecForTeam(teamId);
    const float base = (ads ? w.spreadBaseDegAds : w.spreadBaseDegHip) * kDegToRad;
    // ADS moving is punished less than HIP moving (spec §1.1): ×1.3 vs ×1.5.
    const float moveMult = 1.0f + moveFactor * (ads ? 0.3f : 0.5f);
    return base * moveMult + bloomDeg * kDegToRad;
}

//-----------------------------------------------------------------------------
// RecoilConeOffset — deterministic in-cone direction offset for one shot.
// Two independent uniform samples map into the unit disc (sqrt for uniform
// area coverage), scaled by the cone half-angle. dPitch/dYaw are angle
// offsets, NOT a renormalized direction — the caller applies them exactly
// like punch: DirectionFromYawPitch(yaw + dYaw, pitch + dPitch).
//-----------------------------------------------------------------------------
inline void RecoilConeOffset(float spreadRad, uint16_t fireCounter,
                             float& outDPitch, float& outDYaw)
{
    const float r0 = Hash01(fireCounter);
    const float r1 = Hash01(static_cast<uint16_t>(fireCounter + 0x9E37u));
    const float r   = std::sqrt(r0) * spreadRad;     // uniform disc radius
    const float ang = r1 * 6.28318530717958647692f;  // uniform angle
    // Small-angle mapping: yaw offset scaled by 1/cos(pitch) is ignored —
    // max pitch error at 45+° is <2% of the cone, far under tuning noise.
    outDYaw   = r * std::sin(ang);
    outDPitch = r * std::cos(ang);
}

//-----------------------------------------------------------------------------
// RecoilAdvance — integrate one recoil step.
//   newlyFired: a shot resolved on this call → apply per-shot punch/kick/bloom.
//   Always decays punch & bloom by dt (frame-rate independent; server ticks
//   and client frames share the same formula).
//   shotKick is intentionally NOT decayed — it is the accumulated real
//   trajectory component and is broadcast for reconciliation.
//-----------------------------------------------------------------------------
inline void RecoilAdvance(RecoilState& rs, uint8_t teamId, uint16_t fireCounter,
                          bool ads, bool newlyFired, float dt)
{
    if (newlyFired)
    {
        const uint16_t idx = static_cast<uint16_t>((fireCounter - 1u) % RecoilConfig::PATTERN_LEN);
        const RecoilConfig::WeaponSpec& w = RecoilConfig::SpecForTeam(teamId);
        const float env  = PunchEnvelope(idx);
        const float adsS = ads ? w.adsPunchScale : 1.0f;
        const float zig  = (idx & 1u) ? -1.0f : 1.0f;   // sway alternates each shot
        rs.punchPitch    += w.punchPitchDeg * kDegToRad * env * adsS;
        rs.punchYaw      += w.punchYawDeg * kDegToRad * env * adsS * zig;
        rs.shotKickPitch += w.realKickPitchDeg * kDegToRad;
        rs.bloomDeg       = std::fmin(rs.bloomDeg + w.bloomPerShotDeg, w.bloomMaxDeg);
    }
    const float k = std::exp(-RecoilConfig::SpecForTeam(teamId).decayHz * dt);
    rs.punchPitch *= k;
    rs.punchYaw   *= k;
    rs.bloomDeg   *= k;
}

//-----------------------------------------------------------------------------
// RecoilTotalOffsets — the direction deltas a shot inherits from recoil.
// WYSIWYG contract (spec §2): bullets follow the punched view, so both the
// server's ray and the client's rendered view use these same offsets added
// to the player's raw yaw/pitch.
//-----------------------------------------------------------------------------
inline void RecoilTotalOffsets(const RecoilState& rs,
                               float& outDPitch, float& outDYaw)
{
    outDPitch = rs.punchPitch + rs.shotKickPitch;
    outDYaw   = rs.punchYaw + rs.shotKickYaw;
}

} // namespace RecoilMath
