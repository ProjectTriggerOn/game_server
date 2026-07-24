#pragma once
//=============================================================================
// map_io.h
//
// TriggerOn ".map" binary format — shared by client AND server. KEEP IN SYNC!
// (An identical copy lives at game_server/Network/map_io.h.)
//
// Header-only, pure POD, no DirectXMath / Windows headers — compiles on the
// Linux g++ server. Layout must be byte-identical across MSVC and g++, so
// every wire struct is naturally aligned with explicit padding and size-guarded.
//=============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mapio {

static const char MAP_MAGIC[4] = { 'T', 'M', 'A', 'P' };
static const uint32_t MAP_VERSION = 1;

enum : uint8_t { TEAM_RED = 0, TEAM_BLUE = 1 };
enum : uint8_t { LIGHT_DIRECTIONAL = 0, LIGHT_POINT = 1 };

// ---- Wire structs (pure POD, explicit padding, size-guarded) ----------------
struct MapHeader {
    char     magic[4];
    uint32_t version;
    uint32_t collisionChecksum;               // FNV-1a over the collision section
    uint32_t collisionOffset, collisionSize;
    uint32_t visualOffset,    visualSize;
    uint32_t thumbnailOffset, thumbnailSize;   // size 0 => no thumbnail
    char     name[64];
    char     author[64];
};
static_assert(sizeof(MapHeader) == 164, "MapHeader layout");

struct MapAABB {                              // collision section
    float   minX, minY, minZ, maxX, maxY, maxZ;
    uint8_t isGround;
    uint8_t _pad[3];
};
static_assert(sizeof(MapAABB) == 28, "MapAABB layout");

struct MapSpawn {                             // collision section
    float   x, y, z, yaw;
    uint8_t team;                             // TEAM_RED / TEAM_BLUE
    uint8_t _pad[3];
};
static_assert(sizeof(MapSpawn) == 20, "MapSpawn layout");

struct MapModelRef {                          // visual section
    char     asset[64];                       // "__box__" for a box brush, else FBX name
    float    pos[3];
    float    rotEuler[3];
    float    scale[3];                        // box brush: full extents
    uint32_t textureId;                       // box brush texture; ignored for FBX
    uint32_t _reserved;
};
static_assert(sizeof(MapModelRef) == 108, "MapModelRef layout");

struct MapLight {                             // visual section
    uint8_t type;                             // LIGHT_DIRECTIONAL / LIGHT_POINT
    uint8_t _pad[3];
    float   pos[3];
    float   dir[3];
    float   color[3];
    float   intensity;
};
static_assert(sizeof(MapLight) == 44, "MapLight layout");

struct MapEnv {                               // visual section (single instance)
    char  skyAsset[64];
    float ambient[3];
    float fogColor[3];
    float fogStart, fogEnd;
};
static_assert(sizeof(MapEnv) == 96, "MapEnv layout");

// ---- Runtime intermediate (owns the variable-length arrays) -----------------
struct MapData {
    char                     name[64]   = {0};
    char                     author[64] = {0};
    std::vector<MapAABB>     colliders;
    std::vector<MapSpawn>    spawns;
    std::vector<MapModelRef> models;
    std::vector<MapLight>    lights;
    MapEnv                   env = {};
    std::vector<uint8_t>     thumbnail;
};

// ---- Helpers ----------------------------------------------------------------
inline uint32_t Fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

template <class T>
inline void AppendPod(std::vector<uint8_t>& buf, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
}
inline void AppendU32(std::vector<uint8_t>& buf, uint32_t v) { AppendPod(buf, v); }

// Serialize the collision section (aabbCount, MapAABB[], spawnCount, MapSpawn[]).
inline void BuildCollisionBytes(const MapData& d, std::vector<uint8_t>& out) {
    out.clear();
    AppendU32(out, static_cast<uint32_t>(d.colliders.size()));
    for (const auto& a : d.colliders) AppendPod(out, a);
    AppendU32(out, static_cast<uint32_t>(d.spawns.size()));
    for (const auto& s : d.spawns) AppendPod(out, s);
}

// Serialize the visual section (modelCount, MapModelRef[], lightCount, MapLight[], MapEnv).
inline void BuildVisualBytes(const MapData& d, std::vector<uint8_t>& out) {
    out.clear();
    AppendU32(out, static_cast<uint32_t>(d.models.size()));
    for (const auto& m : d.models) AppendPod(out, m);
    AppendU32(out, static_cast<uint32_t>(d.lights.size()));
    for (const auto& l : d.lights) AppendPod(out, l);
    AppendPod(out, d.env);
}

