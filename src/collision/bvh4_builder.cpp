// Copyright (c) 2026, CafeFPS
// BVH4 Collision Builder Implementation
// Generates game-compatible BVH4 collision data for Apex Legends

#include "bvh4_builder.h"
#include <algorithm>
#include <cstring>
#include <cassert>
#include <limits>
#include <cmath>
#include <unordered_map>

namespace bvh4
{
    //=========================================================================
    // Constants
    //=========================================================================

    static constexpr uint32_t MAX_TRIANGLES_PER_LEAF = 7;
    static constexpr uint32_t SAH_BIN_COUNT = 16;
    static constexpr float SAH_EPSILON = 1e-6f;

    //=========================================================================
    // BVH4Builder Implementation
    //=========================================================================

    BVH4Builder::BVH4Builder()
        : m_decodeScale(1.0f / 32767.0f)
        , m_totalNodes(0)
        , m_totalLeaves(0)
        , m_maxDepth(0)
    {
    }

    BVH4Builder::~BVH4Builder()
    {
    }

    void BVH4Builder::SetConfig(const BuildConfig& config)
    {
        m_config = config;
    }

    BuildResult BVH4Builder::Build(
        const std::vector<float3>& vertices,
        const std::vector<Triangle>& triangles)
    {
        std::vector<std::string> defaultSurfProps;
        defaultSurfProps.push_back("default");
        return BuildWithSurfaceProps(vertices, triangles, defaultSurfProps);
    }

    BuildResult BVH4Builder::BuildWithSurfaceProps(
        const std::vector<float3>& vertices,
        const std::vector<Triangle>& triangles,
        const std::vector<std::string>& surfacePropNames)
    {
        BuildResult result;

        if (vertices.empty() || triangles.empty())
        {
            result.errorMessage = "Empty input data";
            return result;
        }

        // Store input data
        m_vertices = vertices;
        m_surfacePropNames = surfacePropNames;

        // Initialize surface properties
        m_surfaceProps.clear();
        for (size_t i = 0; i < surfacePropNames.size(); i++)
        {
            CollSurfProps prop;
            prop.surfFlags = 0;
            prop.material = 0;
            prop.contentsIdx = 0;
            prop.nameOffset = 0; // Will be computed during serialization
            m_surfaceProps.push_back(prop);
        }

        // Default contents mask (CONTENTS_SOLID)
        m_contentsMasks.clear();
        m_contentsMasks.push_back(m_config.defaultContentsMask);

        // Store original triangles for lookup (m_buildTriangles gets reordered during build)
        m_originalTriangles = triangles;

        // Prepare build triangles with precomputed data
        m_buildTriangles.clear();
        m_buildTriangles.reserve(triangles.size());

        result.bounds = AABB();

        for (size_t i = 0; i < triangles.size(); i++)
        {
            BuildTriangle bt;
            bt.tri = triangles[i];
            bt.originalIndex = static_cast<uint32_t>(i);
            bt.computeBounds(m_vertices);

            // Validate triangle
            if (!bt.bounds.isValid())
            {
                continue; // Skip degenerate triangles
            }

            result.bounds.expand(bt.bounds);
            m_buildTriangles.push_back(bt);
        }

        if (m_buildTriangles.empty())
        {
            result.errorMessage = "No valid triangles after filtering";
            return result;
        }

        // Compute decode parameters from overall bounds
        ComputeDecodeParams();

        // Clear build data
        m_buildNodes.clear();
        m_buildNodes.reserve(m_buildTriangles.size() * 2);

        m_totalNodes = 0;
        m_totalLeaves = 0;
        m_maxDepth = 0;

        // Build binary BVH first
        uint32_t rootIdx = BuildRecursive(
            m_buildTriangles,
            0,
            static_cast<uint32_t>(m_buildTriangles.size()),
            0
        );

        // Collapse to 4-way BVH
        CollapseTo4Way(rootIdx, 0);

        // CRITICAL: Reorder vertices for optimal delta encoding
        // This ensures triangles in each leaf use adjacent vertex indices,
        // preventing vertex offset overflow for large models
        ReorderVerticesForBVH();

        // Serialize the tree
        SerializeTree();

        // Build final output
        result.nodeCount = static_cast<uint32_t>(m_serializedNodes.size());
        result.leafCount = m_totalLeaves;
        result.triangleCount = static_cast<uint32_t>(m_buildTriangles.size());
        result.success = true;

        // Serialize to final byte stream
        SerializeFinalOutput(result);

        return result;
    }

    //=========================================================================
    // Tree Building
    //=========================================================================

    uint32_t BVH4Builder::BuildRecursive(
        std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end,
        int depth)
    {
        uint32_t count = end - start;
        m_maxDepth = std::max(m_maxDepth, static_cast<uint32_t>(depth));

        // Compute bounds for this range
        AABB bounds = ComputeBounds(tris, start, end);

        // Check if we should create a leaf
        if (count <= m_config.maxTrianglesPerLeaf || depth > 64)
        {
            return CreateLeafNode(tris, start, end);
        }

        // Find best split using SAH
        SplitResult split = FindBestSplit(tris, start, end, bounds);

        // If split isn't beneficial, create a leaf
        if (!split.shouldSplit || split.leftCount == 0 || split.leftCount == count)
        {
            // Force split if too many triangles
            if (count > MAX_TRIANGLES_PER_LEAF)
            {
                // Fallback: split at median
                split.axis = 0;
                float3 size = bounds.size();
                if (size.y > size.x) split.axis = 1;
                if (size.z > (split.axis == 0 ? size.x : size.y)) split.axis = 2;

                // Sort by centroid and split at median
                std::sort(tris.begin() + start, tris.begin() + end,
                    [axis = split.axis](const BuildTriangle& a, const BuildTriangle& b) {
                        float ca = (axis == 0) ? a.centroid.x : (axis == 1) ? a.centroid.y : a.centroid.z;
                        float cb = (axis == 0) ? b.centroid.x : (axis == 1) ? b.centroid.y : b.centroid.z;
                        return ca < cb;
                    });
                split.leftCount = count / 2;
            }
            else
            {
                return CreateLeafNode(tris, start, end);
            }
        }

        // Partition triangles
        uint32_t mid = PartitionTriangles(tris, start, end, split.axis, split.splitPos);

        // Handle edge cases
        if (mid == start) mid = start + 1;
        if (mid == end) mid = end - 1;

        // Recurse
        uint32_t leftChild = BuildRecursive(tris, start, mid, depth + 1);
        uint32_t rightChild = BuildRecursive(tris, mid, end, depth + 1);

        // Create internal node
        uint32_t childIndices[4] = { leftChild, rightChild, 0, 0 };
        uint8_t childTypes[4] = { 0, 0, 0, 0 }; // Internal nodes initially

        if (m_buildNodes[leftChild].isLeaf)
            childTypes[0] = static_cast<uint8_t>(LeafType::Poly_4);
        if (m_buildNodes[rightChild].isLeaf)
            childTypes[1] = static_cast<uint8_t>(LeafType::Poly_4);

        AABB childBounds[4] = {
            m_buildNodes[leftChild].bounds,
            m_buildNodes[rightChild].bounds,
            AABB(),
            AABB()
        };

        return CreateInternalNode(childIndices, childTypes, 2, childBounds);
    }

