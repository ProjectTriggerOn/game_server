#pragma once
//=============================================================================
// server_collision.h
//
// Pure-math collision detection for game_server.
// No DirectXMath dependency - uses Float3 from net_common.h.
// Must be kept in sync with game_client CollisionWorld logic.
//=============================================================================

#include "net_common.h"
#include <vector>
#include <cmath>
#include <algorithm>

struct ServerAABB
{
	Float3 min;
	Float3 max;
};

struct ServerCollider
{
	ServerAABB aabb;
	bool isGround;
};

struct ServerCollisionResult
{
	Float3 position;
	Float3 velocity;
	bool isGrounded;
};

//-----------------------------------------------------------------------------
// Inline implementation (header-only for simplicity)
//-----------------------------------------------------------------------------
namespace ServerCollision
{

inline Float3 ClosestPointOnSegment(const Float3& segA, const Float3& segB, const Float3& point)
{
	float abx = segB.x - segA.x;
	float aby = segB.y - segA.y;
	float abz = segB.z - segA.z;

	float abLenSq = abx * abx + aby * aby + abz * abz;
	if (abLenSq < 1e-8f)
		return segA;

	float apx = point.x - segA.x;
	float apy = point.y - segA.y;
	float apz = point.z - segA.z;

	float t = (apx * abx + apy * aby + apz * abz) / abLenSq;
	t = std::clamp(t, 0.0f, 1.0f);

	return { segA.x + abx * t, segA.y + aby * t, segA.z + abz * t };
}

inline Float3 ClampToAABB(const Float3& point, const ServerAABB& aabb)
{
	return {
		std::clamp(point.x, aabb.min.x, aabb.max.x),
		std::clamp(point.y, aabb.min.y, aabb.max.y),
		std::clamp(point.z, aabb.min.z, aabb.max.z)
	};
}

inline ServerCollisionResult ResolveCapsule(
	const std::vector<ServerCollider>& colliders,
	const Float3& capsuleBottom,
	float capsuleHeight,
	float capsuleRadius,
	const Float3& velocity)
{
	ServerCollisionResult result;
	result.position = capsuleBottom;
	result.velocity = velocity;
	result.isGrounded = false;

	float innerBottom = capsuleRadius;
	float innerTop = capsuleHeight - capsuleRadius;
	if (innerTop < innerBottom) innerTop = innerBottom;

	for (int iter = 0; iter < 4; ++iter)
	{
		bool anyCollision = false;

		for (const auto& collider : colliders)
		{
			const ServerAABB& aabb = collider.aabb;

			Float3 segA = {
				result.position.x,
				result.position.y + innerBottom,
				result.position.z
			};
			Float3 segB = {
				result.position.x,
				result.position.y + innerTop,
				result.position.z
			};

			Float3 closestOnSeg = ClosestPointOnSegment(segA, segB,
				{ std::clamp(segA.x, aabb.min.x, aabb.max.x),
				  std::clamp(segA.y, aabb.min.y, aabb.max.y),
				  std::clamp(segA.z, aabb.min.z, aabb.max.z) });

			Float3 closestOnAABB = ClampToAABB(closestOnSeg, aabb);

			closestOnSeg = ClosestPointOnSegment(segA, segB, closestOnAABB);
			closestOnAABB = ClampToAABB(closestOnSeg, aabb);

			float dx = closestOnSeg.x - closestOnAABB.x;
			float dy = closestOnSeg.y - closestOnAABB.y;
			float dz = closestOnSeg.z - closestOnAABB.z;
			float distSq = dx * dx + dy * dy + dz * dz;

			if (distSq > capsuleRadius * capsuleRadius)
				continue;

			float dist = sqrtf(distSq);
			float nx, ny, nz;

			if (dist > 1e-6f)
			{
				nx = dx / dist;
				ny = dy / dist;
				nz = dz / dist;
			}
			else
			{
				float cx = (aabb.min.x + aabb.max.x) * 0.5f;
				float cy = (aabb.min.y + aabb.max.y) * 0.5f;
				float cz = (aabb.min.z + aabb.max.z) * 0.5f;
				float hx = (aabb.max.x - aabb.min.x) * 0.5f;
				float hy = (aabb.max.y - aabb.min.y) * 0.5f;
				float hz = (aabb.max.z - aabb.min.z) * 0.5f;

				float relX = closestOnSeg.x - cx;
				float relY = closestOnSeg.y - cy;
				float relZ = closestOnSeg.z - cz;

				float overlapX = hx + capsuleRadius - fabsf(relX);
				float overlapY = hy + capsuleRadius - fabsf(relY);
				float overlapZ = hz + capsuleRadius - fabsf(relZ);

				nx = ny = nz = 0.0f;
				if (overlapX <= overlapY && overlapX <= overlapZ)
					nx = (relX >= 0.0f) ? 1.0f : -1.0f;
				else if (overlapY <= overlapX && overlapY <= overlapZ)
					ny = (relY >= 0.0f) ? 1.0f : -1.0f;
				else
					nz = (relZ >= 0.0f) ? 1.0f : -1.0f;
			}

			float penetration = capsuleRadius - dist;

			result.position.x += nx * penetration;
			result.position.y += ny * penetration;
			result.position.z += nz * penetration;

			float velDotN = result.velocity.x * nx + result.velocity.y * ny + result.velocity.z * nz;
			if (velDotN < 0.0f)
			{
				result.velocity.x -= velDotN * nx;
				result.velocity.y -= velDotN * ny;
				result.velocity.z -= velDotN * nz;
			}

			if (collider.isGround && ny > PhysicsConfig::GROUND_NORMAL_THRESHOLD)
			{
				result.isGrounded = true;
			}

			anyCollision = true;
		}

		if (!anyCollision) break;
	}

	return result;
}

} // namespace ServerCollision
