#pragma once
//=============================================================================
// map_colliders.h
//
// Shared map collider definitions.
// This file is used by BOTH client and server — keep in sync!
// No DirectXMath dependency — pure C structs.
//=============================================================================

// Object categories
enum MapCategory
{
	MAP_GROUND = 0,   // Ground plane — rendered as MeshField, has AABB
	MAP_CUBE   = 1,   // Cube block — rendered with Cube_Draw, AABB from position
	MAP_WALL   = 2,   // Invisible wall — AABB only, no rendering
};

// Collider definition
// For MAP_CUBE: AABB is auto-computed from position (±0.5 unit cube)
// For MAP_GROUND / MAP_WALL: AABB is specified explicitly via min/max
struct MapColliderDef
{
	int   category;
	float posX, posY, posZ;     // position (used for cube rendering & AABB center)
	float minX, minY, minZ;     // AABB min (explicit, ignored for MAP_CUBE)
	float maxX, maxY, maxZ;     // AABB max (explicit, ignored for MAP_CUBE)
	bool  isGround;             // true = player can land on this surface
};

// ============================================================================
// MAP DATA — Add new colliders here!
// ============================================================================
static const MapColliderDef MAP_COLLIDERS[] =
{
	// === Ground ===
	//  cat       pos(x,y,z)           AABB min                  AABB max               ground
	{ MAP_GROUND, 0,0,0,    -128.0f,-1.0f,-128.0f,   128.0f, 0.0f, 128.0f,   true  },

	// === Cube Blocks (AABB auto = pos ± 0.5) ===
	// y=0.5 → AABB [0,1] (sits on ground)  |  y=1.5 → AABB [1,2] (second layer)
	//  cat       pos(x,y,z)           (min/max ignored for cubes)                       ground
	{ MAP_CUBE,   7.5f, 0.5f, 7.5f,   0,0,0, 0,0,0,                                    true  },
	{ MAP_CUBE,   7.5f, 0.5f, 8.5f,   0,0,0, 0,0,0,                                    true  },
	{ MAP_CUBE,   8.5f, 0.5f, 7.5f,   0,0,0, 0,0,0,                                    true  },
	{ MAP_CUBE,   8.5f, 0.5f, 8.5f,   0,0,0, 0,0,0,                                    true  },
	{ MAP_CUBE,   7.5f, 1.5f, 7.5f,   0,0,0, 0,0,0,                                    true  },
	{ MAP_CUBE,   8.5f, 1.5f, 7.5f,   0,0,0, 0,0,0,                                    true  },

	// === Invisible Walls (map boundary) ===
	//  cat       pos(unused)          AABB min                  AABB max               ground
	// (Add map boundary walls here if needed)
};

static const int MAP_COLLIDER_COUNT = sizeof(MAP_COLLIDERS) / sizeof(MAP_COLLIDERS[0]);
