// Copyright (c) 2026, CafeFPS
// BVH4 Collision Types for Apex Legends
// Based on reverse engineering of the shipping game

#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cfloat>
#include <algorithm>
#include <cmath>

namespace bvh4
{
    //=========================================================================
    // Basic Types
    //=========================================================================

    struct float3
    {
        float x, y, z;

        float3() : x(0), y(0), z(0) {}
        float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

        float3 operator+(const float3& other) const { return float3(x + other.x, y + other.y, z + other.z); }
        float3 operator-(const float3& other) const { return float3(x - other.x, y - other.y, z - other.z); }
        float3 operator*(float s) const { return float3(x * s, y * s, z * s); }
        float3 operator/(float s) const { return float3(x / s, y / s, z / s); }

        float3& operator+=(const float3& other) { x += other.x; y += other.y; z += other.z; return *this; }
        float3& operator-=(const float3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }

        float dot(const float3& other) const { return x * other.x + y * other.y + z * other.z; }
        float3 cross(const float3& other) const {
            return float3(
                y * other.z - z * other.y,
                z * other.x - x * other.z,
                x * other.y - y * other.x
            );
        }

        float length() const { return std::sqrt(x * x + y * y + z * z); }
        float lengthSqr() const { return x * x + y * y + z * z; }

        float3 normalized() const {
            float len = length();
            if (len > 0.0f) return *this / len;
            return float3(0, 0, 0);
        }

        static float3 minVec(const float3& a, const float3& b) {
            return float3(
                (a.x < b.x) ? a.x : b.x,
                (a.y < b.y) ? a.y : b.y,
                (a.z < b.z) ? a.z : b.z
            );
        }

        static float3 maxVec(const float3& a, const float3& b) {
            return float3(
                (a.x > b.x) ? a.x : b.x,
                (a.y > b.y) ? a.y : b.y,
                (a.z > b.z) ? a.z : b.z
            );
        }
    };

    //=========================================================================
    // Axis-Aligned Bounding Box
    //=========================================================================

    struct AABB
    {
        float3 mins;
        float3 maxs;

        AABB() : mins(FLT_MAX, FLT_MAX, FLT_MAX), maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX) {}
        AABB(const float3& _mins, const float3& _maxs) : mins(_mins), maxs(_maxs) {}

        void expand(const float3& point) {
            mins = float3::minVec(mins, point);
            maxs = float3::maxVec(maxs, point);
        }

        void expand(const AABB& other) {
            mins = float3::minVec(mins, other.mins);
            maxs = float3::maxVec(maxs, other.maxs);
        }

        float3 center() const { return (mins + maxs) * 0.5f; }
        float3 extents() const { return (maxs - mins) * 0.5f; }
        float3 size() const { return maxs - mins; }

        float surfaceArea() const {
            float3 d = size();
            return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
        }

        bool isValid() const {
            return mins.x <= maxs.x && mins.y <= maxs.y && mins.z <= maxs.z;
        }
    };

    //=========================================================================
    // Triangle Primitive
    //=========================================================================

    struct Triangle
    {
        uint32_t indices[3];    // Vertex indices
        uint16_t surfacePropIdx; // Surface property index
        uint16_t flags;          // Triangle flags

        Triangle() : surfacePropIdx(0), flags(0) {
            indices[0] = indices[1] = indices[2] = 0;
        }

        Triangle(uint32_t i0, uint32_t i1, uint32_t i2, uint16_t surfProp = 0)
            : surfacePropIdx(surfProp), flags(0) {
            indices[0] = i0;
            indices[1] = i1;
            indices[2] = i2;
        }
    };

    //=========================================================================
    // BVH4 Leaf Types (from the shipping game)
    //=========================================================================

    // Child type values for BVH4 nodes
    // Type 0 = internal node (recurse to child BVH node)
    // Types 1+ = leaf types
    enum class LeafType : uint8_t
    {
        InternalNode = 0,   // Internal BVH node (NOT a leaf - causes recursion)
        Bvh4 = 1,           // Nested BVH4 sub-tree (special leaf)
        None = 2,           // Empty/placeholder (skipped)
        Bundle = 3,         // Collection of primitives
        Poly_4 = 4,         // Polygon leaf (1-4 polys) - CollPoly_Visit_4
        Poly_5 = 5,         // Polygon leaf (5 polys) - CollPoly_Visit_5
        Poly_6 = 6,         // Polygon leaf (6 polys) - CollPoly_Visit_6
        Poly_7 = 7,         // Polygon leaf (7 polys) - CollPoly_Visit_7
        ConvexHull = 8,     // Convex hull primitive
        StaticProp = 9,     // Reference to static prop (skipped)
        Heightfield = 10,   // Heightfield terrain patch

        MaxLeafTypes = 16
    };

    //=========================================================================
    // Serialized Structures (matching the shipping format)
    //=========================================================================

