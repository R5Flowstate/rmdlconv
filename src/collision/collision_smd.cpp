// Copyright (c) 2026, CafeFPS
// SMD Collision Mesh Loader Implementation

#include "collision_smd.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <cstring>

namespace collision
{
    //=========================================================================
    // Global Configuration
    //=========================================================================

    std::string g_collisionModelPath;

    void SetCollisionModelPath(const std::string& path)
    {
        g_collisionModelPath = path;
    }

    void ClearCollisionModelPath()
    {
        g_collisionModelPath.clear();
    }
    //=========================================================================
    // SMD Parser Helper
    //=========================================================================

    struct SMDVertex
    {
        float x, y, z;
        float nx, ny, nz;

        bool operator==(const SMDVertex& other) const
        {
            // Compare with small epsilon for floating point
            const float eps = 1e-6f;
            return std::abs(x - other.x) < eps &&
                   std::abs(y - other.y) < eps &&
                   std::abs(z - other.z) < eps;
        }
    };

    struct SMDVertexHash
    {
        size_t operator()(const SMDVertex& v) const
        {
            // Simple hash combining x, y, z coordinates
            size_t h1 = std::hash<float>{}(v.x);
            size_t h2 = std::hash<float>{}(v.y);
            size_t h3 = std::hash<float>{}(v.z);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    static std::string Trim(const std::string& str)
    {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    //=========================================================================
    // SMD Loader Implementation
    //=========================================================================

    SMDLoadResult LoadCollisionSMD(const std::string& smdPath)
    {
        SMDLoadResult result;

        std::ifstream file(smdPath);
        if (!file.is_open())
        {
            result.errorMessage = "Failed to open SMD file: " + smdPath;
            return result;
        }

        printf("  loading collision SMD: %s\n", smdPath.c_str());

        // Track unique vertices and their indices
        std::vector<SMDVertex> uniqueVertices;
        std::unordered_map<SMDVertex, uint32_t, SMDVertexHash> vertexMap;

        // Track triangles and materials
        std::vector<MeshTriangle> triangles;
        std::unordered_map<std::string, uint16_t> materialMap;
        std::vector<std::string> materialNames;

        std::string line;
        bool inTriangles = false;
        bool needMaterial = false;
        int triangleVertexCount = 0;
        uint32_t currentTriVertices[3] = {0, 0, 0};
        std::string currentMaterial;
        uint16_t currentMaterialIdx = 0;

        int lineNum = 0;
        while (std::getline(file, line))
        {
            lineNum++;
            line = Trim(line);
            if (line.empty() || line[0] == '/')
                continue;

            // Check for section markers
            if (line == "triangles")
            {
                printf("  [SMD] Found 'triangles' section at line %d\n", lineNum);
                inTriangles = true;
                needMaterial = true;
                triangleVertexCount = 0;
                continue;
            }
            else if (line == "end")
            {
                if (inTriangles)
                    break;  // Done reading triangles
                continue;
            }
            else if (line == "nodes" || line == "skeleton" || line == "version 1")
            {
                inTriangles = false;
                continue;
            }

            if (!inTriangles)
                continue;

            // In triangles section
            // Read material name before each triangle
            if (needMaterial)
            {
                // This is a material name
                currentMaterial = line;

                // Track material
                auto it = materialMap.find(currentMaterial);
                if (it == materialMap.end())
                {
                    currentMaterialIdx = static_cast<uint16_t>(materialNames.size());
                    materialMap[currentMaterial] = currentMaterialIdx;
                    materialNames.push_back(currentMaterial);
                    printf("  [SMD] New material at line %d: '%s' (idx=%d)\n",
                           lineNum, currentMaterial.c_str(), currentMaterialIdx);
                }
                else
                {
                    currentMaterialIdx = it->second;
                }

                needMaterial = false;
                triangleVertexCount = 0;
                continue;
            }

            // Parse vertex line: bone_id x y z nx ny nz u v [weights...]
            std::istringstream iss(line);
            int boneId;
            SMDVertex v;
            float u, v_uv;

            if (!(iss >> boneId >> v.x >> v.y >> v.z >> v.nx >> v.ny >> v.nz >> u >> v_uv))
            {
                // Try without UV (some simplified collision SMDs)
                iss.clear();
                iss.str(line);
                if (!(iss >> boneId >> v.x >> v.y >> v.z >> v.nx >> v.ny >> v.nz))
                {
                    result.errorMessage = "Failed to parse vertex line: " + line;
                    return result;
                }
            }

            // Find or add unique vertex
            uint32_t vertexIdx;
            auto it = vertexMap.find(v);
            if (it == vertexMap.end())
            {
                vertexIdx = static_cast<uint32_t>(uniqueVertices.size());
                vertexMap[v] = vertexIdx;
                uniqueVertices.push_back(v);
            }
            else
            {
                vertexIdx = it->second;
            }

            currentTriVertices[triangleVertexCount] = vertexIdx;
            triangleVertexCount++;

            // Complete triangle
            if (triangleVertexCount == 3)
            {
                MeshTriangle tri;
                tri.v0 = currentTriVertices[0];
                tri.v1 = currentTriVertices[1];
                tri.v2 = currentTriVertices[2];
                tri.surfaceProp = currentMaterialIdx;
                tri.flags = 0;
                triangles.push_back(tri);

                triangleVertexCount = 0;
                needMaterial = true;  // Next line will be material name
            }
        }

        file.close();

        printf("  [SMD] Parsing complete: %zu vertices, %zu triangles\n",
               uniqueVertices.size(), triangles.size());

        if (uniqueVertices.empty() || triangles.empty())
        {
            result.errorMessage = "No valid mesh data found in SMD file (vertices: " +
                                  std::to_string(uniqueVertices.size()) +
                                  ", triangles: " + std::to_string(triangles.size()) + ")";
            return result;
        }

        // Convert to MeshData
        result.mesh.vertices.reserve(uniqueVertices.size());
        for (const auto& v : uniqueVertices)
        {
            MeshVertex mv;
            mv.x = v.x;
            mv.y = v.y;
            mv.z = v.z;
            mv.nx = v.nx;
            mv.ny = v.ny;
            mv.nz = v.nz;
            result.mesh.vertices.push_back(mv);
        }

        result.mesh.triangles = std::move(triangles);

        // Use "default" as surface prop for collision, or the first material
        if (materialNames.empty())
        {
            result.mesh.surfacePropNames.push_back("default");
        }
        else
        {
            result.mesh.surfacePropNames = std::move(materialNames);
        }

        result.success = true;

        printf("  loaded %zu vertices, %zu triangles from collision SMD\n",
            result.mesh.vertices.size(), result.mesh.triangles.size());

        return result;
    }

    std::string FindCollisionSMD(const std::string& modelPath)
    {
        // Check for global override first (set via -collisionmodel parameter)
        if (!g_collisionModelPath.empty())
        {
            if (std::filesystem::exists(g_collisionModelPath))
            {
                return g_collisionModelPath;
            }
            else
            {
                printf("  warning: specified collision model '%s' not found\n",
                    g_collisionModelPath.c_str());
            }
        }

        // Auto-detect collision SMD based on model path
        // Get the base path without extension
        std::filesystem::path path(modelPath);
        std::filesystem::path dir = path.parent_path();
        std::string stem = path.stem().string();

        // Remove existing extension if present
        if (stem.find('.') != std::string::npos)
        {
            stem = stem.substr(0, stem.find('.'));
        }

        // Check for various collision SMD naming conventions
        std::vector<std::string> suffixes = {
            "_phys.smd",
            "_collision.smd",
            "_phy.smd",
            "_col.smd",
            "_physics.smd"
        };

        for (const auto& suffix : suffixes)
        {
            std::filesystem::path collPath = dir / (stem + suffix);
            if (std::filesystem::exists(collPath))
            {
                return collPath.string();
            }
        }

        return "";
    }

} // namespace collision