inline uint32_t CollisionChecksum(const MapData& d) {
    std::vector<uint8_t> coll;
    BuildCollisionBytes(d, coll);
    return Fnv1a(coll.data(), coll.size());
}

inline bool Write(const char* path, const MapData& d) {
    std::vector<uint8_t> coll, vis;
    BuildCollisionBytes(d, coll);
    BuildVisualBytes(d, vis);

    MapHeader h = {};
    std::memcpy(h.magic, MAP_MAGIC, 4);
    h.version = MAP_VERSION;
    h.collisionOffset = sizeof(MapHeader);
    h.collisionSize   = static_cast<uint32_t>(coll.size());
    h.visualOffset    = h.collisionOffset + h.collisionSize;
    h.visualSize      = static_cast<uint32_t>(vis.size());
    h.thumbnailOffset = h.visualOffset + h.visualSize;
    h.thumbnailSize   = static_cast<uint32_t>(d.thumbnail.size());
    h.collisionChecksum = Fnv1a(coll.data(), coll.size());
    std::memcpy(h.name,   d.name,   sizeof(h.name));
    std::memcpy(h.author, d.author, sizeof(h.author));

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1
           && (coll.empty() || std::fwrite(coll.data(), coll.size(), 1, f) == 1)
           && (vis.empty()  || std::fwrite(vis.data(),  vis.size(),  1, f) == 1)
           && (d.thumbnail.empty() || std::fwrite(d.thumbnail.data(), d.thumbnail.size(), 1, f) == 1);
    std::fclose(f);
    return ok;
}

// Read helper: pull a POD value from a byte cursor with bounds check.
template <class T>
inline bool ReadPod(const uint8_t* base, size_t total, size_t& cur, T& out) {
    if (cur + sizeof(T) > total) return false;
    std::memcpy(&out, base + cur, sizeof(T));
    cur += sizeof(T);
    return true;
}

inline bool Read(const char* path, MapData& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < static_cast<long>(sizeof(MapHeader))) { std::fclose(f); return false; }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    bool read_ok = std::fread(bytes.data(), bytes.size(), 1, f) == 1;
    std::fclose(f);
    if (!read_ok) return false;

    const uint8_t* base = bytes.data();
    const size_t   total = bytes.size();
    MapHeader h;
    std::memcpy(&h, base, sizeof(h));
    if (std::memcmp(h.magic, MAP_MAGIC, 4) != 0) return false;
    if (h.version != MAP_VERSION) return false;
    if (h.collisionOffset + h.collisionSize > total) return false;
    if (h.visualOffset + h.visualSize > total) return false;

    std::memcpy(out.name,   h.name,   sizeof(out.name));
    std::memcpy(out.author, h.author, sizeof(out.author));
    out.colliders.clear(); out.spawns.clear();
    out.models.clear();    out.lights.clear();

    // Collision section
    {
        size_t cur = h.collisionOffset;
        uint32_t n = 0;
        if (!ReadPod(base, total, cur, n)) return false;
        for (uint32_t i = 0; i < n; i++) { MapAABB a; if (!ReadPod(base, total, cur, a)) return false; out.colliders.push_back(a); }
        uint32_t sn = 0;
        if (!ReadPod(base, total, cur, sn)) return false;
        for (uint32_t i = 0; i < sn; i++) { MapSpawn s; if (!ReadPod(base, total, cur, s)) return false; out.spawns.push_back(s); }
    }
    // Visual section
    {
        size_t cur = h.visualOffset;
        uint32_t mn = 0;
        if (!ReadPod(base, total, cur, mn)) return false;
        for (uint32_t i = 0; i < mn; i++) { MapModelRef m; if (!ReadPod(base, total, cur, m)) return false; out.models.push_back(m); }
        uint32_t ln = 0;
        if (!ReadPod(base, total, cur, ln)) return false;
        for (uint32_t i = 0; i < ln; i++) { MapLight l; if (!ReadPod(base, total, cur, l)) return false; out.lights.push_back(l); }
        if (!ReadPod(base, total, cur, out.env)) return false;
    }
    // Thumbnail (optional)
    out.thumbnail.clear();
    if (h.thumbnailSize > 0 && h.thumbnailOffset + h.thumbnailSize <= total) {
        out.thumbnail.assign(base + h.thumbnailOffset, base + h.thumbnailOffset + h.thumbnailSize);
    }
    return true;
}

} // namespace mapio
