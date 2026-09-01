// Copyright (c) 2026, CafeFPS
// Collision Integration for rmdlconv model pipeline
// Example integration showing how to generate and attach collision to models

#pragma once

#include "collision_generator.h"
#include "../studio/studio.h"

namespace collision
{
    //=========================================================================
    // Integration helpers for rmdlconv model pipeline
    //=========================================================================

    // Generate collision data from VG (VertexGroup) mesh data
    // This is the primary integration point for models converted from other formats
    //
    // Usage in model conversion:
    //   1. Load/convert mesh geometry
    //   2. Call GenerateCollisionFromVG() with the vertex/index data
    //   3. Append collision data to model file
    //   4. Update studiohdr bvhOffset field
    //
    inline GenerationResult GenerateCollisionFromVG(
        const void* vertexBuffer,
        uint32_t vertexCount,
        uint32_t vertexStride,
        const uint16_t* indexBuffer,
        uint32_t indexCount,
        const GeneratorConfig& config = GeneratorConfig())
    {
        CollisionGenerator generator;
        generator.SetConfig(config);

        // Standard VG vertex format has position at offset 0
        // Position is typically 3 floats (12 bytes)
        return generator.GenerateFromVG(
            vertexBuffer,
            vertexCount,
            vertexStride,
            0,  // Position offset
            indexBuffer,
            indexCount
        );
    }

    // Generate collision for a model with multiple mesh groups
    // Combines all meshes into a single collision tree
    struct MeshGroup
    {
        const void* vertexBuffer;
        uint32_t vertexCount;
        uint32_t vertexStride;
        const uint16_t* indexBuffer;
        uint32_t indexCount;
    };

    inline GenerationResult GenerateCollisionFromMeshGroups(
        const std::vector<MeshGroup>& meshGroups,
        const GeneratorConfig& config = GeneratorConfig())
    {
        // Combine all mesh groups into a single mesh
        MeshData combinedMesh;
        uint32_t vertexOffset = 0;

        for (const auto& group : meshGroups)
        {
            const uint8_t* vertexData = static_cast<const uint8_t*>(group.vertexBuffer);

            // Add vertices
            for (uint32_t i = 0; i < group.vertexCount; i++)
            {
                const float* pos = reinterpret_cast<const float*>(
                    vertexData + (i * group.vertexStride)
                );

                MeshVertex v;
                v.x = pos[0];
                v.y = pos[1];
                v.z = pos[2];
                v.nx = v.ny = v.nz = 0;
                combinedMesh.vertices.push_back(v);
            }

            // Add triangles with adjusted indices
            uint32_t triangleCount = group.indexCount / 3;
            for (uint32_t i = 0; i < triangleCount; i++)
            {
                MeshTriangle t;
                t.v0 = group.indexBuffer[i * 3 + 0] + vertexOffset;
                t.v1 = group.indexBuffer[i * 3 + 1] + vertexOffset;
                t.v2 = group.indexBuffer[i * 3 + 2] + vertexOffset;
                t.surfaceProp = 0;
                t.flags = 0;
                combinedMesh.triangles.push_back(t);
            }

            vertexOffset += group.vertexCount;
        }

        combinedMesh.surfacePropNames.push_back(config.defaultSurfaceProp);

        CollisionGenerator generator;
        generator.SetConfig(config);
        return generator.Generate(combinedMesh);
    }

    //=========================================================================
    // Model file integration
    //=========================================================================

    // Write collision data to model output buffer
    // Returns the size of collision data written
    inline size_t WriteCollisionToModel(
        const GenerationResult& collision,
        uint8_t* outputBuffer,
        size_t outputOffset)
    {
        if (!collision.success || collision.collisionData.empty())
        {
            return 0;
        }

        memcpy(outputBuffer + outputOffset,
               collision.collisionData.data(),
               collision.collisionData.size());

        return collision.collisionData.size();
    }

    // Calculate the studiohdr bvhOffset value
    // The offset is relative to the base of the studiohdr
    inline int32_t CalculateBvhOffset(size_t modelBaseOffset, size_t collisionOffset)
    {
        return static_cast<int32_t>(collisionOffset - modelBaseOffset);
    }

    //=========================================================================
    // Example integration code (documentation)
    //=========================================================================

    /*
    Example: Adding collision to a converted model

    void ConvertModelWithCollision(...)
    {
        // ... existing model conversion code ...

        // After mesh data is ready:
        collision::GeneratorConfig collConfig;
        collConfig.maxTrianglesPerLeaf = 4;
        collConfig.defaultSurfaceProp = "default";

        collision::GenerationResult collResult = collision::GenerateCollisionFromVG(
            vertexBuffer,       // From VG file
            vertexCount,
            vertexStride,       // Usually 48 for standard VG format
            indexBuffer,        // From VG file
            indexCount,
            collConfig
        );

        if (collResult.success)
        {
            // Write collision data at current output position
            size_t collisionOffset = g_model.pData - g_model.pBase;
            memcpy(g_model.pData, collResult.collisionData.data(), collResult.collisionData.size());
            g_model.pData += collResult.collisionData.size();

            // Update studiohdr
            g_model.hdrV54()->bvhOffset = collisionOffset;

            printf("Generated BVH4 collision: %u nodes, %u leaves, %u triangles\n",
                collResult.nodeCount, collResult.leafCount, collResult.triangleCount);
        }
        else
        {
            printf("Failed to generate collision: %s\n", collResult.errorMessage.c_str());
        }

        // ... continue with model finalization ...
    }
    */

} // namespace collision