    //=========================================================================
    // SAH Splitting
    //=========================================================================

    BVH4Builder::SplitResult BVH4Builder::FindBestSplit(
        const std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end,
        const AABB& bounds)
    {
        SplitResult best;
        best.axis = 0;
        best.splitPos = 0;
        best.cost = std::numeric_limits<float>::max();
        best.leftCount = 0;
        best.shouldSplit = false;

        uint32_t count = end - start;

        // Compute centroid bounds
        AABB centroidBounds = ComputeCentroidBounds(tris, start, end);

        // Cost of not splitting (creating a leaf)
        float leafCost = m_config.intersectionCost * count;

        // Try each axis
        for (int axis = 0; axis < 3; axis++)
        {
            float axisMin = (axis == 0) ? centroidBounds.mins.x :
                           (axis == 1) ? centroidBounds.mins.y : centroidBounds.mins.z;
            float axisMax = (axis == 0) ? centroidBounds.maxs.x :
                           (axis == 1) ? centroidBounds.maxs.y : centroidBounds.maxs.z;

            float axisExtent = axisMax - axisMin;
            if (axisExtent < SAH_EPSILON) continue;

            // Binned SAH
            struct Bin
            {
                AABB bounds;
                uint32_t count = 0;
            };
            Bin bins[SAH_BIN_COUNT];

            float scale = SAH_BIN_COUNT / axisExtent;

            // Assign triangles to bins
            for (uint32_t i = start; i < end; i++)
            {
                float centroid = (axis == 0) ? tris[i].centroid.x :
                                (axis == 1) ? tris[i].centroid.y : tris[i].centroid.z;
                int binIdx = static_cast<int>((centroid - axisMin) * scale);
                if (binIdx >= static_cast<int>(SAH_BIN_COUNT)) binIdx = SAH_BIN_COUNT - 1;
                if (binIdx < 0) binIdx = 0;
                bins[binIdx].bounds.expand(tris[i].bounds);
                bins[binIdx].count++;
            }

            // Sweep from left to compute costs
            AABB leftBounds[SAH_BIN_COUNT - 1];
            uint32_t leftCounts[SAH_BIN_COUNT - 1];

            AABB runningBounds;
            uint32_t runningCount = 0;

            for (int i = 0; i < SAH_BIN_COUNT - 1; i++)
            {
                runningBounds.expand(bins[i].bounds);
                runningCount += bins[i].count;
                leftBounds[i] = runningBounds;
                leftCounts[i] = runningCount;
            }

            // Sweep from right and compute SAH costs
            runningBounds = AABB();
            runningCount = 0;

            for (int i = SAH_BIN_COUNT - 1; i > 0; i--)
            {
                runningBounds.expand(bins[i].bounds);
                runningCount += bins[i].count;

                if (leftCounts[i - 1] > 0 && runningCount > 0)
                {
                    float cost = ComputeSAHCost(
                        bounds,
                        leftBounds[i - 1],
                        runningBounds,
                        leftCounts[i - 1],
                        runningCount
                    );

                    if (cost < best.cost)
                    {
                        best.cost = cost;
                        best.axis = axis;
                        best.splitPos = axisMin + (i * axisExtent / SAH_BIN_COUNT);
                        best.leftCount = leftCounts[i - 1];
                    }
                }
            }
        }

        // Only split if it's cheaper than a leaf
        best.shouldSplit = (best.cost < leafCost);

        return best;
    }

    uint32_t BVH4Builder::PartitionTriangles(
        std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end,
        int axis,
        float splitPos)
    {
        auto it = std::partition(tris.begin() + start, tris.begin() + end,
            [axis, splitPos](const BuildTriangle& t) {
                float centroid = (axis == 0) ? t.centroid.x :
                                (axis == 1) ? t.centroid.y : t.centroid.z;
                return centroid < splitPos;
            });

        return static_cast<uint32_t>(it - tris.begin());
    }

    //=========================================================================
    // Node Creation
    //=========================================================================

    uint32_t BVH4Builder::CreateLeafNode(
        const std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end)
    {
        BuildNode node;
        node.isLeaf = true;
        node.childCount = 0;
        node.bounds = ComputeBounds(tris, start, end);

        for (uint32_t i = start; i < end; i++)
        {
            node.triangleIndices.push_back(tris[i].originalIndex);
        }

        uint32_t nodeIdx = static_cast<uint32_t>(m_buildNodes.size());
        m_buildNodes.push_back(node);
        m_totalLeaves++;

        return nodeIdx;
    }

