// Copyright (c) 2026, CafeFPS
// Collision Generator Implementation for rmdlconv

#include "collision_generator.h"
#include <cstring>
#include <algorithm>

namespace collision
{
    //=========================================================================
    // CollisionGenerator Implementation
    //=========================================================================

    CollisionGenerator::CollisionGenerator()
    {
    }

    CollisionGenerator::~CollisionGenerator()
    {
    }

    void CollisionGenerator::SetConfig(const GeneratorConfig& config)
    {
        m_config = config;

        // Apply config to builder
        bvh4::BuildConfig buildConfig;
        buildConfig.maxTrianglesPerLeaf = config.maxTrianglesPerLeaf;
        buildConfig.defaultContentsMask = config.contentsMask;
        m_builder.SetConfig(buildConfig);
    }

    GenerationResult CollisionGenerator::Generate(const MeshData& mesh)
    {
        GenerationResult result;

        if (mesh.vertices.empty() || mesh.triangles.empty())
        {
            result.errorMessage = "Empty mesh data";
            return result;
        }

        // Convert to BVH4 format
        std::vector<bvh4::float3> bvhVertices;
        std::vector<bvh4::Triangle> bvhTriangles;

        bvhVertices.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices)
        {
            bvhVertices.push_back(bvh4::float3(v.x, v.y, v.z));
        }

        bvhTriangles.reserve(mesh.triangles.size());
        for (const auto& t : mesh.triangles)
        {
            bvh4::Triangle tri(t.v0, t.v1, t.v2, t.surfaceProp);
            tri.flags = t.flags;
            bvhTriangles.push_back(tri);
        }

        // Get surface property names
        std::vector<std::string> surfaceProps = mesh.surfacePropNames;
        if (surfaceProps.empty())
        {
            surfaceProps.push_back(m_config.defaultSurfaceProp);
        }

        // Build BVH4
        bvh4::BuildResult buildResult = m_builder.BuildWithSurfaceProps(
            bvhVertices,
            bvhTriangles,
            surfaceProps
        );

        // Convert result
        result.success = buildResult.success;
        result.errorMessage = buildResult.errorMessage;
        result.collisionData = std::move(buildResult.data);
        result.nodeCount = buildResult.nodeCount;
        result.leafCount = buildResult.leafCount;
        result.triangleCount = buildResult.triangleCount;

        if (buildResult.success)
        {
            result.boundsMin[0] = buildResult.bounds.mins.x;
            result.boundsMin[1] = buildResult.bounds.mins.y;
            result.boundsMin[2] = buildResult.bounds.mins.z;
            result.boundsMax[0] = buildResult.bounds.maxs.x;
            result.boundsMax[1] = buildResult.bounds.maxs.y;
            result.boundsMax[2] = buildResult.bounds.maxs.z;
        }

