//=============================================================================//
// PHY to BVH4 Converter
// Converts Titanfall 2 PHY collision to Apex BVH4 format
//=============================================================================//
#pragma once

#include "phy_parser.h"
#include "collision_generator.h"
#include <string>

// Forward declarations
struct matrix3x4_t;

namespace collision
{
    //=========================================================================
    // Conversion Configuration
    //=========================================================================

    struct PHYConversionConfig
    {
        GeneratorConfig bvhConfig;      // BVH4 generation settings
        bool debugOutput;               // Print debug info during conversion
        bool validateTransforms;        // Validate bone transforms
        std::string exportObjPath;      // Optional: Export collision mesh to OBJ (empty = no export)

        PHYConversionConfig()
            : debugOutput(false)
            , validateTransforms(true)
            , exportObjPath("")
        {
            bvhConfig.maxTrianglesPerLeaf = 4;
            bvhConfig.defaultSurfaceProp = "default";
            bvhConfig.contentsMask = 0x1; // CONTENTS_SOLID
        }
    };

    //=========================================================================
    // PHY to BVH4 Converter
    //=========================================================================

    class PHYToBVH4Converter
    {
    public:
        // Convert parsed PHY data to BVH4 collision
        // bonePoses should be array of 3x4 float matrices (bone transforms in reference pose)
        // numBones is the count of bones
        static GenerationResult Convert(
            const ParsedPHYData& phyData,
            const float* bonePoses,  // array of [numBones][3][4] floats
            int32_t numBones,
            const PHYConversionConfig& config = PHYConversionConfig());

        // Export collision mesh to OBJ file for debugging
        static bool ExportMeshToOBJ(
            const MeshData& mesh,
            const std::string& objPath);

    private:
        // Transform vertex from bone-local to world space
        static void TransformVertex(
            const float localPos[3],
            const float* boneTransform,  // 3x4 matrix
            float outWorldPos[3]);

        // Get bone transform matrix (identity for invalid bones)
        static void GetBoneTransform(
            const float* bonePoses,
            int32_t numBones,
            int32_t boneIdx,
            float* outTransform);  // 3x4 matrix output
    };

} // namespace collision