#pragma pack(push, 1)

    // BVH4 Node - 64 bytes
    // Contains bounds for 4 children and metadata
    struct CollBvh4Node
    {
        // Packed min/max bounds for 4 children
        // Layout: minMax[axis][min/max][child]
        // axis: 0=X, 1=Y, 2=Z
        // min/max: 0=min, 1=max
        int16_t minMax[3][2][4];    // 48 bytes

        // Packed metadata for 4 children
        // bits 0-7: Contents mask index
        // bits 8-31: Child index (node index or leaf data index)
        // Child type is encoded in the lower 4 bits of packedMetaData[2] and [3]
        uint32_t packedMetaData[4]; // 16 bytes
    };
    static_assert(sizeof(CollBvh4Node) == 64, "CollBvh4Node must be 64 bytes");

    // Polygon Leaf Header
    struct CollLeafPolyHeader
    {
        // Upper 4 bits: poly count - 1 (0-15 means 1-16 polys)
        // Lower 12 bits: surface property index and flags
        uint16_t numPolysAndSurfPropIdxAndFlags;
        uint16_t baseVertex; // Base vertex index (shifted left 10 bits when stored)
    };

    // Surface Property
    struct CollSurfProps
    {
        uint16_t surfFlags;      // Surface flags
        uint8_t material;        // Material type
        uint8_t contentsIdx;     // Contents index
        uint32_t nameOffset;     // Offset into name buffer
    };
    static_assert(sizeof(CollSurfProps) == 8, "CollSurfProps must be 8 bytes");

    // Skin Info
    struct CollSkinInfo
    {
        uint8_t surfTypeID[2];   // Surface type IDs
        uint16_t zUpInfo;        // Z-up info
    };
    static_assert(sizeof(CollSkinInfo) == 4, "CollSkinInfo must be 4 bytes");

    // Serialized Part Header (40 bytes)
    struct CollBvhSerializedPart
    {
        uint32_t bvhFlags;       // BVH flags
        uint32_t nodesOfs;       // Offset to nodes array
        uint32_t vertsOfs;       // Offset to vertices
        uint32_t leafDataOfs;    // Offset to leaf data stream
        uint32_t skinInfosOfs;   // Offset to skin info array
        uint8_t skinCount;       // Number of skins
        uint8_t meshGroupCount;  // Number of mesh groups
        uint16_t pad;            // Padding
        float decodeOrigin[3];   // Origin for vertex decoding
        float decodeScale;       // Scale for vertex decoding
    };
    static_assert(sizeof(CollBvhSerializedPart) == 40, "CollBvhSerializedPart must be 40 bytes");

    // Serialized Header
    struct CollBvhSerializedHeader
    {
        int32_t contentsMaskOfs;     // Offset to contents mask array
        int32_t surfPropsOfs;        // Offset to surface properties
        int32_t surfPropNameBufOfs;  // Offset to surface prop name strings
        uint32_t numParts;           // Number of collision parts
        // CollBvhSerializedPart parts[] follows
    };

    // Model collision model structure (matches r5::v8::mstudiocollmodel_t)
    struct mstudiocollmodel_t
    {
        int32_t contentMasksIndex;
        int32_t surfacePropsIndex;
        int32_t surfaceNamesIndex;
        int32_t headerCount;
    };

    // Model collision header (matches r5::v8::mstudiocollheader_t)
    struct mstudiocollheader_t
    {
        int32_t unk;             // Unknown flags
        int32_t bvhNodeIndex;    // Offset to BVH nodes
        int32_t vertIndex;       // Offset to vertices
        int32_t bvhLeafIndex;    // Offset to leaf data
        float origin[3];         // Decode origin
        float scale;             // Decode scale
    };

#pragma pack(pop)

    //=========================================================================
    // Build-time Structures
    //=========================================================================

    // Triangle with precomputed data for building
    struct BuildTriangle
    {
        Triangle tri;
        AABB bounds;
        float3 centroid;
        uint32_t originalIndex;

        void computeBounds(const std::vector<float3>& vertices) {
            bounds = AABB();
            for (int i = 0; i < 3; i++) {
                bounds.expand(vertices[tri.indices[i]]);
            }
            centroid = bounds.center();
        }
    };

    // BVH4 Build Node (intermediate representation)
    struct BuildNode
    {
        AABB bounds;
        uint32_t childIndices[4];   // Index to child nodes or leaf data
        uint8_t childTypes[4];      // 0 = internal node, >0 = leaf type
        uint8_t childCount;         // Number of valid children (0-4)
        bool isLeaf;
        std::vector<uint32_t> triangleIndices; // Triangles in this leaf

        BuildNode() : childCount(0), isLeaf(false) {
            for (int i = 0; i < 4; i++) {
                childIndices[i] = 0;
                childTypes[i] = 0;
            }
        }
    };

    //=========================================================================
    // Build Configuration
    //=========================================================================

    struct BuildConfig
    {
        uint32_t maxTrianglesPerLeaf;  // Maximum triangles per leaf node
        uint32_t minTrianglesForSplit; // Minimum triangles to consider splitting
        float traversalCost;            // Cost of traversing a node
        float intersectionCost;         // Cost of triangle intersection
        uint8_t defaultSurfaceProp;     // Default surface property index
        uint32_t defaultContentsMask;   // Default contents mask

        BuildConfig()
            : maxTrianglesPerLeaf(4)
            , minTrianglesForSplit(2)
            , traversalCost(1.0f)
            , intersectionCost(1.0f)
            , defaultSurfaceProp(0)
            , defaultContentsMask(0x1) // CONTENTS_SOLID
        {}
    };

    //=========================================================================
    // Build Result
    //=========================================================================

    struct BuildResult
    {
        std::vector<uint8_t> data;       // Serialized collision data
        uint32_t nodeCount;              // Number of BVH nodes
        uint32_t leafCount;              // Number of leaf nodes
        uint32_t triangleCount;          // Total triangles
        AABB bounds;                     // Overall bounds
        bool success;
        std::string errorMessage;

        BuildResult() : nodeCount(0), leafCount(0), triangleCount(0), success(false) {}
    };

} // namespace bvh4
