//=============================================================================//
// PHY Parser Implementation - Respawn/Valve Physics Format
// Supports both Valve/TF2 (id=0) and Respawn/Apex (id=1) formats
//=============================================================================//

#include "phy_parser.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <array>
#include <utility>

namespace collision
{
    ParsedPHYData PHYParser::Parse(const void* phyData, size_t phySize)
    {
        ParsedPHYData result;

        if (!phyData || phySize < 4)
        {
            result.errorMessage = "Invalid PHY data pointer or size too small";
            return result;
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(phyData);

        // Check header format by looking at first 4 bytes
        // V10/V16 header starts with size=20 or size=16
        // V16 compact header starts with solidCount (usually small) and propertiesOffset
        int32_t firstInt = *reinterpret_cast<const int32_t*>(ptr);

        if (firstInt == 20)
        {
            // 20-byte IVPS header - check id field to determine format
            const phyheader_t* header = reinterpret_cast<const phyheader_t*>(ptr);

            if (header->id == 0)
            {
                // Valve/TF2 format
                printf("  Detected Valve/TF2 PHY format (id=0)\n");
                return ParseValve(phyData, phySize);
            }
            else
            {
                // Respawn/Apex format (id=1)
                printf("  Detected Respawn/Apex PHY format (id=1)\n");
                return ParseV10(phyData, phySize);
            }
        }
        else if (firstInt == 16)
        {
            // 16-byte header (older Valve format)
            printf("  Detected 16-byte PHY header (Valve format)\n");
            return ParseValve(phyData, phySize);
        }
        else
        {
            // V16 compact format (4-byte header) or unknown
            // Check if first two uint16s look like solidCount and propertiesOffset
            uint16_t field0 = *reinterpret_cast<const uint16_t*>(ptr);
            uint16_t field1 = *reinterpret_cast<const uint16_t*>(ptr + 2);

            if (field0 > 0 && field0 < 100 && field1 > 0 && field1 < 65535)
            {
                printf("  Detected v16 compact PHY format (4-byte header)\n");
                return ParseV16(phyData, phySize, 0);
            }
            else
            {
                result.errorMessage = "Unknown PHY format (first int=" + std::to_string(firstInt) + ")";
                return result;
            }
        }
    }

    ParsedPHYData PHYParser::ParseV10(const void* phyData, size_t phySize)
    {
        ParsedPHYData result;

        if (phySize < sizeof(phyheader_t))
        {
            result.errorMessage = "PHY data too small for v10 header";
            return result;
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(phyData);
        const phyheader_t* header = reinterpret_cast<const phyheader_t*>(ptr);

        printf("  PHY v10 header: size=%d, id=%d, solidCount=%d, checksum=0x%08X, propsOff=%d\n",
               header->size, header->id, header->solidCount, header->checkSum, header->propertiesOffset);

        result.checksum = header->checkSum;

        // Data after header is phyptrheader_t
        const uint8_t* ptrHdrStart = ptr + sizeof(phyheader_t);
        size_t dataSize = phySize - sizeof(phyheader_t);

        if (!ParseRespawnSolidData(ptrHdrStart, dataSize, result))
        {
            return result;
        }

        result.valid = true;
        return result;
    }

    ParsedPHYData PHYParser::ParseV16(const void* phyData, size_t phySize, int32_t checkSum)
    {
        ParsedPHYData result;

        if (phySize < sizeof(phyheader_v16_t))
        {
            result.errorMessage = "PHY data too small for v16 header";
            return result;
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(phyData);
        const phyheader_v16_t* header = reinterpret_cast<const phyheader_v16_t*>(ptr);

        printf("  PHY v16 header: solidCount=%d, propsOff=%d\n",
               header->solidCount, header->propertiesOffset);

        result.checksum = checkSum;

        // Data after header is phyptrheader_t
        const uint8_t* ptrHdrStart = ptr + sizeof(phyheader_v16_t);
        size_t dataSize = phySize - sizeof(phyheader_v16_t);

        if (!ParseRespawnSolidData(ptrHdrStart, dataSize, result))
        {
            return result;
        }

        result.valid = true;
        return result;
    }

    ParsedPHYData PHYParser::ParseValve(const void* phyData, size_t phySize)
    {
        ParsedPHYData result;

        if (phySize < 16)
        {
            result.errorMessage = "PHY data too small for Valve header";
            return result;
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(phyData);
        const phyheader_t* header = reinterpret_cast<const phyheader_t*>(ptr);

        int headerSize = header->size;
        if (headerSize != 16 && headerSize != 20)
        {
            result.errorMessage = "Invalid Valve PHY header size: " + std::to_string(headerSize);
            return result;
        }

        printf("  PHY Valve header: size=%d, id=%d, solidCount=%d, checksum=0x%08X\n",
               header->size, header->id, header->solidCount, header->checkSum);

        result.checksum = header->checkSum;

        // Solid data starts after header
        const uint8_t* solidStart = ptr + headerSize;
        size_t dataSize = phySize - headerSize;

        if (!ParseValveSolidData(solidStart, dataSize, header->solidCount, result))
        {
            return result;
        }

        result.valid = true;
        return result;
    }

    bool PHYParser::ParseRespawnSolidData(const uint8_t* ptrHdrStart, size_t dataSize,
                                          ParsedPHYData& result)
    {
        if (dataSize < sizeof(phyptrheader_t))
        {
            result.errorMessage = "Data too small for phyptrheader_t";
            return false;
        }

        const phyptrheader_t* ptrHdr = reinterpret_cast<const phyptrheader_t*>(ptrHdrStart);

        printf("  PHY ptr header: solidOffset=%lld, solidCount=%lld, solidSize=%lld\n",
               (long long)ptrHdr->solidOffset, (long long)ptrHdr->solidCount, (long long)ptrHdr->solidSize);

        // Validate offsets
        if (ptrHdr->solidOffset < 0 || ptrHdr->solidOffset >= (int64_t)dataSize)
        {
            result.errorMessage = "Invalid solidOffset in phyptrheader_t";
            return false;
        }

        if (ptrHdr->solidCount < 0 || ptrHdr->solidCount > 100)
        {
            result.errorMessage = "Invalid solidCount in phyptrheader_t";
            return false;
        }

        // Process each solid group
        for (int64_t groupIdx = 0; groupIdx < ptrHdr->solidCount; groupIdx++)
        {
            size_t groupOffset = static_cast<size_t>(ptrHdr->solidOffset + sizeof(solidgroup_t) * groupIdx);

            if (groupOffset + sizeof(solidgroup_t) > dataSize)
            {
                result.errorMessage = "solidgroup_t out of bounds";
                return false;
            }

            const solidgroup_t* solidGroup = reinterpret_cast<const solidgroup_t*>(ptrHdrStart + groupOffset);

            printf("  Solid group %lld: solidOffset=%lld, solidCount=%lld, bbMin=(%.2f,%.2f,%.2f), bbMax=(%.2f,%.2f,%.2f)\n",
                   (long long)groupIdx, (long long)solidGroup->solidOffset, (long long)solidGroup->solidCount,
                   solidGroup->bbMin[0], solidGroup->bbMin[1], solidGroup->bbMin[2],
                   solidGroup->bbMax[0], solidGroup->bbMax[1], solidGroup->bbMax[2]);

            // Process each solid in this group
            for (int64_t solidIdx = 0; solidIdx < solidGroup->solidCount; solidIdx++)
            {
                size_t solidOffset = static_cast<size_t>(solidGroup->solidOffset + sizeof(solid_t) * solidIdx);

                if (solidOffset + sizeof(solid_t) > dataSize)
                {
                    result.errorMessage = "solid_t out of bounds";
                    return false;
                }

                const solid_t* solid = reinterpret_cast<const solid_t*>(ptrHdrStart + solidOffset);

                printf("    Solid %lld: verts=%lld, sides=%lld, edges=%lld\n",
                       (long long)solidIdx, (long long)solid->vertCount,
                       (long long)solid->sideCount, (long long)solid->edgeCount);

                PHYSolid parsedSolid;
                parsedSolid.parentBone = 0; // Default to root bone

                // Copy bounding box from group
                memcpy(parsedSolid.bbMin, solidGroup->bbMin, sizeof(float) * 3);
                memcpy(parsedSolid.bbMax, solidGroup->bbMax, sizeof(float) * 3);

                // Calculate mass center from bounding box
                parsedSolid.massCenter[0] = (solidGroup->bbMin[0] + solidGroup->bbMax[0]) * 0.5f;
                parsedSolid.massCenter[1] = (solidGroup->bbMin[1] + solidGroup->bbMax[1]) * 0.5f;
                parsedSolid.massCenter[2] = (solidGroup->bbMin[2] + solidGroup->bbMax[2]) * 0.5f;

                // Calculate bounding radius
                float dx = solidGroup->bbMax[0] - solidGroup->bbMin[0];
                float dy = solidGroup->bbMax[1] - solidGroup->bbMin[1];
                float dz = solidGroup->bbMax[2] - solidGroup->bbMin[2];
                parsedSolid.boundingRadius = sqrtf(dx*dx + dy*dy + dz*dz) * 0.5f;

                // Default rotation inertia
                parsedSolid.rotationInertia[0] = 1.0f;
                parsedSolid.rotationInertia[1] = 1.0f;
                parsedSolid.rotationInertia[2] = 1.0f;

                // Read vertices
                if (solid->vertCount > 0 && solid->vertOffset >= 0)
                {
                    size_t vertOffset = static_cast<size_t>(solid->vertOffset);
                    size_t vertEnd = vertOffset + static_cast<size_t>(solid->vertCount) * sizeof(phyvertex_t);

                    if (vertEnd > dataSize)
                    {
                        result.errorMessage = "Vertex data out of bounds";
                        return false;
                    }

                    const phyvertex_t* verts = reinterpret_cast<const phyvertex_t*>(ptrHdrStart + vertOffset);

                    parsedSolid.vertices.reserve(static_cast<size_t>(solid->vertCount) * 3);
                    for (int64_t v = 0; v < solid->vertCount; v++)
                    {
                        parsedSolid.vertices.push_back(verts[v].pos[0]);
                        parsedSolid.vertices.push_back(verts[v].pos[1]);
                        parsedSolid.vertices.push_back(verts[v].pos[2]);
                    }

                    printf("      First vertex: (%.2f, %.2f, %.2f)\n",
                           verts[0].pos[0], verts[0].pos[1], verts[0].pos[2]);
                }

                // Read sides and triangulate
                if (solid->sideCount > 0 && solid->sideOffset >= 0)
                {
                    size_t sideOffset = static_cast<size_t>(solid->sideOffset);
                    size_t sideEnd = sideOffset + static_cast<size_t>(solid->sideCount) * sizeof(side_t);

                    if (sideEnd > dataSize)
                    {
                        result.errorMessage = "Side data out of bounds";
                        return false;
                    }

                    const side_t* sides = reinterpret_cast<const side_t*>(ptrHdrStart + sideOffset);

                    for (int64_t s = 0; s < solid->sideCount; s++)
                    {
                        // Count valid vertices in this face (stop at 0xFF or end)
                        int faceVertCount = 0;
                        for (int i = 0; i < 32; i++)
                        {
                            if (sides[s].vertIndices[i] == 0xFF)
                                break;
                            // Also check if index is valid
                            if (sides[s].vertIndices[i] >= solid->vertCount)
                                break;
                            faceVertCount++;
                        }

                        if (faceVertCount >= 3)
                        {
                            // Triangulate this convex face
                            TriangulateFace(sides[s].vertIndices, faceVertCount, parsedSolid.triangles);
                        }
                    }

                    printf("      Generated %zu triangles from %lld sides\n",
                           parsedSolid.triangles.size(), (long long)solid->sideCount);
                }

                result.solids.push_back(parsedSolid);
            }
        }

        printf("  PHY parsed: %zu solids total\n", result.solids.size());

        return true;
    }

    bool PHYParser::ParseValveSolidData(const uint8_t* solidStart, size_t dataSize,
                                        int solidCount, ParsedPHYData& result)
    {
        const uint8_t* ptr = solidStart;
        const uint8_t* end = solidStart + dataSize;

        printf("  Parsing %d Valve solids...\n", solidCount);

        for (int solidIdx = 0; solidIdx < solidCount; solidIdx++)
        {
            if (ptr + sizeof(swapcompactsurfaceheader_t) + sizeof(legacysurfaceheader_t) > end)
            {
                result.errorMessage = "Unexpected end of data at solid " + std::to_string(solidIdx);
                return false;
            }

            const uint8_t* solidBlockStart = ptr;

            // Read surface headers
            const swapcompactsurfaceheader_t* surfHdr =
                reinterpret_cast<const swapcompactsurfaceheader_t*>(ptr);

            const legacysurfaceheader_t* legacyHdr =
                reinterpret_cast<const legacysurfaceheader_t*>(ptr + sizeof(swapcompactsurfaceheader_t));

            printf("    Solid %d: size=%d, modelType=%d\n",
                   solidIdx, surfHdr->size, surfHdr->modelType);

            PHYSolid parsedSolid;
            memcpy(parsedSolid.massCenter, legacyHdr->mass_center, sizeof(float) * 3);
            memcpy(parsedSolid.rotationInertia, legacyHdr->rotation_inertia, sizeof(float) * 3);
            parsedSolid.boundingRadius = legacyHdr->upper_limit_radius;

            // Initialize bounding box from mass center (will be updated from vertices)
            memcpy(parsedSolid.bbMin, legacyHdr->mass_center, sizeof(float) * 3);
            memcpy(parsedSolid.bbMax, legacyHdr->mass_center, sizeof(float) * 3);

            // Find start of triangle headers
            const uint8_t* triHdrStart = ptr + sizeof(swapcompactsurfaceheader_t) +
                                         sizeof(legacysurfaceheader_t);

            const trianglefaceheader_t* firstTriHdr =
                reinterpret_cast<const trianglefaceheader_t*>(triHdrStart);

            parsedSolid.parentBone = firstTriHdr->parent;

            printf("      Triangle header: vertexindex=%d, parent=%d, flags=0x%X, numfaces=%d\n",
                   firstTriHdr->vertexindex, firstTriHdr->parent, firstTriHdr->flags, firstTriHdr->numfaces);

            // Collect all triangle headers for this solid
            std::vector<const trianglefaceheader_t*> triHeaders;
            const uint8_t* triPtr = triHdrStart;

            // Read all triangle headers until we reach vertex data
            while (triPtr < triHdrStart + firstTriHdr->vertexindex)
            {
                if (triPtr + sizeof(trianglefaceheader_t) > solidBlockStart + surfHdr->size)
                {
                    break;  // Reached end of solid
                }

                const trianglefaceheader_t* hdr =
                    reinterpret_cast<const trianglefaceheader_t*>(triPtr);

                if (hdr->numfaces == 0)
                    break;

                triHeaders.push_back(hdr);

                triPtr += sizeof(trianglefaceheader_t);
                triPtr += sizeof(trianglevertmap_t) * hdr->numfaces;
            }

            // Read all triangles from all headers
            for (size_t hdrIdx = 0; hdrIdx < triHeaders.size(); hdrIdx++)
            {
                const auto* hdr = triHeaders[hdrIdx];
                const trianglevertmap_t* vertMaps =
                    reinterpret_cast<const trianglevertmap_t*>(
                        reinterpret_cast<const uint8_t*>(hdr) + sizeof(trianglefaceheader_t));

                printf("      Triangle group %zu: flags=0x%X, %d triangles\n",
                       hdrIdx, hdr->flags, hdr->numfaces);

                for (int32_t j = 0; j < hdr->numfaces; j++)
                {
                    PHYTriangle tri;
                    tri.v0 = vertMaps[j].vertid;
                    tri.v1 = vertMaps[j].vertid1;
                    tri.v2 = vertMaps[j].vertid2;
                    tri.flags = hdr->flags;
                    parsedSolid.triangles.push_back(tri);
                }
            }

            // Determine vertex count by finding max index
            int32_t maxVertIdx = FindMaxVertexIndex(parsedSolid.triangles);

            if (maxVertIdx < 0)
            {
                // No triangles - might be a collision hull without triangles
                printf("      WARNING: No triangles found in solid %d\n", solidIdx);
                result.solids.push_back(parsedSolid);
                ptr = solidBlockStart + surfHdr->size;
                continue;
            }

            int32_t vertexCount = maxVertIdx + 1;

            // Read vertices
            const valve_phyvertex_t* vertices =
                reinterpret_cast<const valve_phyvertex_t*>(triHdrStart + firstTriHdr->vertexindex);

            const uint8_t* vertEnd = reinterpret_cast<const uint8_t*>(vertices + vertexCount);

            if (vertEnd > solidBlockStart + surfHdr->size)
            {
                result.errorMessage = "Vertex data out of bounds in solid " + std::to_string(solidIdx);
                return false;
            }

            // Store vertices as interleaved XYZ
            parsedSolid.vertices.reserve(vertexCount * 3);
            for (int32_t j = 0; j < vertexCount; j++)
            {
                parsedSolid.vertices.push_back(vertices[j].pos[0]);
                parsedSolid.vertices.push_back(vertices[j].pos[1]);
                parsedSolid.vertices.push_back(vertices[j].pos[2]);

                // Update bounding box
                parsedSolid.bbMin[0] = std::min(parsedSolid.bbMin[0], vertices[j].pos[0]);
                parsedSolid.bbMin[1] = std::min(parsedSolid.bbMin[1], vertices[j].pos[1]);
                parsedSolid.bbMin[2] = std::min(parsedSolid.bbMin[2], vertices[j].pos[2]);
                parsedSolid.bbMax[0] = std::max(parsedSolid.bbMax[0], vertices[j].pos[0]);
                parsedSolid.bbMax[1] = std::max(parsedSolid.bbMax[1], vertices[j].pos[1]);
                parsedSolid.bbMax[2] = std::max(parsedSolid.bbMax[2], vertices[j].pos[2]);
            }

            printf("      Solid %d: bone=%d, verts=%d, tris=%zu\n",
                   solidIdx, parsedSolid.parentBone, vertexCount, parsedSolid.triangles.size());

            result.solids.push_back(parsedSolid);

            // Advance to next solid
            ptr = solidBlockStart + surfHdr->size;
        }

        printf("  PHY parsed: %zu solids total\n", result.solids.size());

        return true;
    }

    void PHYParser::TriangulateFace(const uint8_t* vertIndices, int vertCount,
                                    std::vector<PHYTriangle>& outTriangles)
    {
        // Fan triangulation from first vertex
        // For a convex polygon with vertices [0,1,2,3,4], this creates:
        //   Triangle 0: 0,1,2
        //   Triangle 1: 0,2,3
        //   Triangle 2: 0,3,4
        for (int i = 1; i < vertCount - 1; i++)
        {
            PHYTriangle tri;
            tri.v0 = vertIndices[0];
            tri.v1 = vertIndices[i];
            tri.v2 = vertIndices[i + 1];
            tri.flags = 0;
            outTriangles.push_back(tri);
        }
    }

    //=========================================================================
    // IVP (Valve id=0) writer -- generalises the runtime-proven box layout to an
    // arbitrary convex solid. Layout per solid (offsets relative to surface start CS):
    //   [0..48)            IVP_Compact_Surface header
    //   [48..64)           IVP_Compact_Ledge
    //   [64 .. 64+16*M)    M IVP_Compact_Triangle (16 B each)
    //   [ptsOff..+16*N)    N IVP_U_Float_Point  (float4, METRES)
    //   [nodeOff..+32)     IVP_Compact_Ledgetree_Node (leaf root)
    // Each solid block in the file = swapcompactsurfaceheader_t(32, first int = solid size)
    // + the CS surface. After all solids comes the keyvalue text. Points are in IVP metres
    // (HL inches * 0.0254). Triangle winding is FORCED outward (normal . (face-centroid -
    // solid-centroid) >= 0) so collision can never come out inside-out regardless of source.
    //=========================================================================
    namespace
    {
        const float kPhyScale = 0.0254f;   // HL inches -> IVP metres (single tuning point)

        inline void put32(std::vector<uint8_t>& b, size_t off, int32_t v) { memcpy(&b[off], &v, 4); }
        inline void put16(std::vector<uint8_t>& b, size_t off, int16_t v) { memcpy(&b[off], &v, 2); }
        inline void putf (std::vector<uint8_t>& b, size_t off, float   v) { memcpy(&b[off], &v, 4); }
    }

    std::vector<uint8_t> PHYParser::BuildIVPPhy(const ParsedPHYData& data, int32_t checksum,
                                                const char* keyValues, size_t keyValuesLen)
    {
        const float S = kPhyScale;
        std::vector<uint8_t> out;

        // phyheader_t (20 B). solidCount + keyValuesOffset are patched after the body is built.
        out.resize(20, 0);
        put32(out, 0, 20);          // size
        put32(out, 4, 0);           // id = 0  -> legacy IVP loader (BRANCH A)
        put32(out, 12, checksum);   // checkSum (model checksum)
        const size_t bodyStart = out.size();

        int emitted = 0;
        for (const auto& solid : data.solids)
        {
            const int N = static_cast<int>(solid.vertices.size() / 3);
            int M = static_cast<int>(solid.triangles.size());
            if (N < 4 || M < 4)     // need a genuine 3D convex (tetra minimum)
                continue;

            // Solid centroid (inches) for the outward-winding test.
            float sc[3] = { 0.f, 0.f, 0.f };
            for (int v = 0; v < N; ++v) { sc[0]+=solid.vertices[v*3+0]; sc[1]+=solid.vertices[v*3+1]; sc[2]+=solid.vertices[v*3+2]; }
            sc[0]/=N; sc[1]/=N; sc[2]/=N;

            // Outward-wound, index-validated triangle list (drop any with an OOB index).
            std::vector<std::array<int,3>> tris;
            tris.reserve(M);
            for (int t = 0; t < M; ++t)
            {
                int a = solid.triangles[t].v0, b = solid.triangles[t].v1, c = solid.triangles[t].v2;
                if (a < 0 || b < 0 || c < 0 || a >= N || b >= N || c >= N || a == b || b == c || a == c)
                    continue;
                const float* pa = &solid.vertices[a*3]; const float* pb = &solid.vertices[b*3]; const float* pc = &solid.vertices[c*3];
                const float e1[3] = { pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2] };
                const float e2[3] = { pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2] };
                const float nrm[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                const float fc[3] = { (pa[0]+pb[0]+pc[0])/3.f - sc[0], (pa[1]+pb[1]+pc[1])/3.f - sc[1], (pa[2]+pb[2]+pc[2])/3.f - sc[2] };
                if (nrm[0]*fc[0] + nrm[1]*fc[1] + nrm[2]*fc[2] < 0.f)
                    std::swap(b, c);    // flip to outward
                tris.push_back({ a, b, c });
            }
            M = static_cast<int>(tris.size());
            if (M < 4)
                continue;

            // Bounding radius (metres) from the solid centroid.
            float r2 = 0.f;
            for (int v = 0; v < N; ++v)
            {
                const float dx=solid.vertices[v*3+0]-sc[0], dy=solid.vertices[v*3+1]-sc[1], dz=solid.vertices[v*3+2]-sc[2];
                r2 = std::max(r2, dx*dx+dy*dy+dz*dz);
            }
            const float radius = sqrtf(r2) * S;

            const int ledgeOff = 48;
            const int triOff    = 64;
            const int ptsOff    = triOff + 16 * M;
            const int nodeOff   = ptsOff + 16 * N;
            const int SURF      = nodeOff + 32;

            const size_t blockStart = out.size();
            out.resize(out.size() + 4 + 28 + SURF, 0);
            const size_t hdrOff = blockStart + 4;   // compactsurfaceheader (28 B after the size int)
            const size_t CS     = hdrOff + 28;      // IVP_Compact_Surface

            put32(out, blockStart, 28 + SURF);      // per-solid size prefix (bytes after this int)
            put32(out, hdrOff + 0, 0x59485056);     // 'VPHY'
            put32(out, hdrOff + 4, 256);            // version=256 | modelType=0
            put32(out, hdrOff + 8, SURF);           // surfaceSize
            // drag[3] (hdrOff+12..24) = 0, axisMapSize (hdrOff+24) = 0

            // IVP_Compact_Surface
            putf(out, CS+0, sc[0]*S); putf(out, CS+4, sc[1]*S); putf(out, CS+8, sc[2]*S);     // massCenter
            putf(out, CS+12, 1.f); putf(out, CS+16, 1.f); putf(out, CS+20, 1.f);              // rotInertia
            putf(out, CS+24, radius);                                                          // upper_radius
            put32(out, CS+28, SURF << 8);           // packed: byte_size=SURF, max_deviation=0
            put32(out, CS+32, nodeOff);             // offset_ledgetree_root (rel CS)
            put32(out, CS+44, 0x53505649);          // dummy[2] = 'IVPS'

            // IVP_Compact_Ledge
            const size_t L = CS + ledgeOff;
            put32(out, L+0, ptsOff - ledgeOff);                 // c_point_offset (rel ledge)
            put32(out, L+4, nodeOff - ledgeOff);                // ledgetree_node_offset (rel ledge)
            put32(out, L+8, ((1 + M + N) << 8) | 0x04);         // size_div16 | is_compact
            put16(out, L+12, static_cast<int16_t>(M));          // n_triangles

            // Triangles + opposite_index (reverse directed edge, signed 15-bit).
            std::vector<int> eS(M*3), eE(M*3);
            for (int t = 0; t < M; ++t)
                for (int e = 0; e < 3; ++e) { eS[t*3+e] = tris[t][e]; eE[t*3+e] = tris[t][(e+1)%3]; }
            for (int t = 0; t < M; ++t)
            {
                const size_t T = CS + triOff + t*16;
                put32(out, T, t);                               // tri_index
                for (int e = 0; e < 3; ++e)
                {
                    const int g = t*3+e; int opp = g;           // default self (open edge)
                    for (int k = 0; k < M*3; ++k) if (eS[k]==eE[g] && eE[k]==eS[g]) { opp = k; break; }
                    const int oi = opp - g;
                    put32(out, T+4+e*4, (tris[t][e] & 0xFFFF) | ((oi & 0x7FFF) << 16));
                }
            }

            // Points (metres).
            for (int v = 0; v < N; ++v)
            {
                const size_t P = CS + ptsOff + v*16;
                putf(out, P+0, solid.vertices[v*3+0]*S);
                putf(out, P+4, solid.vertices[v*3+1]*S);
                putf(out, P+8, solid.vertices[v*3+2]*S);
            }

            // Leaf ledgetree node.
            const size_t Nd = CS + nodeOff;
            put32(out, Nd+0, 0);                                // offset_right_node = 0 (leaf)
            put32(out, Nd+4, ledgeOff - nodeOff);               // offset_compact_ledge (rel node)
            putf(out, Nd+8, sc[0]*S); putf(out, Nd+12, sc[1]*S); putf(out, Nd+16, sc[2]*S);
            putf(out, Nd+20, radius);

            ++emitted;
        }

        if (emitted == 0)
            return std::vector<uint8_t>();      // signal caller to fall back to id=1 verbatim

        put32(out, 8, emitted);                 // solidCount = solids actually emitted
        const int32_t kvOff = static_cast<int32_t>(out.size() - bodyStart);
        put32(out, 16, kvOff);                  // keyValuesOffset (rel body start)

        if (keyValues && keyValuesLen > 0)
        {
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(keyValues),
                       reinterpret_cast<const uint8_t*>(keyValues) + keyValuesLen);
            if (out.back() != 0) out.push_back(0);
        }
        else
        {
            static const char kv[] = "solid {\n\"index\" \"0\"\n\"mass\" \"10.0\"\n}\n";
            out.insert(out.end(), kv, kv + sizeof(kv));
        }

        return out;
    }

    void PHYParser::PrintDebugInfo(const ParsedPHYData& data)
    {
        if (!data.valid)
        {
            printf("Invalid PHY data: %s\n", data.errorMessage.c_str());
            return;
        }

        printf("PHY Debug Info:\n");
        printf("  Checksum: 0x%08X\n", data.checksum);
        printf("  Solids: %zu\n", data.solids.size());

        for (size_t i = 0; i < data.solids.size(); i++)
        {
            const auto& solid = data.solids[i];
            printf("  Solid %zu:\n", i);
            printf("    Parent Bone: %d\n", solid.parentBone);
            printf("    Vertices: %zu\n", solid.vertices.size() / 3);
            printf("    Triangles: %zu\n", solid.triangles.size());
            printf("    Mass Center: (%.2f, %.2f, %.2f)\n",
                solid.massCenter[0], solid.massCenter[1], solid.massCenter[2]);
            printf("    Bounding Radius: %.2f\n", solid.boundingRadius);
            printf("    BB Min: (%.2f, %.2f, %.2f)\n",
                solid.bbMin[0], solid.bbMin[1], solid.bbMin[2]);
            printf("    BB Max: (%.2f, %.2f, %.2f)\n",
                solid.bbMax[0], solid.bbMax[1], solid.bbMax[2]);
        }
    }

    int32_t PHYParser::FindMaxVertexIndex(const std::vector<PHYTriangle>& triangles)
    {
        if (triangles.empty())
            return -1;

        int32_t maxIdx = 0;
        for (const auto& tri : triangles)
        {
            maxIdx = std::max({maxIdx, (int32_t)tri.v0, (int32_t)tri.v1, (int32_t)tri.v2});
        }
        return maxIdx;
    }

} // namespace collision
