// Copyright (c) 2026, CafeFPS
// SMD Collision Mesh Loader
// Loads collision mesh from Source SMD format files

#pragma once

#include "collision_generator.h"
#include <string>

namespace collision
{
    //=========================================================================
    // Global Configuration
    //=========================================================================

    // Optional override for collision model path (set via command line)
    // If set, this path is used instead of auto-detection
    extern std::string g_collisionModelPath;

    // Set the collision model path override
    void SetCollisionModelPath(const std::string& path);

    // Clear the collision model path override (use auto-detection)
    void ClearCollisionModelPath();

    //=========================================================================
    // SMD Collision Loader
    //=========================================================================

    struct SMDLoadResult
    {
        MeshData mesh;
        bool success;
        std::string errorMessage;

        SMDLoadResult() : success(false) {}
    };

    // Load collision mesh from SMD file
    // The SMD file should contain only the collision geometry
    // (typically a simplified version of the visual mesh)
    SMDLoadResult LoadCollisionSMD(const std::string& smdPath);

    // Check if a collision SMD file exists for a given model path
    // Returns the collision SMD path if found, empty string if not
    // If g_collisionModelPath is set, returns that path instead
    // Otherwise checks for:
    //   1. modelname_phys.smd
    //   2. modelname_collision.smd
    //   3. modelname_phy.smd
    std::string FindCollisionSMD(const std::string& modelPath);

} // namespace collision