        return result;
    }

    GenerationResult CollisionGenerator::Generate(
        const float* vertices,
        uint32_t vertexCount,
        const uint32_t* indices,
        uint32_t triangleCount,
        const uint16_t* surfaceProps)
    {
        MeshData mesh = ConvertToMeshData(
            vertices, vertexCount,
            indices, triangleCount,
            surfaceProps
        );

        return Generate(mesh);
    }

    GenerationResult CollisionGenerator::GenerateFromVG(
        const void* vertexBuffer,
        uint32_t vertexCount,
        uint32_t vertexStride,
        uint32_t positionOffset,
        const uint16_t* indexBuffer,
        uint32_t indexCount)
    {
        MeshData mesh = ConvertVGToMeshData(
            vertexBuffer, vertexCount,
            vertexStride, positionOffset,
            indexBuffer, indexCount
        );

        return Generate(mesh);
    }

    MeshData CollisionGenerator::ConvertToMeshData(
        const float* vertices,
        uint32_t vertexCount,
        const uint32_t* indices,
        uint32_t triangleCount,
        const uint16_t* surfaceProps)
    {
        MeshData mesh;

        // Copy vertices
        mesh.vertices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; i++)
        {
            mesh.vertices[i].x = vertices[i * 3 + 0];
            mesh.vertices[i].y = vertices[i * 3 + 1];
            mesh.vertices[i].z = vertices[i * 3 + 2];
            mesh.vertices[i].nx = 0;
            mesh.vertices[i].ny = 0;
            mesh.vertices[i].nz = 1;
        }

        // Copy triangles
        mesh.triangles.resize(triangleCount);
        for (uint32_t i = 0; i < triangleCount; i++)
        {
            mesh.triangles[i].v0 = indices[i * 3 + 0];
            mesh.triangles[i].v1 = indices[i * 3 + 1];
            mesh.triangles[i].v2 = indices[i * 3 + 2];
            mesh.triangles[i].surfaceProp = surfaceProps ? surfaceProps[i] : 0;
            mesh.triangles[i].flags = 0;
        }

        // Default surface property
        mesh.surfacePropNames.push_back(m_config.defaultSurfaceProp);

        return mesh;
    }

    MeshData CollisionGenerator::ConvertVGToMeshData(
        const void* vertexBuffer,
        uint32_t vertexCount,
        uint32_t vertexStride,
        uint32_t positionOffset,
        const uint16_t* indexBuffer,
        uint32_t indexCount)
    {
        MeshData mesh;

        const uint8_t* vertexData = static_cast<const uint8_t*>(vertexBuffer);

        // Extract vertices from VG format
        mesh.vertices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; i++)
        {
            const float* pos = reinterpret_cast<const float*>(
                vertexData + (i * vertexStride) + positionOffset
            );

            mesh.vertices[i].x = pos[0];
            mesh.vertices[i].y = pos[1];
            mesh.vertices[i].z = pos[2];
            mesh.vertices[i].nx = 0;
            mesh.vertices[i].ny = 0;
            mesh.vertices[i].nz = 1;
        }

        // Convert 16-bit indices to triangles
        uint32_t triangleCount = indexCount / 3;
        mesh.triangles.resize(triangleCount);
        for (uint32_t i = 0; i < triangleCount; i++)
        {
            mesh.triangles[i].v0 = indexBuffer[i * 3 + 0];
            mesh.triangles[i].v1 = indexBuffer[i * 3 + 1];
            mesh.triangles[i].v2 = indexBuffer[i * 3 + 2];
            mesh.triangles[i].surfaceProp = 0;
            mesh.triangles[i].flags = 0;
        }

        // Default surface property
        mesh.surfacePropNames.push_back(m_config.defaultSurfaceProp);

        return mesh;
    }

    //=========================================================================
    // Utility Functions
    //=========================================================================

    GenerationResult QuickGenerate(
        const float* vertices,
        uint32_t vertexCount,
        const uint32_t* indices,
        uint32_t triangleCount)
    {
        CollisionGenerator generator;
        return generator.Generate(vertices, vertexCount, indices, triangleCount, nullptr);
    }

    bool ValidateCollisionData(const std::vector<uint8_t>& data)
    {
        return bvh4::ValidateCollisionData(data);
    }

    void GetCollisionStats(
        const std::vector<uint8_t>& data,
        uint32_t& outNodeCount,
        uint32_t& outLeafCount,
        float outBoundsMin[3],
        float outBoundsMax[3])
    {
        outNodeCount = 0;
        outLeafCount = 0;
        outBoundsMin[0] = outBoundsMin[1] = outBoundsMin[2] = 0;
        outBoundsMax[0] = outBoundsMax[1] = outBoundsMax[2] = 0;

        if (data.size() < sizeof(bvh4::CollBvhSerializedHeader) + sizeof(bvh4::CollBvhSerializedPart))
        {
            return;
        }

        const bvh4::CollBvhSerializedHeader* header =
            reinterpret_cast<const bvh4::CollBvhSerializedHeader*>(data.data());

        if (header->numParts == 0)
        {
            return;
        }

        const bvh4::CollBvhSerializedPart* part =
            reinterpret_cast<const bvh4::CollBvhSerializedPart*>(
                data.data() + sizeof(bvh4::CollBvhSerializedHeader)
            );

        // Calculate node count from nodes data size
        // nodesOfs points to the nodes array
        if (part->nodesOfs > 0 && part->vertsOfs > part->nodesOfs)
        {
            outNodeCount = (part->vertsOfs - part->nodesOfs) / sizeof(bvh4::CollBvh4Node);
        }

        // Get bounds from decode parameters (approximate)
        outBoundsMin[0] = part->decodeOrigin[0];
        outBoundsMin[1] = part->decodeOrigin[1];
        outBoundsMin[2] = part->decodeOrigin[2];

        // Max bounds would need to be computed from the actual node data
        // For now, just use origin + scale * 32767 as approximation
        float maxExtent = part->decodeScale * 32767.0f;
        outBoundsMax[0] = outBoundsMin[0] + maxExtent;
        outBoundsMax[1] = outBoundsMin[1] + maxExtent;
        outBoundsMax[2] = outBoundsMin[2] + maxExtent;
    }

} // namespace collision
