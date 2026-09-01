// Copyright (c) 2026, CafeFPS
// BVH4 Collision Builder for Apex Legends
// Generates game-compatible BVH4 collision data

#pragma once

#include "bvh4_types.h"
#include <string>
#include <memory>

namespace bvh4
{
    //=========================================================================
    // BVH4 Builder Class
    //=========================================================================

    class BVH4Builder
    {
    public:
        BVH4Builder();
        ~BVH4Builder();

        // Configuration
        void SetConfig(const BuildConfig& config);
        const BuildConfig& GetConfig() const { return m_config; }

        // Build from triangle mesh
        // vertices: array of vertex positions
        // triangles: array of triangle definitions
        BuildResult Build(
            const std::vector<float3>& vertices,
            const std::vector<Triangle>& triangles
        );

        // Build with surface properties
        BuildResult BuildWithSurfaceProps(
            const std::vector<float3>& vertices,
            const std::vector<Triangle>& triangles,
            const std::vector<std::string>& surfacePropNames
        );

    private:
        // Tree building
        uint32_t BuildRecursive(
            std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end,
            int depth
        );

        // SAH (Surface Area Heuristic) splitting
        struct SplitResult
        {
            int axis;           // Best split axis (0=X, 1=Y, 2=Z)
            float splitPos;     // Split position
            float cost;         // SAH cost
            uint32_t leftCount; // Triangles on left side
            bool shouldSplit;   // Whether splitting is beneficial
        };

        SplitResult FindBestSplit(
            const std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end,
            const AABB& bounds
        );

        // Partition triangles around split plane
        uint32_t PartitionTriangles(
            std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end,
            int axis,
            float splitPos
        );

        // Create leaf node
        uint32_t CreateLeafNode(
            const std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end
        );

        // Create internal node with up to 4 children
        uint32_t CreateInternalNode(
            uint32_t childIndices[4],
            uint8_t childTypes[4],
            uint8_t childCount,
            const AABB childBounds[4]
        );

        // Collapse binary tree to 4-way
        void CollapseTo4Way(uint32_t nodeIdx, int depth);

        // Vertex buffer optimization for delta encoding
        // Reorders vertices so triangles in each leaf use adjacent vertex indices
        // This is CRITICAL for large models to avoid vertex offset overflow
        void ReorderVerticesForBVH();

        // Collect all leaf nodes in BFS order for vertex reordering
        void CollectLeavesInOrder(uint32_t nodeIdx, std::vector<uint32_t>& leafNodes);

        // Serialization
        void SerializeTree();
        void SerializeNode(uint32_t nodeIdx, uint32_t& outNodeIdx);
        void SerializeLeafData(uint32_t nodeIdx);

        // Vertex encoding
        void ComputeDecodeParams();
        int16_t EncodeCoordinate(float value, float origin, float scale, int axis);
        int16_t EncodeAABBMin(float value, float origin, float scale);
        int16_t EncodeAABBMax(float value, float origin, float scale);

        // Leaf data packing
        void PackPolygonLeaf(
            const std::vector<uint32_t>& triangleIndices,
            uint32_t& leafDataOffset
        );

        // Final output serialization
        void SerializeFinalOutput(BuildResult& result);

        // Utilities
        AABB ComputeBounds(
            const std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end
        );

        AABB ComputeCentroidBounds(
            const std::vector<BuildTriangle>& tris,
            uint32_t start,
            uint32_t end
        );

        float ComputeSAHCost(
            const AABB& bounds,
            const AABB& leftBounds,
            const AABB& rightBounds,
            uint32_t leftCount,
            uint32_t rightCount
        );

    private:
        BuildConfig m_config;

        // Build-time data
        std::vector<float3> m_vertices;
        std::vector<BuildNode> m_buildNodes;
        std::vector<BuildTriangle> m_buildTriangles;
        std::vector<Triangle> m_originalTriangles;  // Unmodified copy for lookup by originalIndex

        // Surface properties
        std::vector<CollSurfProps> m_surfaceProps;
        std::vector<std::string> m_surfacePropNames;

        // Serialized output
        std::vector<CollBvh4Node> m_serializedNodes;
        std::vector<uint32_t> m_leafDataStream;
        std::vector<uint32_t> m_contentsMasks;

        // Decode parameters
        float3 m_decodeOrigin;
        float m_decodeScale;

        // Statistics
        uint32_t m_totalNodes;
        uint32_t m_totalLeaves;
        uint32_t m_maxDepth;
    };

    //=========================================================================
    // Helper Functions
    //=========================================================================

    // Quick build function for simple use cases
    BuildResult QuickBuild(
        const std::vector<float3>& vertices,
        const std::vector<Triangle>& triangles
    );

    // Validate serialized collision data
    bool ValidateCollisionData(const std::vector<uint8_t>& data);

} // namespace bvh4