    uint32_t BVH4Builder::CreateInternalNode(
        uint32_t childIndices[4],
        uint8_t childTypes[4],
        uint8_t childCount,
        const AABB childBounds[4])
    {
        BuildNode node;
        node.isLeaf = false;
        node.childCount = childCount;
        node.bounds = AABB();

        for (int i = 0; i < 4; i++)
        {
            node.childIndices[i] = childIndices[i];
            node.childTypes[i] = childTypes[i];
            if (i < childCount)
            {
                node.bounds.expand(childBounds[i]);
            }
        }

        uint32_t nodeIdx = static_cast<uint32_t>(m_buildNodes.size());
        m_buildNodes.push_back(node);
        m_totalNodes++;

        return nodeIdx;
    }

    //=========================================================================
    // 4-Way Collapse
    //=========================================================================

    void BVH4Builder::CollapseTo4Way(uint32_t nodeIdx, int depth)
    {
        if (nodeIdx >= m_buildNodes.size()) return;

        BuildNode& node = m_buildNodes[nodeIdx];
        if (node.isLeaf) return;

        // Recursively collapse children first
        for (int i = 0; i < node.childCount; i++)
        {
            if (node.childTypes[i] == 0) // Internal node
            {
                CollapseTo4Way(node.childIndices[i], depth + 1);
            }
        }

        // Try to collapse grandchildren into this node
        // Only collapse if we have room (less than 4 children) and child is internal
        while (node.childCount < 4)
        {
            int bestChild = -1;
            float bestArea = 0;

            // Find the internal child with the largest surface area
            for (int i = 0; i < node.childCount; i++)
            {
                if (node.childTypes[i] == 0) // Internal node
                {
                    BuildNode& child = m_buildNodes[node.childIndices[i]];
                    if (!child.isLeaf && child.childCount == 2)
                    {
                        float area = child.bounds.surfaceArea();
                        if (area > bestArea)
                        {
                            bestArea = area;
                            bestChild = i;
                        }
                    }
                }
            }

            if (bestChild < 0) break;

            // Collapse this child's children into our node
            BuildNode& child = m_buildNodes[node.childIndices[bestChild]];

            // We need room for (child.childCount - 1) more slots
            if (node.childCount + child.childCount - 1 > 4) break;

            // Replace child with its first child
            uint32_t grandChild0 = child.childIndices[0];
            uint8_t grandType0 = child.childTypes[0];

            node.childIndices[bestChild] = grandChild0;
            node.childTypes[bestChild] = grandType0;

            // Add remaining grandchildren
            for (int i = 1; i < child.childCount; i++)
            {
                node.childIndices[node.childCount] = child.childIndices[i];
                node.childTypes[node.childCount] = child.childTypes[i];
                node.childCount++;
            }
        }
    }

    //=========================================================================
    // Vertex Buffer Optimization
    //=========================================================================

    void BVH4Builder::CollectLeavesInOrder(uint32_t nodeIdx, std::vector<uint32_t>& leafNodes)
    {
        // Breadth-first traversal to collect leaves in optimal order
        std::vector<uint32_t> queue;
        queue.push_back(nodeIdx);

        while (!queue.empty())
        {
            uint32_t currentIdx = queue.front();
            queue.erase(queue.begin());

            if (currentIdx >= m_buildNodes.size())
                continue;

            BuildNode& node = m_buildNodes[currentIdx];

            if (node.isLeaf)
            {
                leafNodes.push_back(currentIdx);
            }
            else
            {
                // Add children to queue (BFS order)
                for (int i = 0; i < node.childCount; i++)
                {
                    queue.push_back(node.childIndices[i]);
                }
            }
        }
    }

    void BVH4Builder::ReorderVerticesForBVH()
    {
        // This function DUPLICATES vertices per leaf so each leaf has its own
        // contiguous range of vertex indices. This guarantees no vertex offset
        // overflow in the delta-encoded leaf polygon format.
        //
        // The delta encoding limits are:
        // - v0_offset: 11 bits = max 2047
        // - v1/v2_offset: 9 bits = max 511
        //
        // By giving each leaf its own vertices, the max span per leaf is just
        // the number of unique vertices in that leaf (typically < 50).

        if (m_vertices.empty() || m_originalTriangles.empty() || m_buildNodes.empty())
        {
            printf("  [BVH4] WARNING: Empty data, skipping vertex reorder\n");
            return;
        }

        // Step 1: Collect all leaf nodes in BFS order
        std::vector<uint32_t> leafNodes;
        leafNodes.reserve(m_totalLeaves);

        uint32_t rootIdx = static_cast<uint32_t>(m_buildNodes.size() - 1);
        CollectLeavesInOrder(rootIdx, leafNodes);

        // Step 2: Build new vertex buffer with duplicated vertices per leaf
        std::vector<float3> newVertices;
        newVertices.reserve(m_vertices.size() * 2); // Estimate with duplication

        uint32_t maxVertexSpan = 0;

        // Step 3: Process each leaf - give it its own vertex range
        for (uint32_t leafIdx : leafNodes)
        {
            BuildNode& leaf = m_buildNodes[leafIdx];
            if (!leaf.isLeaf || leaf.triangleIndices.empty())
                continue;

            // Create a LOCAL remap for just this leaf's vertices
            std::unordered_map<uint32_t, uint32_t> leafVertexRemap;
            uint32_t leafBaseVertex = static_cast<uint32_t>(newVertices.size());

            // First pass: collect all unique vertices used by this leaf
            for (uint32_t triOrigIdx : leaf.triangleIndices)
            {
                Triangle& tri = m_originalTriangles[triOrigIdx];
                for (int v = 0; v < 3; v++)
                {
                    uint32_t oldIdx = tri.indices[v];
                    if (leafVertexRemap.find(oldIdx) == leafVertexRemap.end())
                    {
                        // New vertex for this leaf - add to buffer
                        uint32_t newIdx = static_cast<uint32_t>(newVertices.size());
                        leafVertexRemap[oldIdx] = newIdx;
                        newVertices.push_back(m_vertices[oldIdx]);
                    }
                }
            }

            // Second pass: update triangle indices to use new local indices
            for (uint32_t triOrigIdx : leaf.triangleIndices)
            {
                Triangle& tri = m_originalTriangles[triOrigIdx];
                for (int v = 0; v < 3; v++)
                {
                    tri.indices[v] = leafVertexRemap[tri.indices[v]];
                }
            }

            // Track max span for this leaf
            uint32_t leafVertexCount = static_cast<uint32_t>(leafVertexRemap.size());
            maxVertexSpan = std::max(maxVertexSpan, leafVertexCount);
        }

        // Step 4: Replace vertex buffer with new duplicated version
        size_t originalCount = m_vertices.size();
        m_vertices = std::move(newVertices);

        // Report results
        printf("  [BVH4] Vertex duplication: %zu -> %zu vertices (%.1fx), max span %u\n",
               originalCount, m_vertices.size(),
               (float)m_vertices.size() / originalCount,
               maxVertexSpan);

        if (maxVertexSpan > 511)
        {
            printf("  [BVH4] WARNING: max span %u may cause v1/v2 overflow\n", maxVertexSpan);
        }
    }

