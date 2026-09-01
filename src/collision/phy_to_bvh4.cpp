//=============================================================================//
// PHY to BVH4 Converter Implementation
//=============================================================================//

#include "phy_to_bvh4.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace collision
{
    // Validation statistics
    struct ConversionStats
    {
        int totalTrianglesInput = 0;
        int totalTrianglesOutput = 0;
        int skippedInvalidIndices = 0;
        int degenerateTriangles = 0;
        int flippedNormals = 0;
        float minVertexDist = FLT_MAX;
        float maxVertexDist = 0.0f;
        float avgTriangleArea = 0.0f;

        void Print() const
        {
            printf("\n=== PHY to BVH4 Conversion Statistics ===\n");
            printf("  Input triangles:  %d\n", totalTrianglesInput);
            printf("  Output triangles: %d\n", totalTrianglesOutput);
            printf("  Skipped (invalid indices): %d\n", skippedInvalidIndices);
            printf("  Degenerate triangles: %d\n", degenerateTriangles);
            printf("  Vertex distance range: %.6f to %.6f\n", minVertexDist, maxVertexDist);
            printf("  Average triangle area: %.6f\n", avgTriangleArea);
            printf("=========================================\n\n");
        }
    };

    // Calculate triangle area
    static float CalculateTriangleArea(const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2)
    {
        // Edge vectors
        float ax = v1.x - v0.x;
        float ay = v1.y - v0.y;
        float az = v1.z - v0.z;

        float bx = v2.x - v0.x;
        float by = v2.y - v0.y;
        float bz = v2.z - v0.z;

        // Cross product
        float cx = ay * bz - az * by;
        float cy = az * bx - ax * bz;
        float cz = ax * by - ay * bx;

        // Half magnitude of cross product is triangle area
        return 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);
    }

    // Check if triangle is degenerate
    // For collision meshes with /65536 scaling, vertices are ~0.001 units apart
    // Areas will be ~1e-7 to 1e-8, so use much smaller epsilon
    static bool IsTriangleDegenerate(const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2, float epsilon = 1e-9f)
    {
        float area = CalculateTriangleArea(v0, v1, v2);
        return area < epsilon;
    }

    // ============================================================================
    // FIX #2: Detect input units (meters vs inches)
    // TF2/Source Engine typically uses inches, not meters.
    // Only convert if vertices appear to be in meters (small values).
    // ============================================================================
    static bool IsInputInMeters(const std::vector<PHYSolid>& solids)
    {
        float maxExtent = 0.0f;

        for (const auto& solid : solids)
        {
            for (size_t i = 0; i < solid.vertices.size(); i += 3)
            {
                float x = fabsf(solid.vertices[i]);
                float y = fabsf(solid.vertices[i + 1]);
                float z = fabsf(solid.vertices[i + 2]);
                maxExtent = std::max({maxExtent, x, y, z});
            }
        }

        // Heuristic:
        // - Human model in meters: ~2m max extent
        // - Human model in inches: ~72 inches max extent
        // - If max extent < 10, likely meters
        // - If max extent > 30, likely inches

        printf("  PHY max vertex extent: %.4f\n", maxExtent);

        if (maxExtent < 10.0f)
        {
            printf("  -> Detected METERS (will convert to inches x39.3701)\n");
            return true;
        }
        else
        {
            printf("  -> Detected INCHES (no unit conversion needed)\n");
            return false;
        }
    }
    GenerationResult PHYToBVH4Converter::Convert(
        const ParsedPHYData& phyData,
        const float* bonePoses,
        int32_t numBones,
        const PHYConversionConfig& config)
    {
        GenerationResult result;

        if (!phyData.valid)
        {
            result.errorMessage = "Invalid PHY data: " + phyData.errorMessage;
            return result;
        }

        if (phyData.solids.empty())
        {
            result.errorMessage = "No collision solids in PHY data";
            return result;
        }

        if (!bonePoses && numBones > 0)
        {
            result.errorMessage = "Invalid bone pose data";
            return result;
        }

        if (config.debugOutput)
        {
            printf("  Converting %zu PHY solids to BVH4...\n", phyData.solids.size());
        }

        // ============================================================================
        // FIX #2: Detect input units before processing
        // ============================================================================
        bool inputIsMeters = IsInputInMeters(phyData.solids);
        float unitScale = inputIsMeters ? 39.3701f : 1.0f;  // Only convert if meters

        // Build unified collision mesh from all solids
        MeshData meshData;
        MeshData untransformedMesh;  // For OBJ export (matches visual model)
        uint32_t totalVertexOffset = 0;

        ConversionStats stats;

        for (size_t solidIdx = 0; solidIdx < phyData.solids.size(); solidIdx++)
        {
            const PHYSolid& solid = phyData.solids[solidIdx];

            // Get bone transform (3x4 matrix)
            float boneTransform[12];  // 3x4 = 12 floats
            GetBoneTransform(bonePoses, numBones, solid.parentBone, boneTransform);

            if (config.debugOutput)
            {
                printf("    Solid %zu: bone %d, %zu verts, %zu tris\n",
                    solidIdx, solid.parentBone,
                    solid.vertices.size() / 3,
                    solid.triangles.size());
            }

            // Transform vertices from bone-local to world space
            size_t vertCount = solid.vertices.size() / 3;

            if (config.debugOutput && solidIdx == 0)
            {
                printf("    First 4 PHY vertices (bone-local):\n");
                for (size_t i = 0; i < std::min(size_t(4), vertCount); i++)
                {
                    const float* v = &solid.vertices[i * 3];
                    printf("      v%zu: (%.6f, %.6f, %.6f)\n", i, v[0], v[1], v[2]);
                }
            }

            for (size_t i = 0; i < vertCount; i++)
            {
                const float* localPos = &solid.vertices[i * 3];
                float worldPos[3];

                TransformVertex(localPos, boneTransform, worldPos);

                // FIX #2: Apply unit conversion only if needed (detected as meters)
                // Both collision and OBJ export need Y=180° rotation to match visual model
                // Y=180°: (x,y,z) → (-x, y, -z)
                MeshVertex untransformed;
                untransformed.x = -worldPos[0] * unitScale;
                untransformed.y = worldPos[1] * unitScale;
                untransformed.z = -worldPos[2] * unitScale;
                untransformed.nx = untransformed.ny = untransformed.nz = 0;
                untransformedMesh.vertices.push_back(untransformed);

                // Feed BVH4 builder FULL SIZE vertices (in inches) with Y=180° rotation
                // This ensures AABB uses full int16 range like working models
                // The scale will be divided by 65536 later in the BVH4 builder
                MeshVertex v;
                v.x = -worldPos[0] * unitScale;  // Y=180° rotation
                v.y = worldPos[1] * unitScale;
                v.z = -worldPos[2] * unitScale;
                v.nx = v.ny = v.nz = 0;

                meshData.vertices.push_back(v);
            }

            // Add triangles with adjusted indices
            for (const auto& tri : solid.triangles)
            {
                stats.totalTrianglesInput++;

                // Validate indices
                if (tri.v0 >= vertCount || tri.v1 >= vertCount || tri.v2 >= vertCount)
                {
                    stats.skippedInvalidIndices++;
                    if (config.debugOutput)
                    {
                        printf("    WARNING: Skipped triangle with invalid indices [%d, %d, %d] (vertCount=%zu) in solid %zu\n",
                            tri.v0, tri.v1, tri.v2, vertCount, solidIdx);
                    }
                    continue;
                }

                // Check for degenerate triangles in collision mesh
                const MeshVertex& v0 = meshData.vertices[totalVertexOffset + tri.v0];
                const MeshVertex& v1 = meshData.vertices[totalVertexOffset + tri.v1];
                const MeshVertex& v2 = meshData.vertices[totalVertexOffset + tri.v2];

                if (IsTriangleDegenerate(v0, v1, v2))
                {
                    stats.degenerateTriangles++;
                    if (config.debugOutput)
                    {
                        printf("    WARNING: Degenerate triangle detected in solid %zu (area near zero)\n", solidIdx);
                    }
                    // Still add it - let BVH4 builder handle it
                }
                else
                {
                    float area = CalculateTriangleArea(v0, v1, v2);
                    stats.avgTriangleArea += area;
                }

                // ============================================================================
                // FIX #3: Correct winding order after Y=180° rotation
                // Y=180° is equivalent to mirroring X and Z axes.
                // Mirroring transforms flip triangle winding (CCW → CW).
                // Must swap v1 and v2 to restore correct winding.
                // ============================================================================
                MeshTriangle untransformed_t;
                untransformed_t.v0 = totalVertexOffset + tri.v0;
                untransformed_t.v1 = totalVertexOffset + tri.v2;  // SWAPPED for winding fix
                untransformed_t.v2 = totalVertexOffset + tri.v1;  // SWAPPED for winding fix
                untransformed_t.surfaceProp = 0;
                untransformed_t.flags = tri.flags;
                untransformedMesh.triangles.push_back(untransformed_t);

                // Collision mesh also needs winding fix
                MeshTriangle t;
                t.v0 = totalVertexOffset + tri.v0;
                t.v1 = totalVertexOffset + tri.v2;  // SWAPPED for winding fix
                t.v2 = totalVertexOffset + tri.v1;  // SWAPPED for winding fix
                t.surfaceProp = 0;
                t.flags = tri.flags;

                meshData.triangles.push_back(t);
                stats.totalTrianglesOutput++;
            }

            totalVertexOffset += vertCount;
        }

        if (meshData.triangles.empty())
        {
            result.errorMessage = "No valid triangles after PHY conversion";
            return result;
        }

        meshData.surfacePropNames.push_back(config.bvhConfig.defaultSurfaceProp);

        // Calculate average triangle area
        if (stats.totalTrianglesOutput > stats.degenerateTriangles)
        {
            stats.avgTriangleArea /= (stats.totalTrianglesOutput - stats.degenerateTriangles);
        }

        // Print conversion statistics
        if (config.debugOutput)
        {
            stats.Print();
        }

        // Export untransformed mesh to OBJ (matches visual model coordinate system)
        if (!config.exportObjPath.empty())
        {
            untransformedMesh.surfacePropNames.push_back(config.bvhConfig.defaultSurfaceProp);
            ExportMeshToOBJ(untransformedMesh, config.exportObjPath);
        }

        if (config.debugOutput)
        {
            printf("  Total collision mesh: %zu verts, %zu tris\n",
                meshData.vertices.size(), meshData.triangles.size());

            // Print first few vertices to debug scale issue
            if (!meshData.vertices.empty())
            {
                printf("  First 4 world-space vertices (full precision):\n");
                for (size_t i = 0; i < std::min(size_t(4), meshData.vertices.size()); i++)
                {
                    printf("    v%zu: (%.8f, %.8f, %.8f)\n", i,
                        meshData.vertices[i].x,
                        meshData.vertices[i].y,
                        meshData.vertices[i].z);
                }
            }

            // Calculate mesh bounds for debugging
            if (!meshData.vertices.empty())
            {
                float minX = meshData.vertices[0].x, maxX = meshData.vertices[0].x;
                float minY = meshData.vertices[0].y, maxY = meshData.vertices[0].y;
                float minZ = meshData.vertices[0].z, maxZ = meshData.vertices[0].z;

                for (const auto& v : meshData.vertices)
                {
                    if (v.x < minX) minX = v.x;
                    if (v.x > maxX) maxX = v.x;
                    if (v.y < minY) minY = v.y;
                    if (v.y > maxY) maxY = v.y;
                    if (v.z < minZ) minZ = v.z;
                    if (v.z > maxZ) maxZ = v.z;
                }

                float sizeX = maxX - minX;
                float sizeY = maxY - minY;
                float sizeZ = maxZ - minZ;

                printf("  Collision mesh bounds (full precision):\n");
                printf("    X: %.8f to %.8f (size: %.8f)\n", minX, maxX, sizeX);
                printf("    Y: %.8f to %.8f (size: %.8f)\n", minY, maxY, sizeY);
                printf("    Z: %.8f to %.8f (size: %.8f)\n", minZ, maxZ, sizeZ);

                // ============================================================================
                // FIX #4: Validate triangle normal directions
                // Check if most triangles have normals pointing outward from mesh centroid
                // ============================================================================
                float centroidX = (minX + maxX) * 0.5f;
                float centroidY = (minY + maxY) * 0.5f;
                float centroidZ = (minZ + maxZ) * 0.5f;

                int outwardCount = 0;
                int inwardCount = 0;

                for (const auto& tri : meshData.triangles)
                {
                    const auto& v0 = meshData.vertices[tri.v0];
                    const auto& v1 = meshData.vertices[tri.v1];
                    const auto& v2 = meshData.vertices[tri.v2];

                    // Triangle center
                    float triCenterX = (v0.x + v1.x + v2.x) / 3.0f;
                    float triCenterY = (v0.y + v1.y + v2.y) / 3.0f;
                    float triCenterZ = (v0.z + v1.z + v2.z) / 3.0f;

                    // Edge vectors
                    float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
                    float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;

                    // Normal (cross product)
                    float nx = e1y * e2z - e1z * e2y;
                    float ny = e1z * e2x - e1x * e2z;
                    float nz = e1x * e2y - e1y * e2x;

                    // Direction from centroid to triangle
                    float toTriX = triCenterX - centroidX;
                    float toTriY = triCenterY - centroidY;
                    float toTriZ = triCenterZ - centroidZ;

                    // Dot product: positive = outward
                    float dot = nx * toTriX + ny * toTriY + nz * toTriZ;

                    if (dot > 0)
                        outwardCount++;
                    else
                        inwardCount++;
                }

                printf("  Normal direction validation:\n");
                printf("    Outward-facing triangles: %d\n", outwardCount);
                printf("    Inward-facing triangles: %d\n", inwardCount);

                if (inwardCount > outwardCount)
                    printf("    WARNING: More triangles facing INWARD than outward! Winding may still be wrong.\n");
                else
                    printf("    OK: Most triangles facing outward (%.1f%%)\n",
                        100.0f * outwardCount / (outwardCount + inwardCount));
            }
        }

        // Generate BVH4
        CollisionGenerator generator;
        generator.SetConfig(config.bvhConfig);

        result = generator.Generate(meshData);

        if (result.success && config.debugOutput)
        {
            printf("  BVH4 generated: %u nodes, %u triangles\n",
                result.nodeCount, result.triangleCount);
        }

        return result;
    }

    bool PHYToBVH4Converter::ExportMeshToOBJ(
        const MeshData& mesh,
        const std::string& objPath)
    {
        FILE* f = nullptr;
#ifdef _WIN32
        fopen_s(&f, objPath.c_str(), "w");
#else
        f = fopen(objPath.c_str(), "w");
#endif
        if (!f)
        {
            printf("  ERROR: Failed to open %s for writing\n", objPath.c_str());
            return false;
        }

        fprintf(f, "# Collision mesh exported from PHY conversion\n");
        fprintf(f, "# Vertices: %zu\n", mesh.vertices.size());
        fprintf(f, "# Triangles: %zu\n", mesh.triangles.size());
        fprintf(f, "\n");

        // Write vertices
        for (const auto& v : mesh.vertices)
        {
            fprintf(f, "v %.6f %.6f %.6f\n", v.x, v.y, v.z);
        }

        fprintf(f, "\n");

        // Write faces (OBJ uses 1-based indexing)
        for (const auto& tri : mesh.triangles)
        {
            fprintf(f, "f %u %u %u\n", tri.v0 + 1, tri.v1 + 1, tri.v2 + 1);
        }

        fclose(f);
        printf("  Exported collision mesh to: %s\n", objPath.c_str());
        return true;
    }

    void PHYToBVH4Converter::TransformVertex(
        const float localPos[3],
        const float* boneTransform,
        float outWorldPos[3])
    {
        // boneTransform is 3x4 matrix: [row0][row1][row2] where each row is [m0 m1 m2 m3]
        // Transform: out = mat * in + translation

        for (int i = 0; i < 3; i++)
        {
            const float* row = &boneTransform[i * 4];
            outWorldPos[i] = row[0] * localPos[0] +
                             row[1] * localPos[1] +
                             row[2] * localPos[2] +
                             row[3];  // translation
        }
    }

    void PHYToBVH4Converter::GetBoneTransform(
        const float* bonePoses,
        int32_t numBones,
        int32_t boneIdx,
        float* outTransform)
    {
        // PHY uses 1-based bone indices, 0 = root/world
        if (boneIdx <= 0 || boneIdx > numBones || !bonePoses)
        {
            // Identity matrix for invalid bones
            outTransform[0] = 1.0f; outTransform[1] = 0.0f; outTransform[2] = 0.0f; outTransform[3] = 0.0f;
            outTransform[4] = 0.0f; outTransform[5] = 1.0f; outTransform[6] = 0.0f; outTransform[7] = 0.0f;
            outTransform[8] = 0.0f; outTransform[9] = 0.0f; outTransform[10] = 1.0f; outTransform[11] = 0.0f;
            return;
        }

        // Convert to 0-based index and copy the bone transform
        int32_t zeroBasedIdx = boneIdx - 1;
        const float* boneMat = &bonePoses[zeroBasedIdx * 12];  // Each bone is 12 floats (3x4 matrix)

        memcpy(outTransform, boneMat, 12 * sizeof(float));
    }

} // namespace collision
