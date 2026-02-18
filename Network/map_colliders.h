#pragma once
//=============================================================================
// map_colliders.h
//
// Shared map data definitions.
// This file is used by BOTH client and server — keep in sync!
// No DirectXMath dependency — pure C structs.
//
// MAP_GRID[][] defines the obstacle layout (1=block, 0=empty).
// MAP_COLLIDERS[] defines merged AABBs for physics/hitscan.
// The client generates individual cube draw calls from the grid;
// the server only reads MAP_COLLIDERS[].
//=============================================================================

//-----------------------------------------------------------------------------
// Grid-based map layout
//-----------------------------------------------------------------------------
static const int MAP_GRID_ROWS    = 20;
static const int MAP_GRID_COLS    = 20;
static const int MAP_BLOCK_HEIGHT = 4;      // cubes stacked per obstacle cell
static const float MAP_OFFSET_X   = -10.0f; // world X offset (center the grid)
static const float MAP_OFFSET_Z   = -10.0f; // world Z offset (center the grid)

static const int MAP_GRID[MAP_GRID_ROWS][MAP_GRID_COLS] =
{
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

//-----------------------------------------------------------------------------
// Collider definition (physics / hitscan)
//-----------------------------------------------------------------------------
struct MapColliderDef
{
	float minX, minY, minZ;
	float maxX, maxY, maxZ;
	bool  isGround;
};

// ============================================================================
// COLLIDERS — Manually merged AABBs for physics/hitscan.
//
// Grid cell (row, col) maps to world:
//   X = col + MAP_OFFSET_X,  Z = row + MAP_OFFSET_Z
//   A group from (r1,c1)-(r2,c2) → AABB:
//     min = (c1-10, 0, r1-10)   max = (c2+1-10, 4, r2+1-10)
// ============================================================================
static const MapColliderDef MAP_COLLIDERS[] =
{
	//  AABB min                  AABB max                ground

	// --- Ground plane ---
	{ -128.0f,-1.0f,-128.0f,   128.0f, 0.0f, 128.0f,   true  },

	// --- Interior blocks (3x3 each, height 4) ---
	// Block A: rows 5-7, cols 5-7
	{  -5.0f, 0.0f, -5.0f,    -2.0f, 4.0f, -2.0f,      true  },
	// Block B: rows 5-7, cols 12-14
	{   2.0f, 0.0f, -5.0f,     5.0f, 4.0f, -2.0f,      true  },
	// Block C: rows 12-14, cols 5-7
	{  -5.0f, 0.0f,  2.0f,    -2.0f, 4.0f,  5.0f,      true  },
	// Block D: rows 12-14, cols 12-14
	{   2.0f, 0.0f,  2.0f,     5.0f, 4.0f,  5.0f,      true  },
};

static const int MAP_COLLIDER_COUNT = sizeof(MAP_COLLIDERS) / sizeof(MAP_COLLIDERS[0]);