    //=========================================================================
    // Serialization
    //=========================================================================

    void BVH4Builder::ComputeDecodeParams()
    {
        AABB totalBounds;
        for (const auto& tri : m_buildTriangles)
        {
            totalBounds.expand(tri.bounds);
        }

        // Origin should be at CENTER of bounds, not at mins
        // This allows AABBs to span negative to positive int16 values
        // which is how the the shipping collision data is structured
        m_decodeOrigin.x = (totalBounds.mins.x + totalBounds.maxs.x) * 0.5f;
        m_decodeOrigin.y = (totalBounds.mins.y + totalBounds.maxs.y) * 0.5f;
        m_decodeOrigin.z = (totalBounds.mins.z + totalBounds.maxs.z) * 0.5f;

        // Compute scale to fit bounds in int16 range
        // Since origin is centered, we need to fit HALF the extent in 32767
        float3 size = totalBounds.size();
        float maxExtent = std::max({ size.x, size.y, size.z });

        // m_decodeScale is the ACTUAL scale used for encoding:
        //   int16 = (value - origin) / m_decodeScale
        //
        // WORKING CONFIGURATION (has holes, but collision partially works)
        // This fits geometry in int16 range [-32767, 32767]

        m_decodeScale = maxExtent / 32000.0f;
        if (m_decodeScale < 1e-6f)
        {
            m_decodeScale = 1.0f / 32767.0f;
        }
    }

    int16_t BVH4Builder::EncodeCoordinate(float value, float origin, float scale, int /*axis*/)
    {
        float normalized = (value - origin) / scale;
        // Clamp to int16 range and round to nearest
        normalized = std::max(-32767.0f, std::min(32767.0f, normalized));
        return static_cast<int16_t>(std::round(normalized));
    }

    // Encode AABB min coordinate with epsilon shrink to avoid zero-width boxes
    int16_t BVH4Builder::EncodeAABBMin(float value, float origin, float scale)
    {
        float normalized = (value - origin) / scale;
        // For min bounds, subtract small epsilon to ensure min < max for thin AABBs
        normalized -= 1.0f; // 1 unit in encoded space
        normalized = std::max(-32767.0f, std::min(32766.0f, normalized));
        return static_cast<int16_t>(std::round(normalized));
    }

    // Encode AABB max coordinate with epsilon expand to avoid zero-width boxes
    int16_t BVH4Builder::EncodeAABBMax(float value, float origin, float scale)
    {
        float normalized = (value - origin) / scale;
        // For max bounds, add small epsilon to ensure min < max for thin AABBs
        normalized += 1.0f; // 1 unit in encoded space
        normalized = std::max(-32766.0f, std::min(32767.0f, normalized));
        return static_cast<int16_t>(std::round(normalized));
    }

