// Copyright (c) 2026, CafeFPS
// Collision Generator Interface for rmdlconv
// Generates BVH4 collision data from model mesh geometry

#pragma once

#include "bvh4_builder.h"
#include <string>
#include <vector>

namespace collision
{
    //=========================================================================
    // Mesh Input Structure
    //=========================================================================

    struct MeshVertex
    {
        float x, y, z;      // Position
        float nx, ny, nz;   // Normal (optional, for collision flags)
    };

    struct MeshTriangle
    {
        uint32_t v0, v1, v2;    // Vertex indices
        uint16_t surfaceProp;   // Surface property index
        uint16_t flags;         // Triangle flags
    };

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<MeshTriangle> triangles;
        std::vector<std::string> surfacePropNames;
    };

    //=========================================================================
    // Generator Configuration
    //=========================================================================

    struct GeneratorConfig
    {
        uint32_t maxTrianglesPerLeaf;   // 4-7 recommended
        bool generateForAllLODs;         // Generate collision for all LODs
        bool useFirstLODOnly;            // Only use LOD0 for collision
        std::string defaultSurfaceProp;  // Default surface property name
        uint32_t contentsMask;           // Contents mask (CONTENTS_SOLID = 1)

        GeneratorConfig()
            : maxTrianglesPerLeaf(4)
            , generateForAllLODs(false)
            , useFirstLODOnly(true)
            , defaultSurfaceProp("default")
            , contentsMask(0x1)  // CONTENTS_SOLID
        {}
    };

    //=========================================================================
    // Generation Result
    //=========================================================================

    struct GenerationResult
    {
        std::vector<uint8_t> collisionData;  // Serialized BVH4 data
        uint32_t nodeCount;
        uint32_t leafCount;
        uint32_t triangleCount;
        float boundsMin[3];
        float boundsMax[3];
        bool success;
        std::string errorMessage;

        GenerationResult()
            : nodeCount(0)
            , leafCount(0)
            , triangleCount(0)
            , success(false)
        {
            boundsMin[0] = boundsMin[1] = boundsMin[2] = 0;
            boundsMax[0] = boundsMax[1] = boundsMax[2] = 0;
        }
    };

    //=========================================================================
    // Collision Generator Class
    //=========================================================================

    class CollisionGenerator
    {
    public:
        CollisionGenerator();
        ~CollisionGenerator();

        // Configuration
        void SetConfig(const GeneratorConfig& config);
        const GeneratorConfig& GetConfig() const { return m_config; }

        // Generate from mesh data
        GenerationResult Generate(const MeshData& mesh);

        // Generate from raw arrays (for integration with existing code)
        GenerationResult Generate(
            const float* vertices,      // xyz triplets
            uint32_t vertexCount,
            const uint32_t* indices,    // triangle indices
            uint32_t triangleCount,
            const uint16_t* surfaceProps = nullptr  // per-triangle surface props
        );

        // Generate from VG (VertexGroup) data format
        // This is the format used by rmdlconv
        GenerationResult GenerateFromVG(
            const void* vertexBuffer,
            uint32_t vertexCount,
            uint32_t vertexStride,      // Typically 48 or larger for VG vertices
            uint32_t positionOffset,    // Offset to position within vertex
            const uint16_t* indexBuffer,
            uint32_t indexCount
        );

    private:
        GeneratorConfig m_config;
        bvh4::BVH4Builder m_builder;

        // Convert various input formats to standard mesh data
        MeshData ConvertToMeshData(
            const float* vertices,
            uint32_t vertexCount,
            const uint32_t* indices,
            uint32_t triangleCount,
            const uint16_t* surfaceProps
        );

        MeshData ConvertVGToMeshData(
            const void* vertexBuffer,
            uint32_t vertexCount,
            uint32_t vertexStride,
            uint32_t positionOffset,
            const uint16_t* indexBuffer,
            uint32_t indexCount
        );
    };

    //=========================================================================
    // Utility Functions
    //=========================================================================

    // Quick generation function for simple use cases
    GenerationResult QuickGenerate(
        const float* vertices,
        uint32_t vertexCount,
        const uint32_t* indices,
        uint32_t triangleCount
    );

    // Check if collision data is valid
    bool ValidateCollisionData(const std::vector<uint8_t>& data);

    // Get collision data statistics
    void GetCollisionStats(
        const std::vector<uint8_t>& data,
        uint32_t& outNodeCount,
        uint32_t& outLeafCount,
        float outBoundsMin[3],
        float outBoundsMax[3]
    );

} // namespace collision
