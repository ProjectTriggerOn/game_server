#pragma once
//=============================================================================
// server_raycast.h
//
// Ray-Capsule intersection test for server-side hitscan.
// Pure math, no DirectXMath dependency — uses Float3 from net_common.h.
//=============================================================================

#include "net_common.h"
#include <cmath>

namespace ServerRaycast {

//-----------------------------------------------------------------------------
// Dot product
//-----------------------------------------------------------------------------
inline float Dot(const Float3& a, const Float3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

//-----------------------------------------------------------------------------
// Subtract
//-----------------------------------------------------------------------------
inline Float3 Sub(const Float3& a, const Float3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

//-----------------------------------------------------------------------------
// Add
//-----------------------------------------------------------------------------
inline Float3 Add(const Float3& a, const Float3& b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

//-----------------------------------------------------------------------------
// Scale
//-----------------------------------------------------------------------------
inline Float3 Scale(const Float3& v, float s)
{
    return { v.x * s, v.y * s, v.z * s };
}

//-----------------------------------------------------------------------------
// ClosestPointOnSegment — returns t in [0,1]
//-----------------------------------------------------------------------------
inline float ClosestTOnSegment(const Float3& segA, const Float3& segB,
                                const Float3& point)
{
    Float3 ab = Sub(segB, segA);
    Float3 ap = Sub(point, segA);
    float denom = Dot(ab, ab);
    if (denom < 1e-8f) return 0.0f;
    float t = Dot(ap, ab) / denom;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

//-----------------------------------------------------------------------------
// RayCapsule — test ray against a capsule (two hemispheres + cylinder)
//
// Returns true if the ray hits the capsule, and outT is the hit distance.
// Uses the "closest approach between two lines" method:
//   Ray line: P = origin + t * dir
//   Capsule segment: Q = segA + s * (segB - segA)
// Find (t, s) that minimize |P - Q|, check if distance <= radius.
//
// Parameters:
//   origin     — ray start position (eye position)
//   dir        — ray direction (normalized)
//   capBottom   — capsule foot position (bottom of capsule)
//   capHeight   — total height of capsule
//   capRadius   — capsule radius
//   outT        — [out] hit distance along ray (if hit)
//   maxRange    — maximum ray range (default 200m)
//
// Returns: true if hit within maxRange
//-----------------------------------------------------------------------------
inline bool RayCapsule(const Float3& origin, const Float3& dir,
                       const Float3& capBottom, float capHeight, float capRadius,
                       float& outT, float maxRange = 200.0f)
{
    // Capsule segment: A = bottom + (0, radius, 0), B = bottom + (0, height - radius, 0)
    Float3 segA = { capBottom.x, capBottom.y + capRadius, capBottom.z };
    Float3 segB = { capBottom.x, capBottom.y + capHeight - capRadius, capBottom.z };

    // Line-segment closest approach
    // Ray: P(t) = origin + t * dir
    // Seg: Q(s) = segA + s * segDir,  where segDir = segB - segA
    Float3 segDir = Sub(segB, segA);
    Float3 w0 = Sub(origin, segA);

    float a = Dot(dir, dir);       // always 1 if dir is normalized
    float b = Dot(dir, segDir);
    float c = Dot(segDir, segDir);
    float d = Dot(dir, w0);
    float e = Dot(segDir, w0);

    float denom = a * c - b * b;

    float t, s;

    if (denom < 1e-6f)
    {
        // Lines are nearly parallel
        s = 0.0f;
        t = -d / a;
    }
    else
    {
        s = (b * d - a * e) / denom;
        t = (c * d - b * e) / denom;
    }

    // Clamp s to [0, 1] (capsule segment) and recompute t
    if (s < 0.0f)
    {
        s = 0.0f;
        t = -d / a; // dot(dir, origin - segA) / dot(dir, dir)
    }
    else if (s > 1.0f)
    {
        s = 1.0f;
        Float3 w1 = Sub(origin, segB);
        t = -Dot(dir, w1) / a;
    }

    // t must be positive (ray goes forward) and within range
    if (t < 0.0f) t = 0.0f;
    if (t > maxRange) return false;

    // Compute closest points
    Float3 closestOnRay = Add(origin, Scale(dir, t));
    Float3 closestOnSeg = Add(segA, Scale(segDir, s));
    Float3 diff = Sub(closestOnRay, closestOnSeg);
    float distSq = Dot(diff, diff);

    if (distSq <= capRadius * capRadius)
    {
        outT = t;
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// DirectionFromYawPitch — compute normalized direction vector
//-----------------------------------------------------------------------------
inline Float3 DirectionFromYawPitch(float yaw, float pitch)
{
    float cosPitch = cosf(pitch);
    return {
        sinf(yaw) * cosPitch,
        -sinf(pitch),
        cosf(yaw) * cosPitch
    };
}

} // namespace ServerRaycast