    void BVH4Builder::SerializeTree()
    {
        m_serializedNodes.clear();
        m_leafDataStream.clear();

        if (m_buildNodes.empty()) return;

        // Process nodes breadth-first
        std::vector<uint32_t> nodeQueue;
        std::vector<uint32_t> nodeMapping(m_buildNodes.size(), UINT32_MAX);

        nodeQueue.push_back(static_cast<uint32_t>(m_buildNodes.size() - 1)); // Root is last

        while (!nodeQueue.empty())
        {
            uint32_t buildIdx = nodeQueue.front();
            nodeQueue.erase(nodeQueue.begin());

            BuildNode& node = m_buildNodes[buildIdx];

            if (node.isLeaf) continue;

            uint32_t serialIdx = static_cast<uint32_t>(m_serializedNodes.size());
            nodeMapping[buildIdx] = serialIdx;

            CollBvh4Node serialNode;
            memset(&serialNode, 0, sizeof(serialNode));

            // Encode bounds for each child
            for (int i = 0; i < 4; i++)
            {
                if (i < node.childCount)
                {
                    AABB childBounds;
                    if (node.childTypes[i] == 0)
                    {
                        childBounds = m_buildNodes[node.childIndices[i]].bounds;
                        nodeQueue.push_back(node.childIndices[i]);
                    }
                    else
                    {
                        childBounds = m_buildNodes[node.childIndices[i]].bounds;
                    }

                    // Encode min bounds (with epsilon shrink to avoid zero-width AABBs)
                    serialNode.minMax[0][0][i] = EncodeAABBMin(childBounds.mins.x, m_decodeOrigin.x, m_decodeScale);
                    serialNode.minMax[1][0][i] = EncodeAABBMin(childBounds.mins.y, m_decodeOrigin.y, m_decodeScale);
                    serialNode.minMax[2][0][i] = EncodeAABBMin(childBounds.mins.z, m_decodeOrigin.z, m_decodeScale);

                    // Encode max bounds (with epsilon expand to avoid zero-width AABBs)
                    serialNode.minMax[0][1][i] = EncodeAABBMax(childBounds.maxs.x, m_decodeOrigin.x, m_decodeScale);
                    serialNode.minMax[1][1][i] = EncodeAABBMax(childBounds.maxs.y, m_decodeOrigin.y, m_decodeScale);
                    serialNode.minMax[2][1][i] = EncodeAABBMax(childBounds.maxs.z, m_decodeOrigin.z, m_decodeScale);
                }
                else
                {
                    // Empty slot - will be filled with duplicate child data in second pass
                    // Initialize to zero for now, will be overwritten
                    serialNode.minMax[0][0][i] = 0;
                    serialNode.minMax[1][0][i] = 0;
                    serialNode.minMax[2][0][i] = 0;
                    serialNode.minMax[0][1][i] = 0;
                    serialNode.minMax[1][1][i] = 0;
                    serialNode.minMax[2][1][i] = 0;
                }
            }

            m_serializedNodes.push_back(serialNode);
        }

        // Second pass: fill in child indices and pack leaf data
        for (uint32_t buildIdx = 0; buildIdx < m_buildNodes.size(); buildIdx++)
        {
            if (nodeMapping[buildIdx] == UINT32_MAX) continue;

            BuildNode& node = m_buildNodes[buildIdx];
            CollBvh4Node& serialNode = m_serializedNodes[nodeMapping[buildIdx]];

            // First, collect all child indices and types
            // IMPORTANT: type=2 is never used for empty slots.
            // Instead, we'll duplicate the last valid child to fill empty slots.
            // NOTE: polyCount is stored in leaf data header, not in node metadata
            uint32_t childIndices[4] = {0, 0, 0, 0};
            uint8_t childTypes[4] = {0, 0, 0, 0};

            // Track the last LEAF child for duplication into empty slots
            // We only want to duplicate leaves, not internal nodes (which would cause
            // the same subtree to be visited multiple times)
            int lastLeafChild = -1;

            for (int i = 0; i < node.childCount; i++)
            {
                if (node.childTypes[i] == 0 && !m_buildNodes[node.childIndices[i]].isLeaf)
                {
                    // Internal child - type 0 means internal node (NOT a leaf)
                    // The game recurses when childType == 0
                    // NOTE: Don't update lastLeafChild here - we only want to duplicate leaves
                    childIndices[i] = nodeMapping[node.childIndices[i]];
                    childTypes[i] = 0; // Internal node type (causes recursion in traversal)
                }
                else
                {
                    // Leaf child - pack leaf data
                    BuildNode& leafNode = m_buildNodes[node.childIndices[i]];

                    // Determine leaf type based on triangle count
                    uint32_t triCount = static_cast<uint32_t>(leafNode.triangleIndices.size());

                    if (triCount == 0)
                    {
                        // Empty leaf - will be filled with duplicate later
                        // Mark as needing fill (we'll handle after the loop)
                        childTypes[i] = 0xFF; // Marker for "needs fill"
                    }
                    else
                    {
                        // Valid leaf with triangles
                        childIndices[i] = static_cast<uint32_t>(m_leafDataStream.size());

                        // IMPORTANT: Types 5/6/7 use 6-byte vertices (int16 with decode)
                        // Type 4 uses 12-byte vertices (raw floats, no decode)
                        // Since we store int16 encoded vertices, we MUST use types 5/6/7
                        // Use type 5 for all polygon leaves since our vertices are int16 encoded
                        // Note: polyCount is stored in leaf data header, not in node metadata
                        childTypes[i] = static_cast<uint8_t>(LeafType::Poly_5);

                        PackPolygonLeaf(leafNode.triangleIndices, childIndices[i]);
                        lastLeafChild = i;
                    }
                }
            }

            // Fill empty slots (childCount < 4) by duplicating last LEAF child
            // This is how the shipping data works - no empty slots, just duplicated children
            // The duplicate will have the same AABB so it won't cause extra work
            // IMPORTANT: Only duplicate leaves, not internal nodes!
            if (lastLeafChild >= 0)
            {
                for (int i = node.childCount; i < 4; i++)
                {
                    childIndices[i] = childIndices[lastLeafChild];
                    childTypes[i] = childTypes[lastLeafChild];

                    // Also copy AABB from last leaf child
                    serialNode.minMax[0][0][i] = serialNode.minMax[0][0][lastLeafChild];
                    serialNode.minMax[1][0][i] = serialNode.minMax[1][0][lastLeafChild];
                    serialNode.minMax[2][0][i] = serialNode.minMax[2][0][lastLeafChild];
                    serialNode.minMax[0][1][i] = serialNode.minMax[0][1][lastLeafChild];
                    serialNode.minMax[1][1][i] = serialNode.minMax[1][1][lastLeafChild];
                    serialNode.minMax[2][1][i] = serialNode.minMax[2][1][lastLeafChild];
                }

                // Also fill any marked empty children (type=0xFF)
                for (int i = 0; i < node.childCount; i++)
                {
                    if (childTypes[i] == 0xFF)
                    {
                        childIndices[i] = childIndices[lastLeafChild];
                        childTypes[i] = childTypes[lastLeafChild];

                        serialNode.minMax[0][0][i] = serialNode.minMax[0][0][lastLeafChild];
                        serialNode.minMax[1][0][i] = serialNode.minMax[1][0][lastLeafChild];
                        serialNode.minMax[2][0][i] = serialNode.minMax[2][0][lastLeafChild];
                        serialNode.minMax[0][1][i] = serialNode.minMax[0][1][lastLeafChild];
                        serialNode.minMax[1][1][i] = serialNode.minMax[1][1][lastLeafChild];
                        serialNode.minMax[2][1][i] = serialNode.minMax[2][1][lastLeafChild];
                    }
                }
            }
            else if (node.childCount < 4)
            {
                // No leaf children found - this node only has internal children
                // We need to duplicate the last internal child instead
                int lastChild = node.childCount - 1;
                for (int i = node.childCount; i < 4; i++)
                {
                    childIndices[i] = childIndices[lastChild];
                    childTypes[i] = childTypes[lastChild];

                    serialNode.minMax[0][0][i] = serialNode.minMax[0][0][lastChild];
                    serialNode.minMax[1][0][i] = serialNode.minMax[1][0][lastChild];
                    serialNode.minMax[2][0][i] = serialNode.minMax[2][0][lastChild];
                    serialNode.minMax[0][1][i] = serialNode.minMax[0][1][lastChild];
                    serialNode.minMax[1][1][i] = serialNode.minMax[1][1][lastChild];
                    serialNode.minMax[2][1][i] = serialNode.minMax[2][1][lastChild];
                }
            }

            // Pack metadata:
            // packedMetaData[0]: bits 0-7 = contentsMaskIdx, bits 8-31 = childIdx[0]
            // packedMetaData[1]: bits 0-7 = contentsMaskIdx, bits 8-31 = childIdx[1]
            // packedMetaData[2]: bits 0-3 = childType[0], bits 4-7 = childType[1], bits 8-31 = childIdx[2]
            // packedMetaData[3]: bits 0-3 = childType[2], bits 4-7 = childType[3], bits 8-31 = childIdx[3]
            //
            // NOTE: polyCount is stored in the LEAF DATA header, not in node metadata!
            uint8_t contentsMaskIdx = 0; // Default contents mask index
            serialNode.packedMetaData[0] = (childIndices[0] << 8) | contentsMaskIdx;
            serialNode.packedMetaData[1] = (childIndices[1] << 8) | contentsMaskIdx;
            serialNode.packedMetaData[2] = (childIndices[2] << 8) | ((childTypes[1] & 0xF) << 4) | (childTypes[0] & 0xF);
            serialNode.packedMetaData[3] = (childIndices[3] << 8) | ((childTypes[3] & 0xF) << 4) | (childTypes[2] & 0xF);
        }
    }

