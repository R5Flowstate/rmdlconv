//=============================================================================//
// PHY Parser - Respawn/Valve Physics Collision Format
// Parses PHY format from physics data files (supports TF2 and Apex formats)
//=============================================================================//
#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace collision
{
    //=========================================================================
    // PHY Format Structures
    // Supports both Valve/TF2 format (id=0) and Respawn/Apex format (id=1)
    //=========================================================================

#pragma pack(push, 1)

    // V10/Apex PHY header - 20 bytes
    struct phyheader_t
    {
        int32_t size;            // size of this data structure (20 or 16)
        int32_t id;              // 0 for valve/TF2, 1 for apex's new type
        int32_t solidCount;      // number of surface headers
        int32_t checkSum;        // checksum of source .mdl file
        int32_t propertiesOffset; // offset to key-value string block
    };

    // V19.1/compact PHY header - 4 bytes
    struct phyheader_v16_t
    {
        uint16_t solidCount;      // number of surface headers
        uint16_t propertiesOffset; // offset to key-value string block
    };

    //=========================================================================
    // Valve/TF2 Format Structures (id=0)
    //=========================================================================

    struct swapcompactsurfaceheader_t
    {
        int32_t size;           // Size of this solid's data
        int32_t vphysicsID;     // Physics system ID
        int16_t version;        // Format version
        int16_t modelType;      // Model type flags
        int32_t surfaceSize;    // Total surface data size
        float   dragAxisAreas[3]; // Drag coefficients (x, y, z)
        int32_t axisMapSize;    // Size of axis map data
    };

    struct legacysurfaceheader_t
    {
        float   mass_center[3];      // Center of mass
        float   rotation_inertia[3]; // Rotational inertia tensor
        float   upper_limit_radius;  // Bounding sphere radius
        int32_t packed;              // Packed flags
        int32_t offset_ledgetree_root; // Offset to BVH tree root
        int32_t dummy[3];            // dummy[2] contains ID
    };

    struct trianglefaceheader_t
    {
        int32_t vertexindex; // Offset from this struct to vertex array
        int32_t parent;      // Parent bone ID (1-based, 0 = root)
        int32_t flags;       // Collision flags
        int32_t numfaces;    // Number of triangle faces
    };

    struct trianglevertmap_t
    {
        uint8_t  faceindex;
        uint8_t  padding[3];
        int16_t  vertid;
        uint8_t  padding1[2];
        int16_t  vertid1;
        uint8_t  padding2[2];
        int16_t  vertid2;
        uint8_t  padding3[2];
    };

    struct valve_phyvertex_t
    {
        float pos[3];
        float unk;    // Padding or weight
    };

    // Pointer header - 32 bytes (offsets are relative to start of this struct)
    struct phyptrheader_t
    {
        int64_t solidOffset;    // offset to solidgroup array (from this struct)
        int64_t solidCount;     // number of solid groups
        int64_t unk_0x10;       // unknown
        int64_t solidSize;      // total size of all surface data
    };

    // Solid group - contains bounds and references to solids
    struct solidgroup_t
    {
        int64_t solidOffset;    // offset to solid_t array (from phyptrheader_t)
        int64_t solidCount;     // number of solids in this group

        float   unk_0x10;       // possibly scale

        int32_t unk_0x14[3];    // unknown

        // Per-solid data (5 entries)
        struct {
            float   unk0x0[3];
            int32_t unk_0xC;
        } perSolidData[5];

        // Bounding box
        float bbMin[3];
        float bbMax[3];

        char pad[8];            // padding for 16-byte alignment
    };

    // Solid data - contains vertex/side/edge counts and offsets
    struct solid_t
    {
        float   unk_0x0[3];     // unknown floats
        float   unk_0xC;        // unknown

        int64_t vertOffset;     // offset to vertex array (from phyptrheader_t)
        int64_t vertCount;      // number of vertices

        int64_t sideOffset;     // offset to side array (from phyptrheader_t)
        int64_t sideCount;      // number of sides (faces)

        int64_t edgeOffset;     // offset to edge array (from phyptrheader_t)
        int64_t edgeCount;      // number of edges
    };

    // Side (convex face) - 32 bytes of vertex indices
    struct side_t
    {
        uint8_t vertIndices[32]; // vertex indices for this face (0xFF = unused)
    };

    // Edge - connects two vertices
    struct edge_t
    {
        uint8_t vertIndices[2];  // two vertex indices
    };

    // V10 edge with side references
    struct edge_v10_t
    {
        uint8_t vertIndices[2];  // two vertex indices
        uint8_t sideIndices[2];  // two side indices (faces sharing this edge)
    };

    // Vertex - 3 floats (12 bytes)
    struct phyvertex_t
    {
        float pos[3];
    };

#pragma pack(pop)

    //=========================================================================
    // Parsed PHY Data Structures
    //=========================================================================

    struct PHYTriangle
    {
        uint16_t v0, v1, v2;  // Vertex indices (local to solid)
        uint32_t flags;       // Triangle flags
    };

    struct PHYSolid
    {
        int32_t parentBone;               // Parent bone index (0 = root)
        std::vector<float> vertices;      // Interleaved XYZ vertices
        std::vector<PHYTriangle> triangles;
        float massCenter[3];
        float rotationInertia[3];
        float boundingRadius;
        float bbMin[3];
        float bbMax[3];
    };

    struct ParsedPHYData
    {
        std::vector<PHYSolid> solids;
        int32_t checksum;
        bool valid;
        std::string errorMessage;

        ParsedPHYData() : checksum(0), valid(false) {}
    };

    //=========================================================================
    // PHY Parser Class
    //=========================================================================

    class PHYParser
    {
    public:
        // Parse PHY data from buffer (auto-detects format)
        static ParsedPHYData Parse(const void* phyData, size_t phySize);

        // Parse v10 IVPS format (20-byte header) - Apex format
        static ParsedPHYData ParseV10(const void* phyData, size_t phySize);

        // Parse v16/v19.1 compact format (4-byte header) - Apex format
        static ParsedPHYData ParseV16(const void* phyData, size_t phySize, int32_t checkSum = 0);

        // Parse Valve/TF2 format (id=0 in header)
        static ParsedPHYData ParseValve(const void* phyData, size_t phySize);

        // Build a Valve/IVP (id=0) .phy buffer from parsed convex solids. The S3 engine's
        // model-physics builder branches on the phyheader id: id=0 -> the legacy IVP loader
        // (VCollideLoad) builds REAL CPhysCollide solids; id>=1 (Apex geoms) yields no solids
        // and the server drops every prop_physics/ragdoll ("No physics object ... Removing.").
        // Emitting id=0 with proper IVP compact-surfaces is the root-cause fix. keyValues is the
        // original Apex keyvalue text block (preserved verbatim so mass/constraints survive); may
        // be null/empty. Returns an empty vector if no usable convex solid could be built (caller
        // should then fall back to the legacy id=1 verbatim copy so the convert never fails).
        static std::vector<uint8_t> BuildIVPPhy(const ParsedPHYData& data, int32_t checksum,
                                                const char* keyValues, size_t keyValuesLen);

        // Debug: Print PHY structure info
        static void PrintDebugInfo(const ParsedPHYData& data);

    private:
        // Internal: Parse Respawn/Apex solid data (phyptrheader_t + solidgroup_t + solid_t)
        static bool ParseRespawnSolidData(const uint8_t* ptrHdrStart, size_t dataSize,
                                          ParsedPHYData& result);

        // Internal: Parse Valve/TF2 solid data (swapcompactsurfaceheader_t + triangles)
        static bool ParseValveSolidData(const uint8_t* solidStart, size_t dataSize,
                                        int solidCount, ParsedPHYData& result);

        // Helper: Triangulate a convex face from vertex indices
        static void TriangulateFace(const uint8_t* vertIndices, int vertCount,
                                    std::vector<PHYTriangle>& outTriangles);

        // Helper: Find maximum vertex index in triangle list
        static int32_t FindMaxVertexIndex(const std::vector<PHYTriangle>& triangles);
    };

} // namespace collision
