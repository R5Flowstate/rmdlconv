//=============================================================================//
// Valve/IVP (id=0) .phy -> Apex geoms (id=1) .phy
//=============================================================================//
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace collision
{
    // One convex piece: bone-local inches, model axes.
    struct ApexHull
    {
        std::vector<std::array<float, 3>> verts;
        std::vector<std::vector<uint8_t>> sides;   // wound CCW seen from outside
        std::vector<std::array<uint8_t, 4>> edges; // v0, v1, side walking v0->v1, the other side
        double volume = 0.0;
        double centroid[3] = {};
        double inertiaAtOrigin[3][3] = {};         // per unit mass, about the bone origin
        float inscribedRadius = 0.f;
        float bbMin[3] = {};
        float bbMax[3] = {};
    };

    // One phy solid (one ragdoll bone); IVP ledges become separate hulls.
    struct ApexSolid
    {
        std::vector<ApexHull> hulls;
        std::string boneName;
        std::string surfaceProp;
    };

    struct ApexPhy
    {
        std::vector<ApexSolid> solids;
        std::string keyValues;
        int32_t checksum = 0;
        bool valid = false;
        std::string error;
    };

    ApexPhy BuildApexPhyFromValve(const void* phyData, size_t phySize);

    // 20-byte header id=1 layout: what the S3 dedi ships and what the S21 client compacts.
    std::vector<uint8_t> WriteApexPhy(const ApexPhy& phy);

    // Fan-triangulated hull faces, for collision builders.
    void TriangulateHull(const ApexHull& hull, std::vector<std::array<uint32_t, 3>>& outTris);
}