    void BVH4Builder::PackPolygonLeaf(
        const std::vector<uint32_t>& triangleIndices,
        uint32_t& leafDataOffset)
    {
        if (triangleIndices.empty()) return;

        // Set the leaf data offset BEFORE adding data - this is the DWORD index
        // where this leaf's data will start in the stream
        leafDataOffset = static_cast<uint32_t>(m_leafDataStream.size());

        // LEAF DATA FORMAT (CollLeafPoly_s structure):
        //
        // Header (4 bytes total = 1 DWORD):
        //   uint16_t numPolysAndSurfPropIdxAndFlags:
        //     bits 12-15: polyCount - 1 (4 bits, supports 1-16 polys)
        //     bits 0-11:  surfacePropIdx and flags
        //   uint16_t baseVertex:
        //     Stored value = actual_base_vertex >> 10 (shifted right by 10)
        //
        // Polygon DWORDs (one per triangle):
        //   bits 0-10:  v0 offset from (baseVertex << 10) - 11 bits
        //   bits 11-19: v1_offset (v1 = v0 + offset + 1) - 9 bits
        //   bits 20-28: v2_offset (v2 = v0 + offset + 1) - 9 bits
        //   bits 29-31: flags - 3 bits

        // Find min vertex index to use as base
        uint32_t minVertIdx = UINT32_MAX;
        for (uint32_t triIdx : triangleIndices)
        {
            const Triangle& tri = m_originalTriangles[triIdx];
            minVertIdx = std::min(minVertIdx, tri.indices[0]);
            minVertIdx = std::min(minVertIdx, tri.indices[1]);
            minVertIdx = std::min(minVertIdx, tri.indices[2]);
        }

        // Base vertex is stored shifted right by 10
        // So actual base for decoding = storedBase << 10
        // We align minVertIdx down to nearest 1024 boundary
        uint32_t baseVertex = (minVertIdx >> 10) << 10;  // Align to 1024
        uint16_t storedBaseVertex = static_cast<uint16_t>(minVertIdx >> 10);

        // Build header DWORD
        uint32_t polyCount = static_cast<uint32_t>(triangleIndices.size());
        uint16_t surfPropIdx = 0;  // Default surface property
        uint16_t headerWord0 = static_cast<uint16_t>(((polyCount - 1) << 12) | (surfPropIdx & 0xFFF));
        uint16_t headerWord1 = storedBaseVertex;

        uint32_t headerDword = (static_cast<uint32_t>(headerWord1) << 16) | headerWord0;
        m_leafDataStream.push_back(headerDword);

        // Pack polygon DWORDs
        // IMPORTANT: v0_offset is CUMULATIVE! Each polygon's v0 offset is a delta
        // from the previous polygon's v0, not an absolute offset from baseVertex.
        // The game decodes: running_v0 += v0_offset; v1 = running_v0 + v1_offset + 1; etc.
        //
        // For this to work, triangles must be sorted by their minimum vertex index!
        // Otherwise we'd get negative deltas that wrap around to large unsigned values.

        // Create a list of triangle indices sorted by minimum vertex
        struct TriSortEntry {
            uint32_t triIdx;
            uint32_t minVert;
        };
        std::vector<TriSortEntry> sortedTris;
        sortedTris.reserve(triangleIndices.size());

        for (uint32_t triIdx : triangleIndices)
        {
            const Triangle& tri = m_originalTriangles[triIdx];
            uint32_t minV = std::min({tri.indices[0], tri.indices[1], tri.indices[2]});
            sortedTris.push_back({triIdx, minV});
        }

        // Sort by minimum vertex index
        std::sort(sortedTris.begin(), sortedTris.end(),
            [](const TriSortEntry& a, const TriSortEntry& b) {
                return a.minVert < b.minVert;
            });

        uint32_t runningV0 = baseVertex;  // Start at baseVertex, accumulates with each polygon

        for (const auto& entry : sortedTris)
        {
            uint32_t triIdx = entry.triIdx;
            const Triangle& tri = m_originalTriangles[triIdx];

            // CRITICAL: Rotate triangle to put smallest vertex first while preserving winding
            // Format requires v0 < v1 and v0 < v2, but we must NOT change winding order!
            // Original winding: (v0, v1, v2) defines the triangle's normal direction
            //
            // Find which position has the smallest index
            uint32_t v0_idx = tri.indices[0];
            uint32_t v1_idx = tri.indices[1];
            uint32_t v2_idx = tri.indices[2];

            uint32_t indices[3];
            if (v0_idx <= v1_idx && v0_idx <= v2_idx) {
                // v0 is already smallest - keep original order
                indices[0] = v0_idx;
                indices[1] = v1_idx;
                indices[2] = v2_idx;
            } else if (v1_idx <= v0_idx && v1_idx <= v2_idx) {
                // v1 is smallest - rotate left: (v1, v2, v0)
                indices[0] = v1_idx;
                indices[1] = v2_idx;
                indices[2] = v0_idx;
            } else {
                // v2 is smallest - rotate right: (v2, v0, v1)
                indices[0] = v2_idx;
                indices[1] = v0_idx;
                indices[2] = v1_idx;
            }

            uint32_t v0 = indices[0];
            uint32_t v1 = indices[1];
            uint32_t v2 = indices[2];

            // v0_offset is the DELTA from the running v0 position
            // Game decodes: runningV0 = runningV0 + v0_offset
            uint32_t v0Offset = v0 - runningV0;

            // Update running v0 for next polygon
            runningV0 = v0;

            // v1 and v2 are encoded as offsets from v0
            // Game decodes: v1 = v0 + v1_offset + 1, v2 = v0 + v2_offset + 1
            uint32_t v1Offset = v1 - v0 - 1;
            uint32_t v2Offset = v2 - v0 - 1;

            // Clamp to bit widths: v0=11 bits, v1/v2=9 bits each
            v0Offset = std::min(v0Offset, 0x7FFu);  // 11 bits max = 2047
            v1Offset = std::min(v1Offset, 0x1FFu);  // 9 bits max = 511
            v2Offset = std::min(v2Offset, 0x1FFu);  // 9 bits max = 511

            // Flags in high 3 bits (bits 29-31)
            // Use triangle flags from source mesh (default 0x7 for solid collision)
            uint32_t flags = (tri.flags != 0) ? (tri.flags & 0x7) : 0x7;

            // Pack polygon: (flags << 29) | (v2_offset << 20) | (v1_offset << 11) | v0_offset
            uint32_t packedPoly = (flags << 29) | (v2Offset << 20) | (v1Offset << 11) | v0Offset;

            m_leafDataStream.push_back(packedPoly);
        }
    }

