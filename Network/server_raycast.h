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
// RaySphere — test ray against a sphere
//
// Returns true if hit, outT = entry distance along ray.
//-----------------------------------------------------------------------------
inline bool RaySphere(const Float3& origin, const Float3& dir,
                      const Float3& center, float radius, float& outT)
{
    Float3 oc = Sub(origin, center);
    float a = Dot(dir, dir);
    float h = Dot(oc, dir);
    float c = Dot(oc, oc) - radius * radius;
    float disc = h * h - a * c;
    if (disc < 0.0f) return false;
    float sqrtDisc = sqrtf(disc);
    float t = (-h - sqrtDisc) / a;
    if (t < 0.0f) t = (-h + sqrtDisc) / a;
    if (t < 0.0f) return false;
    outT = t;
    return true;
}

//-----------------------------------------------------------------------------
// RayCapsule — test ray against a capsule (cylinder + two hemispheres)
//
// Decomposes the capsule into:
//   1. Infinite cylinder (clamped to segment extent)
//   2. Bottom hemisphere (sphere at segA)
//   3. Top hemisphere (sphere at segB)
// Returns the closest hit among all three.
//
// Parameters:
//   origin      — ray start position (eye position)
//   dir         — ray direction (normalized)
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
    // Capsule segment endpoints (sphere centers)
    Float3 segA = { capBottom.x, capBottom.y + capRadius, capBottom.z };
    Float3 segB = { capBottom.x, capBottom.y + capHeight - capRadius, capBottom.z };
    Float3 segDir = Sub(segB, segA);
    float segLenSq = Dot(segDir, segDir);

    float bestT = maxRange + 1.0f;
    bool hasHit = false;

    // 1. Test ray against infinite cylinder, clamp to segment extent
    if (segLenSq > 1e-8f)
    {
        float segLen = sqrtf(segLenSq);
        Float3 axis = Scale(segDir, 1.0f / segLen);
        Float3 oc = Sub(origin, segA);

        float dDotAxis = Dot(dir, axis);
        float ocDotAxis = Dot(oc, axis);

        // Project out the capsule axis component
        Float3 dPerp = Sub(dir, Scale(axis, dDotAxis));
        Float3 ocPerp = Sub(oc, Scale(axis, ocDotAxis));

        float a = Dot(dPerp, dPerp);
        float b = Dot(dPerp, ocPerp);
        float c = Dot(ocPerp, ocPerp) - capRadius * capRadius;

        float disc = b * b - a * c;
        if (disc >= 0.0f && a > 1e-8f)
        {
            float sqrtDisc = sqrtf(disc);
            float t = (-b - sqrtDisc) / a;
            if (t < 0.0f) t = (-b + sqrtDisc) / a;

            if (t >= 0.0f && t <= maxRange)
            {
                float hitOnAxis = ocDotAxis + t * dDotAxis;
                if (hitOnAxis >= 0.0f && hitOnAxis <= segLen)
                {
                    bestT = t;
                    hasHit = true;
                }
            }
        }
    }

    // 2. Test against bottom hemisphere (sphere at segA)
    float tSphere;
    if (RaySphere(origin, dir, segA, capRadius, tSphere))
    {
        if (tSphere <= maxRange && tSphere < bestT)
        {
            bestT = tSphere;
            hasHit = true;
        }
    }

    // 3. Test against top hemisphere (sphere at segB)
    if (RaySphere(origin, dir, segB, capRadius, tSphere))
    {
        if (tSphere <= maxRange && tSphere < bestT)
        {
            bestT = tSphere;
            hasHit = true;
        }
    }

    if (hasHit)
    {
        outT = bestT;
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
        sinf(pitch),
        cosf(yaw) * cosPitch
    };
}

} // namespace ServerRaycast
