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

    GenerationResult CollisionGenerator::GenerateParts(const std::vector<MeshData>& parts)
    {
        GenerationResult result;
        if (parts.empty())
        {
            result.errorMessage = "No collision parts";
            return result;
        }

        struct Built { std::vector<uint8_t> blob; int32_t nodes, verts, leaf; float decode[4]; };
        std::vector<Built> built;
        for (size_t i = 0; i < parts.size(); i++)
        {
            GenerationResult one = Generate(parts[i]);
            if (!one.success)
            {
                result.errorMessage = "part " + std::to_string(i) + ": " + one.errorMessage;
                return result;
            }
            Built b;
            b.blob = std::move(one.collisionData);
            const int32_t* hdr = reinterpret_cast<const int32_t*>(b.blob.data() + 16);
            b.nodes = hdr[1]; b.verts = hdr[2]; b.leaf = hdr[3];
            memcpy(b.decode, b.blob.data() + 32, sizeof(b.decode));
            result.nodeCount += one.nodeCount;
            result.leafCount += one.leafCount;
            result.triangleCount += one.triangleCount;
            built.push_back(std::move(b));
        }

        // Surface props / contents / names are identical across parts: take part 0's block.
        const int32_t* cm0 = reinterpret_cast<const int32_t*>(built[0].blob.data());
        const size_t oldTable = 16 + 32;
        const size_t newTable = 16 + 32 * built.size();
        const size_t sharedSize = static_cast<size_t>(built[0].verts) - oldTable;
        const int32_t shift = static_cast<int32_t>(newTable) - static_cast<int32_t>(oldTable);

        auto align64 = [](size_t v) { return (v + 63) & ~static_cast<size_t>(63); };
        std::vector<uint8_t>& out = result.collisionData;
        out.assign(newTable, 0);
        out.insert(out.end(), built[0].blob.begin() + oldTable, built[0].blob.begin() + oldTable + sharedSize);

        int32_t* cm = reinterpret_cast<int32_t*>(out.data());
        cm[0] = cm0[0] + shift;
        cm[1] = cm0[1] + shift;
        cm[2] = cm0[2] + shift;
        cm[3] = static_cast<int32_t>(built.size());

        std::vector<size_t> vOfs(built.size()), lOfs(built.size()), nOfs(built.size());
        for (size_t i = 0; i < built.size(); i++)
        {
            const Built& b = built[i];
            out.resize(align64(out.size()), 0);
            vOfs[i] = out.size();
            out.insert(out.end(), b.blob.begin() + b.verts, b.blob.begin() + b.leaf);
            out.resize(align64(out.size()), 0);
            lOfs[i] = out.size();
            out.insert(out.end(), b.blob.begin() + b.leaf, b.blob.begin() + b.nodes);
        }
        for (size_t i = 0; i < built.size(); i++)
        {
            const Built& b = built[i];
            out.resize(align64(out.size()), 0);
            nOfs[i] = out.size();
            out.insert(out.end(), b.blob.begin() + b.nodes, b.blob.end());
        }

        for (size_t i = 0; i < built.size(); i++)
        {
            int32_t* hdr = reinterpret_cast<int32_t*>(out.data() + 16 + 32 * i);
            hdr[0] = 1;
            hdr[1] = static_cast<int32_t>(nOfs[i]);
            hdr[2] = static_cast<int32_t>(vOfs[i]);
            hdr[3] = static_cast<int32_t>(lOfs[i]);
            memcpy(out.data() + 16 + 32 * i + 16, built[i].decode, sizeof(built[i].decode));
        }

        result.success = true;
        return result;
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