    void BVH4Builder::SerializeFinalOutput(BuildResult& result)
    {
        // Model collision format (matching r5::v8) - exact order matters!
        // - mstudiocollmodel_t (16 bytes): contentMasksIndex, surfacePropsIndex, surfaceNamesIndex, headerCount
        // - mstudiocollheader_t[headerCount] (32 bytes each): unk, bvhNodeIndex, vertIndex, bvhLeafIndex, origin[3], scale
        // - Surface properties (dsurfaceproperty_t array)
        // - Content masks (uint32_t array)
        // - Surface names (null-terminated strings)
        // - Vertices (ALIGN64)
        // - Leaf data (ALIGN64)
        // - BVH nodes (ALIGN64) - nodes come LAST

        // Structure sizes
        const size_t collModelSize = 16;  // mstudiocollmodel_t
        const size_t collHeaderSize = 32; // mstudiocollheader_t
        const size_t surfPropSize = 8;    // dsurfaceproperty_t

        size_t nodesSize = m_serializedNodes.size() * sizeof(CollBvh4Node);
        size_t vertsSize = m_vertices.size() * 6;  // 3 x int16 = 6 bytes per vertex
        size_t leafDataSize = m_leafDataStream.size() * sizeof(uint32_t);
        size_t contentsMaskSize = m_contentsMasks.size() * sizeof(uint32_t);
        size_t surfPropsSize = m_surfaceProps.size() * surfPropSize;

        // Calculate surface prop name buffer size
        // IMPORTANT: The surfaceNamesIndex must point to an int32 = 0 first (offset array),
        // then the actual string data follows. Shipped models always have this first int = 0.
        // The template rmdl.bt:680 asserts: Assert(surfaceNameOffset[0] == 0);
        size_t surfPropNameBufSize = sizeof(int32_t);  // Start with offset placeholder (must be 0)
        for (const auto& name : m_surfacePropNames)
        {
            surfPropNameBufSize += name.size() + 1;
        }

        // Calculate offsets (all relative to start of collision data = mstudiocollmodel_t base)
        size_t currentOffset = collModelSize + collHeaderSize; // 1 header for now

        size_t surfPropsOfs = currentOffset;
        currentOffset += surfPropsSize;

        size_t contentsMaskOfs = currentOffset;
        currentOffset += contentsMaskSize;

        size_t surfNamesOfs = currentOffset;
        currentOffset += surfPropNameBufSize;

        // Align to 64 bytes for vertices
        currentOffset = (currentOffset + 63) & ~63;
        size_t vertsOfs = currentOffset;
        currentOffset += vertsSize;

        // Align to 64 bytes for leaf data
        currentOffset = (currentOffset + 63) & ~63;
        size_t leafDataOfs = currentOffset;
        currentOffset += leafDataSize;

        // Align to 64 bytes for BVH nodes (nodes come LAST)
        currentOffset = (currentOffset + 63) & ~63;
        size_t nodesOfs = currentOffset;
        currentOffset += nodesSize;

        // Total size
        size_t totalSize = currentOffset;

        // Allocate output buffer
        result.data.resize(totalSize);
        uint8_t* data = result.data.data();
        memset(data, 0, totalSize);

        // Write mstudiocollmodel_t
        int32_t* collModel = reinterpret_cast<int32_t*>(data);
        collModel[0] = static_cast<int32_t>(contentsMaskOfs);  // contentMasksIndex
        collModel[1] = static_cast<int32_t>(surfPropsOfs);     // surfacePropsIndex
        collModel[2] = static_cast<int32_t>(surfNamesOfs);     // surfaceNamesIndex
        collModel[3] = 1;                                       // headerCount

        // Write mstudiocollheader_t
        uint8_t* collHeader = data + collModelSize;
        int32_t* headerInts = reinterpret_cast<int32_t*>(collHeader);
        headerInts[0] = 1;                                      // flags (the shipping data uses 0x1)
        headerInts[1] = static_cast<int32_t>(nodesOfs);        // bvhNodeIndex
        headerInts[2] = static_cast<int32_t>(vertsOfs);        // vertIndex
        headerInts[3] = static_cast<int32_t>(leafDataOfs);     // bvhLeafIndex

        float* headerFloats = reinterpret_cast<float*>(collHeader + 16);
        headerFloats[0] = m_decodeOrigin.x;                    // origin[0]
        headerFloats[1] = m_decodeOrigin.y;                    // origin[1]
        headerFloats[2] = m_decodeOrigin.z;                    // origin[2]
        // Game decode: value = origin + int16 * (storedScale * 65536)
        // CRITICAL: Store scale divided by 65536 to match Respawn's fixed-point encoding
        // But vertices are encoded with FULL scale, game multiplies stored scale by 65536 when decoding
        float storedScale = m_decodeScale / 65536.0f;
        headerFloats[3] = storedScale;                         // scale


        // Write surface properties (dsurfaceproperty_t format)
        uint8_t* surfPropPtr = data + surfPropsOfs;
        // Name offset starts at 4 because strings come after the int32=0 placeholder
        // at surfaceNamesIndex. The offset is relative to surfaceNamesIndex.
        uint32_t nameOffset = sizeof(int32_t);
        for (size_t i = 0; i < m_surfaceProps.size(); i++)
        {
            int16_t* prop = reinterpret_cast<int16_t*>(surfPropPtr + i * surfPropSize);
            prop[0] = 0;                                        // unk (surfFlags)
            uint8_t* propBytes = reinterpret_cast<uint8_t*>(surfPropPtr + i * surfPropSize);
            propBytes[2] = m_surfaceProps[i].material;         // surfacePropId
            propBytes[3] = 0;                                   // contentMaskOffset
            int32_t* propNameOfs = reinterpret_cast<int32_t*>(surfPropPtr + i * surfPropSize + 4);
            *propNameOfs = static_cast<int32_t>(nameOffset);   // surfaceNameOffset (relative to surfNamesOfs)
            nameOffset += static_cast<uint32_t>(m_surfacePropNames[i].size() + 1);
        }

        // Write contents masks
        memcpy(data + contentsMaskOfs, m_contentsMasks.data(), contentsMaskSize);

        // Write surface property name offset array and names
        // CRITICAL: First write int32 = 0 at surfaceNamesIndex to satisfy template assertion
        // The template rmdl.bt:680 expects: Assert(surfaceNameOffset[0] == 0);
        int32_t* surfNameOffsetPtr = reinterpret_cast<int32_t*>(data + surfNamesOfs);
        *surfNameOffsetPtr = 0;  // First entry must be 0 for template compatibility

        // Write actual string data after the offset placeholder
        char* namePtr = reinterpret_cast<char*>(data + surfNamesOfs + sizeof(int32_t));
        for (const auto& name : m_surfacePropNames)
        {
            memcpy(namePtr, name.c_str(), name.size() + 1);
            namePtr += name.size() + 1;
        }

        // Write vertices as int16 triplets (6 bytes per vertex)
        // Format: [int16 x][int16 y][int16 z] for each vertex
        // Encode: int16 = (vertex - origin) / m_decodeScale
        // Game decode: vertex = origin + int16 * (storedScale * 65536)
        //                     = origin + int16 * m_decodeScale  ✓
        int16_t* vertPtr = reinterpret_cast<int16_t*>(data + vertsOfs);
        for (size_t i = 0; i < m_vertices.size(); i++)
        {
            const float3& v = m_vertices[i];
            vertPtr[i * 3 + 0] = EncodeCoordinate(v.x, m_decodeOrigin.x, m_decodeScale, 0);
            vertPtr[i * 3 + 1] = EncodeCoordinate(v.y, m_decodeOrigin.y, m_decodeScale, 1);
            vertPtr[i * 3 + 2] = EncodeCoordinate(v.z, m_decodeOrigin.z, m_decodeScale, 2);
        }

        // Write leaf data (comes after vertices, before nodes)
        memcpy(data + leafDataOfs, m_leafDataStream.data(), leafDataSize);

        // Write BVH nodes (comes LAST)
        memcpy(data + nodesOfs, m_serializedNodes.data(), nodesSize);
    }

    //=========================================================================
    // Utilities
    //=========================================================================

    AABB BVH4Builder::ComputeBounds(
        const std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end)
    {
        AABB bounds;
        for (uint32_t i = start; i < end; i++)
        {
            bounds.expand(tris[i].bounds);
        }
        return bounds;
    }

    AABB BVH4Builder::ComputeCentroidBounds(
        const std::vector<BuildTriangle>& tris,
        uint32_t start,
        uint32_t end)
    {
        AABB bounds;
        for (uint32_t i = start; i < end; i++)
        {
            bounds.expand(tris[i].centroid);
        }
        return bounds;
    }

    float BVH4Builder::ComputeSAHCost(
        const AABB& bounds,
        const AABB& leftBounds,
        const AABB& rightBounds,
        uint32_t leftCount,
        uint32_t rightCount)
    {
        float parentArea = bounds.surfaceArea();
        if (parentArea < SAH_EPSILON) return std::numeric_limits<float>::max();

        float leftArea = leftBounds.surfaceArea();
        float rightArea = rightBounds.surfaceArea();

        float cost = m_config.traversalCost +
                    (leftArea / parentArea) * leftCount * m_config.intersectionCost +
                    (rightArea / parentArea) * rightCount * m_config.intersectionCost;

        return cost;
    }

    //=========================================================================
    // Helper Functions
    //=========================================================================

    BuildResult QuickBuild(
        const std::vector<float3>& vertices,
        const std::vector<Triangle>& triangles)
    {
        BVH4Builder builder;
        return builder.Build(vertices, triangles);
    }

    bool ValidateCollisionData(const std::vector<uint8_t>& data)
    {
        if (data.size() < sizeof(CollBvhSerializedHeader) + sizeof(CollBvhSerializedPart))
        {
            return false;
        }

        const CollBvhSerializedHeader* header =
            reinterpret_cast<const CollBvhSerializedHeader*>(data.data());

        // Basic validation
        if (header->numParts == 0 || header->numParts > 16)
        {
            return false;
        }

        if (header->contentsMaskOfs < 0 || header->surfPropsOfs < 0)
        {
            return false;
        }

        return true;
    }

} // namespace bvh4
