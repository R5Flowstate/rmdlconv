// Copyright (c) 2024, rexx
// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/studio_r5_v19.h>
#include <studio/versions.h>
#include <studio/common.h>
#include <studio/optimize.h>
#include <collision/collision_generator.h>

/*
	Type:    RMDL
	Version: 19.1
	Game:    Apex Legends Season 19+

	Files: .rmdl, .vg (compressed, in rpak)

	This converter handles downgrading from v19.1 packed format to v10 (Season 2/3) format.
	Key challenges:
	- v19.1 uses 2-byte packing with 16-bit offsets (FIX_OFFSET is identity - direct byte offsets)
	- Bones are split into header, data, and linear arrays
	- Animations may have external animDataAsset GUIDs
*/

#define FILEBUFSIZE (32 * 1024 * 1024)

// Flag to strip from v19.1 mesh flags - v10 doesn't support UV2
#define VERTEX_HAS_UV2_FLAG 0x200000000ULL

// v19.1 animdesc bit meaning "anim data lives in an external asset". v54 has no
// such bit and this converter writes the data inline, so it must not survive.
static constexpr int kStudioAnimExternalFlag = 0x200000;

//
// FindBoneStateData_v191
// Purpose: Find the correct boneState data in v19.1 RMDL by pattern matching.
// The header's pBoneStates() offset can point to garbage in some v19.1 RMDLs.
// This function searches for a sequence of boneStateCount unique bytes that
// are all valid bone indices (< boneCount).
//
// Returns pointer to boneState data, or nullptr if not found.
//
static const uint8_t* FindBoneStateData_v191(const char* rmdlData, size_t rmdlSize,
                                             uint16_t boneStateCount, uint16_t totalBones)
{
	if (boneStateCount == 0 || totalBones == 0 || rmdlSize < boneStateCount)
		return nullptr;

	// Search BACKWARDS from end of file - boneState is typically near the end
	// in the embedded VG data section. Searching backwards avoids false positives
	// from bone name tables and other data earlier in the file.
	const size_t searchStart = 0x1000;  // Don't search in header area

	for (size_t off = rmdlSize - boneStateCount; off >= searchStart; off--)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(rmdlData + off);

		// Check if all values are valid bone indices AND all unique
		std::set<uint8_t> uniqueValues;
		bool valid = true;

		for (uint16_t i = 0; i < boneStateCount; i++)
		{
			if (p[i] >= totalBones)
			{
				valid = false;
				break;
			}
			uniqueValues.insert(p[i]);
		}

		// Must have ALL unique values (each hardware bone maps to different model bone)
		if (valid && uniqueValues.size() == boneStateCount)
		{
			// Additional validation: the 16 bytes before boneState should look like
			// a small header structure, not part of the data. Check for:
			// - header[0] = LOD count (1-8, non-zero)
			// - header[2-3] = flags (typically 01 01)
			// - header[4-15] = zeros (padding)
			// - header[15] must be 0 (not part of bone data)
			if (off >= 16)
			{
				const uint8_t* header = p - 16;
				bool looksLikeHeader = (header[0] >= 1 && header[0] <= 8  // Valid LOD count
					&& header[4] == 0 && header[8] == 0 && header[12] == 0  // Padding zeros
					&& header[15] == 0);  // Last byte before data must be 0
				if (looksLikeHeader)
				{
					return p;
				}
			}
		}
	}

	// Fallback: search forward without header check (less reliable)
	for (size_t off = searchStart; off < rmdlSize - boneStateCount; off++)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(rmdlData + off);

		std::set<uint8_t> uniqueValues;
		bool valid = true;

		for (uint16_t i = 0; i < boneStateCount; i++)
		{
			if (p[i] >= totalBones)
			{
				valid = false;
				break;
			}
			uniqueValues.insert(p[i]);
		}

		if (valid && uniqueValues.size() == boneStateCount)
		{
			return p;
		}
	}

	return nullptr;
}

// Convert v19.1 mesh flags to v10 format by stripping unsupported flags
static uint64_t ConvertMeshFlags_v191_to_v10(uint64_t v191Flags)
{
	// Strip VERTEX_HAS_UV2 - v10 format doesn't support this
	return v191Flags & ~VERTEX_HAS_UV2_FLAG;
}

// Calculate vertex size based on flags (without UV2)
static uint32_t CalculateVertexSize_v10(uint64_t flags)
{
	uint32_t size = 0;

	if (flags & 0x1) size += 12;      // VERTEX_HAS_POSITION
	if (flags & 0x2) size += 8;       // VERTEX_HAS_POSITION_PACKED
	if (flags & 0x10) size += 4;      // VERTEX_HAS_COLOR
	if (flags & 0x200) size += 4;     // VERTEX_HAS_NORMAL_PACKED
	if (flags & 0x1000) size += 4;    // VERTEX_HAS_WEIGHT_BONES
	if (flags & 0x2000) size += 8;    // VERTEX_HAS_WEIGHT_VALUES_N
	if (flags & 0x4000) size += 4;    // VERTEX_HAS_WEIGHT_VALUES_2
	if (flags & 0x2000000) size += 8; // VERTEX_HAS_UV1
	// Note: UV2 (0x200000000) is stripped, so we don't add 8 for it

	return size;
}

// V19.1 bone flag definitions
#define V191_BONE_FIXED_ALIGNMENT    0x00100000  // bit 20
// V10 bone flag definitions
#define V10_BONE_USED_BY_BONE_MERGE  0x00040000  // bit 18
#define BONE_USED_BY_VERTEX_MASK     0x0003FC00  // bits 10-17

// TranslateBoneFlags_v191_to_v10
// Purpose: Translate v19.1 bone flags to v10 format
//
// NOTE: BONE_FIXED_ALIGNMENT (0x100000) DOES exist in v10 format!
// Original v10 models have this flag set on bones. We should preserve it.
// The flag indicates the bone has a fixed qAlignment quaternion.
//
// FIX: Newer format models have BONE_USED_BY_BONE_MERGE (0x00040000) set on all bones,
// but original v10 models do NOT have this flag. This flag causes the
// "ref anim bone" issue where animations glitch. Strip it for v10 compatibility.
//
static int TranslateBoneFlags_v191_to_v10(int v191Flags) {
	// Strip BONE_USED_BY_BONE_MERGE (0x00040000) - newer formats set this but v10 originals don't have it
	// This fixes the "ref anim bone" glitching issue
	return v191Flags & ~V10_BONE_USED_BY_BONE_MERGE;
}

//
// ConvertVGData_191
// Purpose: converts v19.1 VG data (rev4 format without magic) to v8/v9 format (rev1 with '0tVG' magic)
//
static void ConvertVGData_191(const char* vgInputBuf, uintmax_t vgInputSize, const std::string& vgOutPath,
	const r5::v191::studiohdr_t* pRmdlHdr = nullptr, const char* rmdlData = nullptr, size_t rmdlSize = 0)
{
	printf("Converting v19.1 VG data (rev4) to v8/v9 format (rev1)...\n");

	const vg::rev4::VertexGroupHeader_t* pGroupHdr = reinterpret_cast<const vg::rev4::VertexGroupHeader_t*>(vgInputBuf);

	printf("  VG Header: lodIndex=%d, lodCount=%d, groupIndex=%d, lodMap=0x%02X\n",
		pGroupHdr->lodIndex, pGroupHdr->lodCount, pGroupHdr->groupIndex, pGroupHdr->lodMap);

	if (pGroupHdr->lodCount == 0)
	{
		printf("WARNING: VG has 0 LODs, skipping conversion\n");
		return;
	}

	// Calculate total sizes needed for output
	size_t totalMeshCount = 0;
	size_t totalVertexCount = 0;  // For legacyWeight
	size_t totalVertexBufSize = 0;
	size_t totalIndexBufSize = 0;
	size_t totalExtraWeightSize = 0;
	size_t totalStripCount = 0;

	// BoneStateChange data collection
	const uint8_t* pBoneStateData = nullptr;
	size_t boneStateChangeCount = 0;
	uint8_t maxBoneIndex = 0;

	// First pass: count all meshes and data sizes
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		printf("  LOD %d: meshCount=%d, meshIndex=%d\n", lodIdx, pLodHdr->meshCount, pLodHdr->meshIndex);

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			totalMeshCount++;
			totalVertexCount += pMesh->vertCount;  // Track total vertices for legacyWeight

			// Convert flags and calculate v10 vertex size (without UV2)
			uint64_t v191Flags = pMesh->flags;
			uint64_t v10Flags = ConvertMeshFlags_v191_to_v10(v191Flags);
			uint32_t v191VertSize = pMesh->vertCacheSize;
			uint32_t v10VertSize = CalculateVertexSize_v10(v10Flags);

			totalVertexBufSize += v10VertSize * pMesh->vertCount;
			totalIndexBufSize += pMesh->indexCount * sizeof(uint16_t);
			totalExtraWeightSize += pMesh->extraBoneWeightSize;

			// Scan vertices for bone indices if no RMDL bone state data
			if (pBoneStateData == nullptr && (pMesh->flags & 0x1000))  // VERTEX_HAS_WEIGHT_BONES
			{
				const char* pVerts = pMesh->pVertices();
				if (pVerts && pMesh->vertCount > 0)
				{
					for (uint32_t v = 0; v < pMesh->vertCount; v++)
					{
						const uint8_t* pBones = reinterpret_cast<const uint8_t*>(
							pVerts + v * pMesh->vertCacheSize + 12);
						for (int b = 0; b < 4; b++)
						{
							if (pBones[b] > maxBoneIndex)
								maxBoneIndex = pBones[b];
						}
					}
				}
			}

			// Each mesh with geometry gets one strip
			if (pMesh->flags != 0 && pMesh->vertCount > 0)
				totalStripCount++;

			printf("    Mesh %d: flags=0x%llX->0x%llX, verts=%d, indices=%d, vertSize=%d->%d\n",
				meshIdx, v191Flags, v10Flags, pMesh->vertCount, pMesh->indexCount,
				v191VertSize, v10VertSize);
		}
	}

	printf("  Total strips needed: %zu\n", totalStripCount);

	// Vector to hold boneState data (pattern search returns pointer, we need to copy)
	std::vector<uint8_t> boneStates;

	printf("  Total vertices: %zu\n", totalVertexCount);

	// Get boneStateChange - try pattern search first (more reliable), then header offset
	if (pRmdlHdr && pRmdlHdr->boneStateCount > 0 && rmdlData && rmdlSize > 0)
	{
		// Try pattern search first - this is more reliable than header offset
		pBoneStateData = FindBoneStateData_v191(rmdlData, rmdlSize,
			pRmdlHdr->boneStateCount, pRmdlHdr->boneCount);

		if (pBoneStateData)
		{
			boneStateChangeCount = pRmdlHdr->boneStateCount;
			ptrdiff_t foundOffset = pBoneStateData - reinterpret_cast<const uint8_t*>(rmdlData);
			printf("  BoneStateChange: %zu bones found by pattern search at offset 0x%X\n",
				boneStateChangeCount, (unsigned int)foundOffset);

			// Copy to vector for output
			boneStates.assign(pBoneStateData, pBoneStateData + boneStateChangeCount);

			printf("  First 10 bones: ");
			for (size_t i = 0; i < min((size_t)10, boneStateChangeCount); i++)
				printf("%d ", boneStates[i]);
			printf("\n");
		}
		else
		{
			// Fallback: try header offset (may work for some models)
			printf("  WARNING: Pattern search failed, trying header offset...\n");
			pBoneStateData = pRmdlHdr->pBoneStates();
			boneStateChangeCount = pRmdlHdr->boneStateCount;

			// Validate the data from header offset
			bool validData = true;
			std::set<uint8_t> uniqueCheck;
			for (size_t i = 0; i < boneStateChangeCount && validData; i++)
			{
				if (pBoneStateData[i] >= pRmdlHdr->boneCount)
					validData = false;
				uniqueCheck.insert(pBoneStateData[i]);
			}
			validData = validData && (uniqueCheck.size() == boneStateChangeCount);

			if (validData)
			{
				printf("  BoneStateChange: %zu bones from RMDL header (validated)\n", boneStateChangeCount);
				boneStates.assign(pBoneStateData, pBoneStateData + boneStateChangeCount);
			}
			else
			{
				printf("  WARNING: Header offset points to invalid data!\n");
				pBoneStateData = nullptr;
			}
		}
	}

	// Final fallback: sequential indices
	if (boneStates.empty() && maxBoneIndex > 0)
	{
		boneStateChangeCount = maxBoneIndex + 1;
		printf("  BoneStateChange: %zu bones (sequential fallback, max index: %d)\n",
			boneStateChangeCount, maxBoneIndex);
		boneStates.resize(boneStateChangeCount);
		for (size_t i = 0; i < boneStateChangeCount; i++)
			boneStates[i] = static_cast<uint8_t>(i);
	}

	// Calculate unknown data count (one entry per mesh in first LOD, typically same across all LODs)
	size_t unknownCount = (pGroupHdr->lodCount > 0) ? (totalMeshCount / pGroupHdr->lodCount) : 0;
	printf("  Unknown data entries needed: %zu\n", unknownCount);

	// LegacyWeight: 16 bytes per vertex (4 floats for bone weights)
	size_t legacyWeightSize = totalVertexCount * 16;
	printf("  LegacyWeight size: %zu bytes (%zu vertices * 16)\n", legacyWeightSize, totalVertexCount);

	// Allocate output buffer
	// Structure: [Header][BoneStateChange][Meshes][Indices][Vertices][Weights][Unknown][LODs][LegacyWeight][Strips]
	size_t outputBufSize = sizeof(vg::rev1::VertexGroupHeader_t)
		+ boneStates.size()  // BoneStateChange section
		+ (totalMeshCount * sizeof(vg::rev1::MeshHeader_t))
		+ totalIndexBufSize + 16  // indices + alignment
		+ totalVertexBufSize + 16 // vertices + alignment
		+ totalExtraWeightSize
		+ (unknownCount * sizeof(vg::rev1::UnkVgData_t))
		+ (pGroupHdr->lodCount * sizeof(vg::rev1::ModelLODHeader_t))
		+ legacyWeightSize  // LegacyWeight section
		+ (totalStripCount * sizeof(OptimizedModel::StripHeader_t))
		+ 4096; // Extra padding

	std::unique_ptr<char[]> outputBuf(new char[outputBufSize]);
	memset(outputBuf.get(), 0, outputBufSize);

	char* pWrite = outputBuf.get();
	char* pBufferEnd = outputBuf.get() + outputBufSize;

	// Write rev1 header
	vg::rev1::VertexGroupHeader_t* pOutHdr = reinterpret_cast<vg::rev1::VertexGroupHeader_t*>(pWrite);
	memset(pOutHdr, 0, sizeof(vg::rev1::VertexGroupHeader_t));
	pOutHdr->id = 'GVt0'; // '0tVG' magic
	pOutHdr->version = 1;
	pOutHdr->unk = 0;
	pOutHdr->lodCount = pGroupHdr->lodCount;
	pOutHdr->meshCount = totalMeshCount;
	pWrite += sizeof(vg::rev1::VertexGroupHeader_t);

	// Write BoneStateChange section immediately after header (like v16)
	char* pBoneStateChange = pWrite;
	pOutHdr->boneStateChangeOffset = pBoneStateChange - outputBuf.get();
	pOutHdr->boneStateChangeCount = boneStates.size();

	if (!boneStates.empty())
	{
		memcpy(pWrite, boneStates.data(), boneStates.size());
		pWrite += boneStates.size();
	}

	// Mesh headers
	char* pMeshStart = pWrite;
	pOutHdr->meshOffset = pMeshStart - outputBuf.get();
	pWrite = pMeshStart + (totalMeshCount * sizeof(vg::rev1::MeshHeader_t));

	// Align for index data
	pWrite = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(pWrite) + 15) & ~15);

	// Index data
	char* pIndexData = pWrite;
	char* pVertexData = nullptr;
	char* pWeightData = nullptr;
	char* pStripData = nullptr;
	pOutHdr->indexOffset = pIndexData - outputBuf.get();

	// Copy index data first
	size_t indexOffset = 0;
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const uint16_t* pSrcIndices = pMesh->pIndices();
			if (pSrcIndices && pMesh->indexCount > 0)
			{
				size_t indexSize = pMesh->indexCount * sizeof(uint16_t);
				memcpy(pWrite, pSrcIndices, indexSize);
				pWrite += indexSize;
			}
		}
	}

	// Align
	pWrite = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(pWrite) + 15) & ~15);
	pVertexData = pWrite;

	// Copy vertex data
	size_t vertexOffset = 0;
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const char* pSrcVerts = pMesh->pVertices();
			if (pSrcVerts && pMesh->vertCount > 0)
			{
				uint64_t v191Flags = pMesh->flags;
				uint64_t v10Flags = ConvertMeshFlags_v191_to_v10(v191Flags);
				uint32_t v191VertSize = pMesh->vertCacheSize;
				uint32_t v10VertSize = CalculateVertexSize_v10(v10Flags);

				// If source vertex size differs from output, need to copy vertex by vertex
				if (v191VertSize != v10VertSize)
				{
					// Copy each vertex with the correct output size
					// This handles UV2 stripping and any other size differences
					for (uint32_t v = 0; v < pMesh->vertCount; v++)
					{
						memcpy(pWrite, pSrcVerts + v * v191VertSize, v10VertSize);
						pWrite += v10VertSize;
					}
				}
				else
				{
					// Same size, copy as-is
					memcpy(pWrite, pSrcVerts, pMesh->vertBufferSize);
					pWrite += pMesh->vertBufferSize;
				}
			}
		}
	}

	// Align
	pWrite = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(pWrite) + 15) & ~15);
	pWeightData = pWrite;

	// Copy extra bone weight data
	size_t weightOffset = 0;
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const char* pSrcWeights = pMesh->pBoneWeights();
			if (pSrcWeights && pMesh->extraBoneWeightSize > 0)
			{
				memcpy(pWrite, pSrcWeights, pMesh->extraBoneWeightSize);
				pWrite += pMesh->extraBoneWeightSize;
			}
		}
	}

	// Write unknown data (UnkVgData_t entries - one per mesh in first LOD)
	char* pUnknownData = pWrite;
	if (unknownCount > 0)
	{
		// Zero-fill the unknown data entries (0x30 bytes each)
		memset(pWrite, 0, unknownCount * sizeof(vg::rev1::UnkVgData_t));
		pWrite += unknownCount * sizeof(vg::rev1::UnkVgData_t);
	}

	// LOD headers
	char* pLodStart = pWrite;
	pOutHdr->lodOffset = pLodStart - outputBuf.get();
	pWrite += pGroupHdr->lodCount * sizeof(vg::rev1::ModelLODHeader_t);

	// LegacyWeight data (16 bytes per vertex)
	char* pLegacyWeight = pWrite;
	pOutHdr->legacyWeightOffset = pLegacyWeight - outputBuf.get();
	pOutHdr->legacyWeightCount = totalVertexCount;

	// Generate default legacy weights
	for (size_t v = 0; v < totalVertexCount; v++)
	{
		float* pWeights = reinterpret_cast<float*>(pWrite);
		pWeights[0] = 1.0f;  // First bone gets full weight
		pWeights[1] = 0.0f;
		pWeights[2] = 0.0f;
		pWeights[3] = 0.0f;
		pWrite += 16;
	}

	// Write strip data
	pStripData = pWrite;
	size_t stripIndex = 0;
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			// Only create strips for meshes with actual geometry
			if (pMesh->flags != 0 && pMesh->vertCount > 0)
			{
				OptimizedModel::StripHeader_t* pStrip = reinterpret_cast<OptimizedModel::StripHeader_t*>(pWrite);
				memset(pStrip, 0, sizeof(OptimizedModel::StripHeader_t));

				pStrip->numIndices = pMesh->indexCount;
				pStrip->indexOffset = 0; // Offset within strip (always 0 for single strip)
				pStrip->numVerts = pMesh->vertCount;
				pStrip->vertOffset = 0;  // Offset within strip (always 0 for single strip)
				pStrip->numBones = pMesh->vertBoneCount;  // Copy bone count from v16/v19.1 mesh header
				pStrip->flags = OptimizedModel::STRIP_IS_TRILIST; // 0x01

				pWrite += sizeof(OptimizedModel::StripHeader_t);
				stripIndex++;
			}
		}
	}

	// Now fill in the header offsets
	pOutHdr->indexOffset = pIndexData - outputBuf.get();
	pOutHdr->indexCount = 0;
	pOutHdr->vertOffset = pVertexData - outputBuf.get();
	pOutHdr->vertBufferSize = 0;
	pOutHdr->extraBoneWeightOffset = pWeightData - outputBuf.get();
	pOutHdr->extraBoneWeightSize = 0;
	pOutHdr->unknownOffset = pUnknownData - outputBuf.get();
	pOutHdr->unknownCount = unknownCount;
	pOutHdr->stripOffset = pStripData - outputBuf.get();
	pOutHdr->stripCount = totalStripCount;

	// Fill in LOD and mesh headers with correct offsets
	short meshStartIdx = 0;
	indexOffset = 0;
	vertexOffset = 0;
	weightOffset = 0;
	size_t legacyWeightIdx = 0;  // Running index into legacyWeight buffer
	stripIndex = 0;

	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		vg::rev1::ModelLODHeader_t* pOutLod = reinterpret_cast<vg::rev1::ModelLODHeader_t*>(pLodStart) + lodIdx;
		pOutLod->meshOffset = meshStartIdx;
		pOutLod->meshCount = pLodHdr->meshCount;
		pOutLod->switchPoint = 0.0f; // Default switch point

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			vg::rev1::MeshHeader_t* pOutMesh = reinterpret_cast<vg::rev1::MeshHeader_t*>(pMeshStart) + meshStartIdx;
			memset(pOutMesh, 0, sizeof(vg::rev1::MeshHeader_t));

			// Convert flags and calculate v10 vertex size
			uint64_t v10Flags = ConvertMeshFlags_v191_to_v10(pMesh->flags);
			uint32_t v10VertCacheSize = CalculateVertexSize_v10(v10Flags);

			pOutMesh->flags = v10Flags;
			pOutMesh->vertOffset = static_cast<uint32_t>(vertexOffset);
			pOutMesh->vertCacheSize = v10VertCacheSize;
			pOutMesh->vertCount = static_cast<uint32_t>(pMesh->vertCount);
			pOutMesh->indexOffset = static_cast<int>(indexOffset / sizeof(uint16_t));
			pOutMesh->indexCount = static_cast<int>(pMesh->indexCount);
			pOutMesh->extraBoneWeightOffset = static_cast<int>(weightOffset);
			pOutMesh->extraBoneWeightSize = static_cast<int>(pMesh->extraBoneWeightSize);

			// Set legacyWeight offsets for this mesh
			pOutMesh->legacyWeightOffset = static_cast<int>(legacyWeightIdx);
			pOutMesh->legacyWeightCount = static_cast<int>(pMesh->vertCount);

			// Set strip data for meshes with geometry
			if (pMesh->flags != 0 && pMesh->vertCount > 0)
			{
				pOutMesh->stripOffset = static_cast<int>(stripIndex);
				pOutMesh->stripCount = 1;
				stripIndex++;
			}
			else
			{
				pOutMesh->stripOffset = 0;
				pOutMesh->stripCount = 0;
			}

			// Update running offsets
			indexOffset += pMesh->indexCount * sizeof(uint16_t);
			vertexOffset += v10VertCacheSize * pMesh->vertCount;  // Use v10 size
			weightOffset += pMesh->extraBoneWeightSize;
			legacyWeightIdx += pMesh->vertCount;

			// Update header totals
			pOutHdr->indexCount += pMesh->indexCount;
			pOutHdr->vertBufferSize += v10VertCacheSize * pMesh->vertCount;  // Use v10 size
			pOutHdr->extraBoneWeightSize += pMesh->extraBoneWeightSize;

			meshStartIdx++;
		}
	}

	// Set data size
	pOutHdr->dataSize = static_cast<int>(pWrite - outputBuf.get());

	// Check for buffer overflow
	if (pWrite > pBufferEnd)
	{
		printf("[ERROR] VG CONV: Buffer overflow! Used %lld bytes more than allocated\n",
			(long long)(pWrite - pBufferEnd));
	}

	// Write output file
	std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
	vgOut.write(outputBuf.get(), pOutHdr->dataSize);
	vgOut.close();

	printf("VG conversion complete: %d LODs, %zu meshes, %zu unknowns, %zu strips, %d bytes\n",
		pGroupHdr->lodCount, totalMeshCount, unknownCount, totalStripCount, pOutHdr->dataSize);
}

//
// ConvertStudioHdr_191
// Purpose: converts the v19.1 studiohdr_t to v10 compatible (r5::v8::studiohdr_t)
//
static void ConvertStudioHdr_191(r5::v8::studiohdr_t* out, const r5::v191::studiohdr_t* hdr, const char* pInputData)
{
	// Zero-initialize the entire header first to ensure all offsets default to 0
	memset(out, 0, sizeof(r5::v8::studiohdr_t));

	out->id = 'TSDI';
	out->version = 54;

	out->checksum = hdr->checksum;

	// Name will be handled separately with string table
	memset(out->name, 0, sizeof(out->name));

	out->length = 0xbadf00d; // needs to be written later

	// These vectors need to be read from packed format
	out->eyeposition = Vector(0, 0, 0); // Will compute from bones if available
	out->illumposition = hdr->illumposition;
	out->hull_min = hdr->hull_min;
	out->hull_max = hdr->hull_max;

	out->mins = hdr->hull_min;
	out->maxs = hdr->hull_max;

	out->view_bbmin = hdr->view_bbmin;
	out->view_bbmax = hdr->view_bbmax;

	// Copy flags with filtering for v10 compatibility
	// Clear flags that v19.1 sets but v10 originals don't have:
	// - STUDIOHDR_FLAGS_USES_UV2 (0x2000000) - we strip UV2 data
	// - STUDIOHDR_FLAGS_AMBIENT_BOOST (0x10000) - not in v10 models
	// - STUDIOHDR_FLAGS_SUBDIVISION_SURFACE (0x80000) - not in v10 models
	int headerFlags = hdr->flags;
	headerFlags &= ~0x2000000;  // Clear USES_UV2
	headerFlags &= ~0x10000;    // Clear AMBIENT_BOOST
	headerFlags &= ~0x80000;    // Clear SUBDIVISION_SURFACE
	out->flags = headerFlags;

	// Count vars
	out->numbones = hdr->boneCount;
	out->numbonecontrollers = 0; // Not used in v19.1
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalanim = 0; // deprecated
	out->numlocalseq = hdr->numlocalseq;
	out->activitylistversion = hdr->activitylistversion;

	out->numtextures = hdr->numtextures;
	out->numcdtextures = 1; // We'll generate a single empty cdtexture
	out->numskinref = hdr->numskinref;
	out->numskinfamilies = hdr->numskinfamilies;
	out->numbodyparts = hdr->numbodyparts;
	out->numlocalattachments = hdr->numlocalattachments;

	out->numlocalnodes = hdr->numlocalnodes;
	out->numikchains = hdr->numikchains;
	out->numlocalposeparameters = hdr->numlocalposeparameters;
	out->numsrcbonetransform = hdr->numsrcbonetransform;

	out->numincludemodels = -1; // No include models

	// Misc vars
	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->fadeDistance;
	out->flVertAnimFixedPointScale = 1.0f; // Default scale

	// Explicitly set sourceFilenameOffset to 0 (no maya strings)
	out->sourceFilenameOffset = 0;

	// VG/physics file info - will need to be computed
	out->phyOffset = -123456;  // Sentinel for external .phy file
	out->vtxOffset = 0;
	out->vvdOffset = 0;
	out->vvcOffset = 0;
	out->vvwOffset = 0;
	out->vtxSize = 0;
	out->vvdSize = 0;
	out->vvcSize = 0;
	out->vvwSize = 0;
	out->phySize = 0;  // Will be set later if .phy file exists

	// unk4_v54[2] at +0x218/+0x21A is the BVH4 collision-part selector.
	// STATIC_PROP is a placeholder until post-collision resolve; animated keeps source mapping.
	constexpr uint32_t kFlagsStaticProp = 0x10;
	if (hdr->flags & kFlagsStaticProp)
	{
		out->unk4_v54[0] = 0;
		out->unk4_v54[1] = 0;
	}
	else
	{
		out->unk4_v54[0] = static_cast<short>(static_cast<int8_t>(hdr->bvhUnk[0]));
		out->unk4_v54[1] = static_cast<short>(static_cast<int8_t>(hdr->bvhUnk[1]));
	}
}

//
// GenerateRigHdr_191
// Purpose: generates a rig header from the v19.1 model header
//
static void GenerateRigHdr_191(r5::v8::studiohdr_t* out, const r5::v191::studiohdr_t* hdr)
{
	// Zero-initialize the entire header first
	memset(out, 0, sizeof(r5::v8::studiohdr_t));

	out->id = 'TSDI';
	out->version = 54;

	out->numbones = hdr->boneCount;
	out->numbonecontrollers = 0;
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalattachments = hdr->numlocalattachments;
	out->numlocalnodes = hdr->numlocalnodes;
	out->numikchains = hdr->numikchains;
	out->numlocalposeparameters = hdr->numlocalposeparameters;

	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->fadeDistance;
}

//
// ConvertBones_191
// Purpose: converts v19.1 split bone data (header + data + linear) to v10 monolithic bone format
//
static void ConvertBones_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numBones, bool isRig)
{
	printf("converting %i bones...\n", numBones);

	std::vector<r5::v8::mstudiobone_t*> proceduralBones;

	char* pBoneStart = g_model.pData;
	g_model.hdrV54()->boneindex = g_model.pData - g_model.pBase;

	const r5::v191::mstudiolinearbone_t* pLinearBone = r5::v191::GetLinearBone(pOldHdr);

	// Validate linear bone data
	if (pLinearBone && pLinearBone->numbones != numBones)
		pLinearBone = nullptr;

	for (int i = 0; i < numBones; ++i)
	{
		const r5::v191::mstudiobonehdr_t* oldBoneHdr = r5::v191::GetBoneHdr(pOldHdr, i);
		const r5::v191::mstudiobonedata_t* oldBoneData = r5::v191::GetBoneData(pOldHdr, i);

		r5::v8::mstudiobone_t* newBone = reinterpret_cast<r5::v8::mstudiobone_t*>(g_model.pData) + i;

		AddToStringTable((char*)newBone, &newBone->sznameindex, oldBoneHdr->pszName());

		const char* surfaceProp = (char*)oldBoneHdr + FIX_OFFSET(oldBoneHdr->surfacepropidx);
		AddToStringTable((char*)newBone, &newBone->surfacepropidx, surfaceProp);

		newBone->parent = oldBoneData->parent;
		newBone->flags = TranslateBoneFlags_v191_to_v10(oldBoneData->flags);  // Translate flags
		newBone->proctype = oldBoneData->proctype;
		newBone->procindex = oldBoneData->procindex;
		newBone->contents = oldBoneHdr->contents;
		newBone->surfacepropLookup = oldBoneHdr->surfacepropLookup;
		newBone->physicsbone = oldBoneHdr->physicsbone;

		// Convert collision index (0xFF in v19.1 means -1)
		newBone->collisionIndex = oldBoneData->collisionIndex == 0xFF ? -1 : oldBoneData->collisionIndex;

		// Bone controllers (not used in newer formats)
		memset(&newBone->bonecontroller, -1, sizeof(newBone->bonecontroller));

		// Pose data from linear bone arrays
		if (pLinearBone && pLinearBone->numbones > 0)
		{
			newBone->pos = *pLinearBone->pPos(i);
			newBone->quat = *pLinearBone->pQuat(i);
			newBone->rot = *pLinearBone->pRot(i);
			newBone->scale = *pLinearBone->pScale(i);
			newBone->poseToBone = *pLinearBone->pPoseToBone(i);
			newBone->qAlignment = *pLinearBone->pQAlignment(i);
		}
		else
		{
			// Fallback - use identity transforms
			newBone->pos = Vector(0, 0, 0);
			newBone->quat = Quaternion(0, 0, 0, 1);
			newBone->rot = RadianEuler(0, 0, 0);
			newBone->scale = Vector(1, 1, 1);
			newBone->poseToBone.Init(Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1), Vector(0, 0, 0));
			newBone->qAlignment = Quaternion(0, 0, 0, 1);
		}

		// Only process JIGGLE bones (proctype == 5)
		// Other proctypes (AXISINTERP=1, QUATINTERP=2, AIMATBONE=3, AIMATATTACH=4, TWIST_MASTER=6, TWIST_SLAVE=7)
		// are not supported in v10 format
		const int STUDIO_PROC_JIGGLE = 5;
		if (oldBoneData->proctype == STUDIO_PROC_JIGGLE)
			proceduralBones.push_back(newBone);
		else if (oldBoneData->proctype > 0)
		{
			// Clear proctype for unsupported proc bone types
			newBone->proctype = 0;
			newBone->procindex = 0;
		}
	}

	g_model.pData += numBones * sizeof(r5::v8::mstudiobone_t);
	ALIGN4(g_model.pData);

	// Rigs do not have proc bones
	if (proceduralBones.empty())
		return;

	printf("copying %lld jiggle bones...\n", proceduralBones.size());

	std::map<uint8_t, uint8_t> linearprocbones;

	for (auto bone : proceduralBones)
	{
		int boneid = ((char*)bone - pBoneStart) / sizeof(r5::v8::mstudiobone_t);
		const r5::v191::mstudiobonedata_t* oldBoneData = r5::v191::GetBoneData(pOldHdr, boneid);

		void* oldJBone = (char*)oldBoneData + FIX_OFFSET(oldBoneData->procindex);

		r5::v8::mstudiojigglebone_t* jBone = reinterpret_cast<r5::v8::mstudiojigglebone_t*>(g_model.pData);

		bone->procindex = (char*)jBone - (char*)bone;

		memcpy_s(jBone, sizeof(r5::v8::mstudiojigglebone_t), oldJBone, sizeof(r5::v8::mstudiojigglebone_t));

		linearprocbones.emplace(jBone->bone, static_cast<uint8_t>(linearprocbones.size()));

		g_model.pData += sizeof(r5::v8::mstudiojigglebone_t);
	}

	ALIGN4(g_model.pData);

	if (linearprocbones.empty())
		return;

	g_model.hdrV54()->procBoneCount = static_cast<int>(linearprocbones.size());
	g_model.hdrV54()->procBoneTableOffset = static_cast<int>(g_model.pData - g_model.pBase);

	for (auto& it : linearprocbones)
	{
		*g_model.pData = it.first;
		g_model.pData += sizeof(uint8_t);
	}

	g_model.hdrV54()->linearProcBoneOffset = static_cast<int>(g_model.pData - g_model.pBase);

	for (int i = 0; i < numBones; i++)
	{
		*g_model.pData = linearprocbones.count(i) ? linearprocbones.find(i)->second : 0xff;
		g_model.pData += sizeof(uint8_t);
	}

	ALIGN4(g_model.pData);
}

//
// ConvertHitboxes_191
// Purpose: converts v19.1 hitbox sets and hitboxes to v10 format
//
static void ConvertHitboxes_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numHitboxSets)
{
	printf("converting %i hitboxsets...\n", numHitboxSets);

	g_model.hdrV54()->hitboxsetindex = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v191::mstudiohitboxset_t* pOldHitboxSets = reinterpret_cast<const r5::v191::mstudiohitboxset_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->hitboxsetindex));

	mstudiohitboxset_t* hboxsetStart = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);

	// Write hitbox set headers
	for (int i = 0; i < numHitboxSets; ++i)
	{
		const r5::v191::mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);

		AddToStringTable((char*)newhboxset, &newhboxset->sznameindex, oldhboxset->pszName());
		newhboxset->numhitboxes = oldhboxset->numhitboxes;
		newhboxset->hitboxindex = 0; // Will be set later

		g_model.pData += sizeof(mstudiohitboxset_t);
	}

	// Write hitboxes for each set
	for (int i = 0; i < numHitboxSets; ++i)
	{
		const r5::v191::mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = hboxsetStart + i;

		newhboxset->hitboxindex = static_cast<int>(g_model.pData - (char*)newhboxset);

		for (int j = 0; j < oldhboxset->numhitboxes; ++j)
		{
			const r5::v191::mstudiobbox_t* oldHitbox = oldhboxset->pHitbox(j);
			r5::v8::mstudiobbox_t* newHitbox = reinterpret_cast<r5::v8::mstudiobbox_t*>(g_model.pData);

			newHitbox->bone = oldHitbox->bone;
			newHitbox->group = oldHitbox->group;
			newHitbox->bbmin = oldHitbox->bbmin;
			newHitbox->bbmax = oldHitbox->bbmax;

			AddToStringTable((char*)newHitbox, &newHitbox->szhitboxnameindex, oldHitbox->pszHitboxName());

			// Hit data group
			const char* hitDataGroup = (char*)oldHitbox + FIX_OFFSET(oldHitbox->hitdataGroupOffset);
			AddToStringTable((char*)newHitbox, &newHitbox->hitdataGroupOffset, hitDataGroup);

			g_model.pData += sizeof(r5::v8::mstudiobbox_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertAttachments_191
// Purpose: converts v19.1 attachments to v10 format
//
static int ConvertAttachments_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numAttachments)
{
	printf("converting %i attachments...\n", numAttachments);

	int index = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v191::mstudioattachment_t* pOldAttachments = reinterpret_cast<const r5::v191::mstudioattachment_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->localattachmentindex));

	for (int i = 0; i < numAttachments; ++i)
	{
		const r5::v191::mstudioattachment_t* oldAttach = &pOldAttachments[i];
		r5::v8::mstudioattachment_t* newAttach = reinterpret_cast<r5::v8::mstudioattachment_t*>(g_model.pData) + i;

		AddToStringTable((char*)newAttach, &newAttach->sznameindex, oldAttach->pszName());
		newAttach->flags = oldAttach->flags;
		newAttach->localbone = oldAttach->localbone;
		memcpy(&newAttach->localmatrix, &oldAttach->local, sizeof(oldAttach->local));
	}

	g_model.pData += numAttachments * sizeof(r5::v8::mstudioattachment_t);
	ALIGN4(g_model.pData);

	return index;
}

//
// ConvertBodyParts_191
// Purpose: converts v19.1 bodyparts, models, and meshes to v10 format
//
static void ConvertBodyParts_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numBodyParts)
{
	printf("converting %i bodyparts...\n", numBodyParts);

	g_model.hdrV54()->bodypartindex = static_cast<int>(g_model.pData - g_model.pBase);

	mstudiobodyparts_t* bodypartStart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

	// Write bodypart headers
	for (int i = 0; i < numBodyParts; ++i)
	{
		const r5::v191::mstudiobodyparts_t* oldbodypart = pOldHdr->pBodypart(i);
		mstudiobodyparts_t* newbodypart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

		AddToStringTable((char*)newbodypart, &newbodypart->sznameindex, oldbodypart->pszName());
		newbodypart->nummodels = oldbodypart->nummodels;
		newbodypart->base = oldbodypart->base;

		printf("  bodypart: %s\n", oldbodypart->pszName());

		g_model.pData += sizeof(mstudiobodyparts_t);
	}

	// Write models and meshes for each bodypart
	for (int i = 0; i < numBodyParts; ++i)
	{
		const r5::v191::mstudiobodyparts_t* oldbodypart = pOldHdr->pBodypart(i);
		mstudiobodyparts_t* newbodypart = bodypartStart + i;

		newbodypart->modelindex = static_cast<int>(g_model.pData - (char*)newbodypart);

		r5::v8::mstudiomodel_t* newModels = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);

		// Write model headers
		for (int j = 0; j < oldbodypart->nummodels; ++j)
		{
			const r5::v191::mstudiomodel_t* oldModel = oldbodypart->pModel(j);
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			// v19.1 model uses unkStringOffset for name
			memset(newModel->name, 0, sizeof(newModel->name));
			const char* modelName = oldModel->pszString();
			if (modelName && *modelName)
			{
				strncpy_s(newModel->name, sizeof(newModel->name), modelName, _TRUNCATE);
			}

			newModel->type = 0; // Standard model type
			newModel->boundingradius = 0.0f; // Not stored in v19.1
			newModel->nummeshes = oldModel->meshCountTotal;
			newModel->meshindex = 0; // Will be set later

			// Vertex data indices will reference VG file
			newModel->numvertices = 0;
			newModel->vertexindex = 0;
			newModel->tangentsindex = 0;
			newModel->numattachments = 0;
			newModel->attachmentindex = 0;
			newModel->deprecated_numeyeballs = 0;
			newModel->deprecated_eyeballindex = 0;
			newModel->colorindex = 0;
			newModel->uv2index = 0;
		}

		g_model.pData += oldbodypart->nummodels * sizeof(r5::v8::mstudiomodel_t);

		// Write meshes for each model
		for (int j = 0; j < oldbodypart->nummodels; ++j)
		{
			const r5::v191::mstudiomodel_t* oldModel = oldbodypart->pModel(j);
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			newModel->meshindex = static_cast<int>(g_model.pData - (char*)newModel);

			r5::v8::mstudiomesh_t* newMeshes = reinterpret_cast<r5::v8::mstudiomesh_t*>(g_model.pData);

			for (int k = 0; k < oldModel->meshCountTotal; ++k)
			{
				const r5::v191::mstudiomesh_t* oldMesh = oldModel->pMesh(k);
				r5::v8::mstudiomesh_t* newMesh = newMeshes + k;

				newMesh->material = oldMesh->material;
				newMesh->meshid = oldMesh->meshid;
				newMesh->center = oldMesh->center;

				// Vertex data will be filled from VG conversion
				newMesh->numvertices = 0;
				newMesh->vertexoffset = 0;
				newMesh->vertexloddata.numLODVertexes[0] = 0;

				newMesh->modelindex = static_cast<int>((char*)newModel - (char*)newMesh);
			}

			g_model.pData += oldModel->meshCountTotal * sizeof(r5::v8::mstudiomesh_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertTextures_191
// Purpose: converts v19.1 texture references to v10 format
//
static void ConvertTextures_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numTextures)
{
	printf("converting %i textures...\n", numTextures);

	g_model.hdrV54()->textureindex = static_cast<int>(g_model.pData - g_model.pBase);

	// Read original material GUIDs from v19.1 data
	int textureDataOffset = FIX_OFFSET(pOldHdr->textureindex);
	const uint64_t* pOldTextureGuids = reinterpret_cast<const uint64_t*>(
		(const char*)pOldHdr + textureDataOffset);

	for (int i = 0; i < numTextures; ++i)
	{
		uint64_t materialGuid = pOldTextureGuids[i];
		r5::v8::mstudiotexture_t* newTexture = reinterpret_cast<r5::v8::mstudiotexture_t*>(g_model.pData);

		// Use default empty material name - engine will resolve by GUID
		char* textureName = new char[32];
		snprintf(textureName, 32, "dev/empty");
		AddToStringTable((char*)newTexture, &newTexture->sznameindex, textureName);

		// Keep original GUID - engine uses GUID lookup
		newTexture->textureGuid = materialGuid;

		printf("  texture %d: GUID=0x%016llX\n", i, materialGuid);
		g_model.pData += sizeof(r5::v8::mstudiotexture_t);
	}

	// Material shader types - use RGDP for static props
	ALIGN4(g_model.pData);

	g_model.hdrV54()->materialtypesindex = static_cast<int>(g_model.pData - g_model.pBase);
	memset(g_model.pData, RGDP, numTextures);
	g_model.pData += numTextures;

	ALIGN4(g_model.pData);

	// Write static cdtexture data
	g_model.hdrV54()->cdtextureindex = static_cast<int>(g_model.pData - g_model.pBase);
	AddToStringTable(g_model.pBase, (int*)g_model.pData, "");
	g_model.pData += sizeof(int);
}

//
// ConvertSkins_191
// Purpose: converts v19.1 skin data to v10 format
//
// V19.1 skin data layout (NO alignment between sections):
//   1. short skins[numSkinfamilies][numSkinRef] - skin lookup table
//   2. uint16_t skinNameOffsets[numSkinfamilies - 1] - offsets to skin names (relative to header)
//
// V10 skin data layout (WITH alignment):
//   1. short skins[numSkinfamilies][numSkinRef] - skin lookup table
//   2. ALIGN4
//   3. int skinNameOffsets[numSkinfamilies - 1] - offsets to skin names (relative to model base)
//
static void ConvertSkins_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numSkinRef, int numSkinFamilies)
{
	printf("converting %i skins (%i skinrefs)...\n", numSkinFamilies, numSkinRef);

	g_model.hdrV54()->skinindex = static_cast<int>(g_model.pData - g_model.pBase);

	const char* pOldSkinData = (const char*)pOldHdr + FIX_OFFSET(pOldHdr->skinindex);

	// Copy skin index data (short array: skins[numSkinfamilies][numSkinRef])
	int skinIndexDataSize = sizeof(short) * numSkinRef * numSkinFamilies;
	memcpy(g_model.pData, pOldSkinData, skinIndexDataSize);

	// Debug: print skin lookup table
	const short* skinLookup = reinterpret_cast<const short*>(pOldSkinData);
	for (int family = 0; family < numSkinFamilies; family++)
	{
		printf("  skin family %d: [", family);
		for (int ref = 0; ref < numSkinRef; ref++)
		{
			if (ref > 0) printf(", ");
			if (ref < 8)  // Limit output for readability
				printf("%d", skinLookup[family * numSkinRef + ref]);
			else if (ref == 8)
				printf("...");
		}
		printf("]\n");
	}

	g_model.pData += skinIndexDataSize;

	// V10 requires alignment before skin name offsets
	ALIGN4(g_model.pData);

	// Write skin group names
	// Skin 0 is "default" (unnamed), so only (numSkinFamilies - 1) names exist
	if (numSkinFamilies > 1)
	{
		// V19.1 stores skin name offsets as uint16_t IMMEDIATELY after skin data (no alignment!)
		// These offsets are relative to the header start
		const uint16_t* pOldSkinNameOffsets = reinterpret_cast<const uint16_t*>(
			pOldSkinData + skinIndexDataSize);

		for (int i = 0; i < numSkinFamilies - 1; ++i)
		{
			uint16_t nameOffset = pOldSkinNameOffsets[i];

			// Validate offset before dereferencing
			if (nameOffset > 0 && nameOffset < 0xFFFF)
			{
				// V19.1 offsets are relative to header start
				const char* skinName = (const char*)pOldHdr + FIX_OFFSET(nameOffset);

				// Validate string pointer (basic sanity check)
				if (skinName && skinName[0] != '\0' && strlen(skinName) < 256)
				{
					printf("  skin %d name: '%s' (offset 0x%04X)\n", i + 1, skinName, nameOffset);
					AddToStringTable(g_model.pBase, (int*)g_model.pData, skinName);
				}
				else
				{
					// Fallback to generated name if string looks invalid
					printf("  skin %d name: generated (invalid string at offset 0x%04X)\n", i + 1, nameOffset);
					char* skinNameBuf = new char[32];
					sprintf_s(skinNameBuf, 32, "skin%d", i + 1);
					AddToStringTable(g_model.pBase, (int*)g_model.pData, skinNameBuf);
				}
			}
			else
			{
				// Fallback to generated name if offset is invalid
				printf("  skin %d name: generated (invalid offset 0x%04X)\n", i + 1, nameOffset);
				char* skinNameBuf = new char[32];
				sprintf_s(skinNameBuf, 32, "skin%d", i + 1);
				AddToStringTable(g_model.pBase, (int*)g_model.pData, skinNameBuf);
			}

			g_model.pData += sizeof(int);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertIkChains_191
// Purpose: converts v19.1 IK chains to v10 format
//
static void ConvertIkChains_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numIkChains, bool isRig)
{
	g_model.hdrV54()->ikchainindex = static_cast<int>(g_model.pData - g_model.pBase);

	if (numIkChains == 0)
		return;

	printf("converting %i ikchains...\n", numIkChains);

	const r5::v191::mstudioikchain_t* pOldChains = reinterpret_cast<const r5::v191::mstudioikchain_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->ikchainindex));

	int currentLinkCount = 0;

	// Write chain headers
	for (int i = 0; i < numIkChains; i++)
	{
		const r5::v191::mstudioikchain_t* oldChain = &pOldChains[i];
		r5::v8::mstudioikchain_t* newChain = reinterpret_cast<r5::v8::mstudioikchain_t*>(g_model.pData);

		AddToStringTable((char*)newChain, &newChain->sznameindex, oldChain->pszName());

		newChain->linktype = oldChain->linktype;
		newChain->numlinks = oldChain->numlinks;
		newChain->linkindex = static_cast<int>((sizeof(r5::v8::mstudioiklink_t) * currentLinkCount) + (sizeof(r5::v8::mstudioikchain_t) * (numIkChains - i)));
		newChain->unk = oldChain->unk_10;

		g_model.pData += sizeof(r5::v8::mstudioikchain_t);

		currentLinkCount += oldChain->numlinks;
	}

	// Write chain links
	for (int i = 0; i < numIkChains; i++)
	{
		const r5::v191::mstudioikchain_t* oldChain = &pOldChains[i];

		for (int linkIdx = 0; linkIdx < oldChain->numlinks; linkIdx++)
		{
			const r5::v191::mstudioiklink_t* oldLink = oldChain->pLink(linkIdx);
			r5::v8::mstudioiklink_t* newLink = reinterpret_cast<r5::v8::mstudioiklink_t*>(g_model.pData);

			newLink->bone = oldLink->bone;
			newLink->kneeDir = oldLink->kneeDir;

			g_model.pData += sizeof(r5::v8::mstudioiklink_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertPoseParams_191
// Purpose: converts v19.1 pose parameters to v10 format
//
static int ConvertPoseParams_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numPoseParams, bool isRig)
{
	int index = static_cast<int>(g_model.pData - g_model.pBase);

	if (numPoseParams == 0)
		return index;

	printf("converting %i pose parameters...\n", numPoseParams);

	const r5::v191::mstudioposeparamdesc_t* pOldParams = reinterpret_cast<const r5::v191::mstudioposeparamdesc_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->localposeparamindex));

	for (int i = 0; i < numPoseParams; i++)
	{
		const r5::v191::mstudioposeparamdesc_t* oldParam = &pOldParams[i];
		mstudioposeparamdesc_t* newParam = reinterpret_cast<mstudioposeparamdesc_t*>(g_model.pData);

		AddToStringTable((char*)newParam, &newParam->sznameindex, oldParam->pszName());
		newParam->flags = oldParam->flags;
		newParam->start = oldParam->start;
		newParam->end = oldParam->end;
		newParam->loop = oldParam->loop;

		g_model.pData += sizeof(mstudioposeparamdesc_t);
	}

	ALIGN4(g_model.pData);

	return index;
}

//
// ConvertSequences_191
// Purpose: converts v19.1 sequences to v10 format
// Note: v19.1 animation data is stored externally via animDataAsset GUIDs in pak files.
// We copy sequence metadata from v19.1 but create placeholder animation data.
//
static void ConvertSequences_191(const r5::v191::studiohdr_t* pOldHdr, const char* pOldData, int numSeqs)
{
	g_model.hdrV54()->localseqindex = static_cast<int>(g_model.pData - g_model.pBase);
	g_model.hdrV54()->numlocalseq = numSeqs;

	if (numSeqs == 0)
		return;

	printf("converting %i sequences from v19.1...\n", numSeqs);

	// Get v19.1 sequence array
	int v19SeqOffset = FIX_OFFSET(pOldHdr->localseqindex);
	const r5::v191::mstudioseqdesc_t* pOldSeqs =
		reinterpret_cast<const r5::v191::mstudioseqdesc_t*>((const char*)pOldHdr + v19SeqOffset);

	r5::v8::mstudioseqdesc_t* newSeqBase = reinterpret_cast<r5::v8::mstudioseqdesc_t*>(g_model.pData);

	// Write sequence descriptors
	for (int i = 0; i < numSeqs; i++)
	{
		const r5::v191::mstudioseqdesc_t* oldSeq = &pOldSeqs[i];
		r5::v8::mstudioseqdesc_t* newSeq = &newSeqBase[i];

		// Initialize with zeros
		memset(newSeq, 0, sizeof(r5::v8::mstudioseqdesc_t));

		// Copy label from v19.1
		const char* label = oldSeq->pszLabel();
		AddToStringTable((char*)newSeq, &newSeq->szlabelindex, label);
		printf("  seq %d: label='%s'\n", i, label);

		// Copy activity name if present (non-zero index)
		if (oldSeq->szactivitynameindex > 0)
		{
			const char* activityName = oldSeq->pszActivity();
			AddToStringTable((char*)newSeq, &newSeq->szactivitynameindex, activityName);
		}
		else
		{
			newSeq->szactivitynameindex = 0;
		}

		// Copy flags - preserve STUDIO_ANIM_UNK2 (0x80000) as it exists in v10/v54 format
		// The original v10 files have this flag set on "ref" sequences
		newSeq->flags = oldSeq->flags;

		// Copy activity (promote uint16_t to int, 65535 means no activity -> -1)
		newSeq->activity = (oldSeq->activity == 65535) ? -1 : static_cast<int>(oldSeq->activity);
		newSeq->actweight = static_cast<int>(oldSeq->actweight);

		// Events - v19.1 event data would need separate handling
		newSeq->numevents = 0;
		newSeq->eventindex = 0;

		// Copy bounding box from v19.1
		newSeq->bbmin = oldSeq->bbmin;
		newSeq->bbmax = oldSeq->bbmax;

		// Copy blend info
		newSeq->numblends = static_cast<int>(oldSeq->numblends);
		newSeq->groupsize[0] = oldSeq->groupsize[0];
		newSeq->groupsize[1] = oldSeq->groupsize[1];

		// Copy parameter indices and values (critical for blend animations!)
		newSeq->paramindex[0] = static_cast<int>(oldSeq->paramindex[0]);
		newSeq->paramindex[1] = static_cast<int>(oldSeq->paramindex[1]);
		newSeq->paramstart[0] = oldSeq->paramstart[0];
		newSeq->paramstart[1] = oldSeq->paramstart[1];
		newSeq->paramend[0] = oldSeq->paramend[0];
		newSeq->paramend[1] = oldSeq->paramend[1];
		newSeq->paramparent = 0; // Not in v19.1

		// Copy fade times
		newSeq->fadeintime = oldSeq->fadeintime;
		newSeq->fadeouttime = oldSeq->fadeouttime;

		// Copy node info
		newSeq->localentrynode = static_cast<int>(oldSeq->localentrynode);
		newSeq->localexitnode = static_cast<int>(oldSeq->localexitnode);
		newSeq->nodeflags = 0; // v19.1 doesn't have this field

		// Default phase values (not in v19.1)
		newSeq->entryphase = 0.0f;
		newSeq->exitphase = 0.0f;
		newSeq->lastframe = 0.0f;
		newSeq->nextseq = 0;
		newSeq->pose = 0;

		// Copy IK/autolayer counts
		newSeq->numikrules = static_cast<int>(oldSeq->numikrules);
		newSeq->numautolayers = static_cast<int>(oldSeq->numautolayers);
		newSeq->numiklocks = static_cast<int>(oldSeq->numiklocks);

		// Copy activity modifiers
		newSeq->numactivitymodifiers = static_cast<int>(oldSeq->numactivitymodifiers);
		newSeq->ikResetMask = oldSeq->ikResetMask;

		// cycleposeindex - promote to int
		newSeq->cycleposeindex = static_cast<int>(oldSeq->cycleposeindex);

		// These will be set later when writing the actual data
		newSeq->animindexindex = 0;
		newSeq->movementindex = 0;
		newSeq->autolayerindex = 0;
		newSeq->weightlistindex = 0;
		newSeq->posekeyindex = 0;
		newSeq->iklockindex = 0;
		newSeq->activitymodifierindex = 0;
		newSeq->keyvalueindex = 0;
		newSeq->keyvaluesize = 0;
	}

	g_model.pData += numSeqs * sizeof(r5::v8::mstudioseqdesc_t);

	// Write animation data for each sequence
	for (int i = 0; i < numSeqs; i++)
	{
		const r5::v191::mstudioseqdesc_t* oldSeq = &pOldSeqs[i];
		r5::v8::mstudioseqdesc_t* newSeq = &newSeqBase[i];

		// Get number of animations in blend grid
		int numAnims = oldSeq->groupsize[0] * oldSeq->groupsize[1];
		if (numAnims <= 0)
			numAnims = 1; // At least one animation

		// Write animation index array (offsets to animation descriptors)
		ALIGN4(g_model.pData);
		newSeq->animindexindex = static_cast<int>(g_model.pData - (char*)newSeq);
		int* newAnimIndices = reinterpret_cast<int*>(g_model.pData);
		g_model.pData += numAnims * sizeof(int);

		// Get v19.1 animation indices
		const uint16_t* v19AnimIndices = nullptr;
		if (oldSeq->animindexindex > 0)
		{
			v19AnimIndices = reinterpret_cast<const uint16_t*>((char*)oldSeq + FIX_OFFSET(oldSeq->animindexindex));
		}

		// Process each animation in the blend grid
		for (int animIdx = 0; animIdx < numAnims; animIdx++)
		{
			ALIGN4(g_model.pData);
			r5::v8::mstudioanimdesc_t* newAnim = reinterpret_cast<r5::v8::mstudioanimdesc_t*>(g_model.pData);
			newAnimIndices[animIdx] = static_cast<int>((char*)newAnim - (char*)newSeq);
			memset(newAnim, 0, sizeof(r5::v8::mstudioanimdesc_t));
			g_model.pData += sizeof(r5::v8::mstudioanimdesc_t);

			// Get v19.1 animation descriptor
			const r5::v191::mstudioanimdesc_t* oldAnimDesc = nullptr;
			if (v19AnimIndices && v19AnimIndices[animIdx] > 0)
			{
				oldAnimDesc = reinterpret_cast<const r5::v191::mstudioanimdesc_t*>(
					(char*)oldSeq + FIX_OFFSET(v19AnimIndices[animIdx]));
			}

			if (oldAnimDesc)
			{
				// Copy animation name
				if (oldAnimDesc->sznameindex > 0)
				{
					const char* animName = oldAnimDesc->pszName();
					AddToStringTable((char*)newAnim, &newAnim->sznameindex, animName);
				}
				else
				{
					// Use sequence label as fallback
					const char* seqLabel = oldSeq->pszLabel();
					AddToStringTable((char*)newAnim, &newAnim->sznameindex, seqLabel);
				}

				// Copy animation metadata from v19.1
				newAnim->fps = oldAnimDesc->fps;
				// Clear the v19.1-only 0x200000 "anim data is external" bit: the data
				// below is written INLINE, and v54 does not define that bit.
				newAnim->flags = oldAnimDesc->flags & ~kStudioAnimExternalFlag;
				newAnim->numframes = oldAnimDesc->numframes;
				newAnim->nummovements = 0; // Deprecated

				// Track animation GUID if present (v19.1 uses external assets)
				if (oldAnimDesc->animDataAsset != 0)
				{
					printf("    Animation %d ('%s') has external GUID asset: 0x%016llX\n",
						animIdx, 
						oldAnimDesc->sznameindex > 0 ? oldAnimDesc->pszName() : oldSeq->pszLabel(),
						oldAnimDesc->animDataAsset);
					printf("      WARNING: External animation data not loaded - placeholder conversion only\n");
					printf("      TODO: Implement rpak GUID resolution for full animation data\n");
					
					// Check if external section data is loaded at runtime
					if (oldAnimDesc->sectionDataExternal != nullptr)
					{
						printf("      External section data pointer: %p (runtime-loaded)\n", 
							oldAnimDesc->sectionDataExternal);
					}
				}

				// NOTE: Animation section conversion is PLACEHOLDER
				// TODO: Implement proper section handling for v19.1
				printf("    WARNING: Animation conversion is PLACEHOLDER - animation sections not fully converted\n");

				// Frame movement - v19.1 has framemovementindex but structure is incompatible, skip for now
				// TODO: Implement frame movement if needed

				// Copy IK rules if present
				if (oldAnimDesc->numikrules > 0 && oldAnimDesc->ikruleindex > 0)
				{
					ALIGN4(g_model.pData);
					newAnim->ikruleindex = static_cast<int>(g_model.pData - (char*)newAnim);
					newAnim->numikrules = oldAnimDesc->numikrules;

					const r5::v191::mstudioikrule_t* oldIKRules =
						reinterpret_cast<const r5::v191::mstudioikrule_t*>(
							(char*)oldAnimDesc + FIX_OFFSET(oldAnimDesc->ikruleindex));

					for (int ikIdx = 0; ikIdx < oldAnimDesc->numikrules; ikIdx++)
					{
						const r5::v191::mstudioikrule_t* oldIK = &oldIKRules[ikIdx];
						r5::v8::mstudioikrule_t* newIK = reinterpret_cast<r5::v8::mstudioikrule_t*>(g_model.pData);

						// v8 IK rule structure is different - map fields
						newIK->index = ikIdx;
						newIK->type = oldIK->type;
						newIK->chain = oldIK->chain;
						newIK->bone = oldIK->bone;
						newIK->slot = oldIK->slot;
						newIK->height = oldIK->height;
						newIK->radius = oldIK->radius;
						newIK->floor = oldIK->floor;
						newIK->pos = oldIK->pos;
						newIK->q = oldIK->q;

						// Copy compressed IK error from v19.1
						newIK->compressedIkError.sectionFrames = oldIK->compressedikerror.sectionframes;
						for (int s = 0; s < 6; s++)
							newIK->compressedIkError.scale[s] = oldIK->compressedikerror.scale[s];

						// Expand offsets from uint16_t to int
						newIK->compressedikerrorindex = static_cast<int>(oldIK->compressedikerrorindex);
						newIK->iStart = oldIK->iStart;
						newIK->ikerrorindex = static_cast<int>(oldIK->ikerrorindex);

						// Copy timing data
						newIK->start = oldIK->start;
						newIK->peak = oldIK->peak;
						newIK->tail = oldIK->tail;
						newIK->end = oldIK->end;
						newIK->contact = oldIK->contact;
						newIK->drop = oldIK->drop;
						newIK->top = oldIK->top;
						newIK->endHeight = oldIK->endHeight;

						// Copy attachment name if present
						if (oldIK->szattachmentindex > 0)
						{
							const char* attachName = (char*)oldIK + FIX_OFFSET(oldIK->szattachmentindex);
							AddToStringTable((char*)newIK, &newIK->szattachmentindex, attachName);
						}

						g_model.pData += sizeof(r5::v8::mstudioikrule_t);
					}
				}

				// v19.1 NOTE: Animation data is stored externally via animDataAsset GUID
				// v19.1 does NOT have embedded animindex like v16 - all animations are in RSEQ files
				// However, we still need to create placeholder animation data for v10 format
				
				// Create minimal placeholder animation data
				ALIGN4(g_model.pData);
				newAnim->animindex = static_cast<int>(g_model.pData - (char*)newAnim);

				// Write minimal bone flags array for STUDIO_ALLZEROS
				int numBones = g_model.hdrV54()->numbones;
				if (numBones > 0)
					g_model.pData += WriteAnimRefBlock(g_model.pData, numBones,
						AnimRefBone0Quat(g_model.hdrV54()));

				// Copy animation sections if present
				if (oldAnimDesc->sectionindex > 0)
				{
					ALIGN2(g_model.pData);
					newAnim->sectionindex = static_cast<int>(g_model.pData - (char*)newAnim);
					newAnim->sectionframes = oldAnimDesc->sectionframes;

					// Calculate number of sections
					int numSections = 1;
					if (oldAnimDesc->sectionframes > 0)
					{
						int stall = oldAnimDesc->sectionstallframes;
						numSections = ((oldAnimDesc->numframes - stall - 1) / oldAnimDesc->sectionframes) + 2;
					}

					const r5::v191::mstudioanimsections_t* oldSections =
						reinterpret_cast<const r5::v191::mstudioanimsections_t*>(
							(char*)oldAnimDesc + FIX_OFFSET(oldAnimDesc->sectionindex));

					for (int s = 0; s < numSections; s++)
					{
						r5::v8::mstudioanimsections_t* newSection =
							reinterpret_cast<r5::v8::mstudioanimsections_t*>(g_model.pData);
						newSection->animindex = oldSections[s].animindex;
						g_model.pData += sizeof(r5::v8::mstudioanimsections_t);
					}
				}
			}
			else
			{
				// No animation descriptor - create minimal placeholder
				const char* seqLabel = oldSeq->pszLabel();
				AddToStringTable((char*)newAnim, &newAnim->sznameindex, seqLabel);

				newAnim->fps = 30.0f;
				newAnim->flags = STUDIO_ALLZEROS;
				newAnim->numframes = 1;
				newAnim->nummovements = 0;

				ALIGN4(g_model.pData);
				newAnim->animindex = static_cast<int>(g_model.pData - (char*)newAnim);

				int numBones = g_model.hdrV54()->numbones;
				if (numBones > 0)
					g_model.pData += WriteAnimRefBlock(g_model.pData, numBones,
						AnimRefBone0Quat(g_model.hdrV54()));
			}

			ALIGN2(g_model.pData);
		}

		// Copy autolayers if present
		if (oldSeq->numautolayers > 0 && oldSeq->autolayerindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->autolayerindex = static_cast<int>(g_model.pData - (char*)newSeq);

			const r5::v191::mstudioautolayer_t* oldAutoLayers =
				reinterpret_cast<const r5::v191::mstudioautolayer_t*>(
					(char*)oldSeq + FIX_OFFSET(oldSeq->autolayerindex));

			for (int layerIdx = 0; layerIdx < oldSeq->numautolayers; layerIdx++)
			{
				const r5::v191::mstudioautolayer_t* oldLayer = &oldAutoLayers[layerIdx];
				r5::v8::mstudioautolayer_t* newLayer = reinterpret_cast<r5::v8::mstudioautolayer_t*>(g_model.pData);

				// v19.1 autolayer is 32 bytes, v8 is 24 bytes
				// Skip the 8-byte assetSequence GUID at the start
				newLayer->iSequence = oldLayer->iSequence;
				newLayer->iPose = oldLayer->iPose;
				newLayer->flags = oldLayer->flags;
				newLayer->start = oldLayer->start;
				newLayer->peak = oldLayer->peak;
				newLayer->tail = oldLayer->tail;
				newLayer->end = oldLayer->end;

				g_model.pData += sizeof(r5::v8::mstudioautolayer_t);
			}
		}

		// Write event data if present
		// v19.1 mstudioevent_t: 20 bytes (float cycle, int event, int type, int unk_C, uint16_t optionsindex, uint16_t szeventindex)
		// v10 mstudioevent_t: 80 bytes (float cycle, int event, int type, char options[64], int szeventindex)
		if (oldSeq->numevents > 0 && oldSeq->eventindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->eventindex = static_cast<int>(g_model.pData - (char*)newSeq);
			newSeq->numevents = oldSeq->numevents;

			const char* pOldEvents = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->eventindex);

			printf("    Converting %d events from v19.1 to v10...\n", oldSeq->numevents);

			for (int e = 0; e < oldSeq->numevents; e++)
			{
				// Read v19.1 event (20 bytes)
				const r5::v191::mstudioevent_t* pOldEvent = reinterpret_cast<const r5::v191::mstudioevent_t*>(pOldEvents + e * sizeof(r5::v191::mstudioevent_t));

				// Write v10 event (80 bytes)
				r5::v8::mstudioevent_t* pNewEvent = reinterpret_cast<r5::v8::mstudioevent_t*>(g_model.pData);
				memset(pNewEvent, 0, sizeof(r5::v8::mstudioevent_t));

				pNewEvent->cycle = pOldEvent->cycle;
				pNewEvent->event = pOldEvent->event;
				pNewEvent->type = pOldEvent->type;

				// Copy options string from v19.1 string table to v10 inline array
				if (pOldEvent->optionsindex > 0)
				{
					const char* optionsStr = reinterpret_cast<const char*>(pOldEvent) + FIX_OFFSET(pOldEvent->optionsindex);
					strncpy_s(pNewEvent->options, sizeof(pNewEvent->options), optionsStr, _TRUNCATE);
				}

				// Copy event name string reference
				if (pOldEvent->szeventindex > 0)
				{
					const char* eventName = reinterpret_cast<const char*>(pOldEvent) + FIX_OFFSET(pOldEvent->szeventindex);
					AddToStringTable(reinterpret_cast<char*>(pNewEvent), &pNewEvent->szeventindex, eventName);
				}

				g_model.pData += sizeof(r5::v8::mstudioevent_t);
			}
		}

		// Per-sequence bone weightlist. A v19.1 local seq is a degenerate placeholder
		// (its real animation is an external aseq asset), so its weightlistindex does
		// NOT address boneCount floats -- on every animated prop it reads 3, which
		// FIX_OFFSET turns into 32, i.e. the seqdesc's own bbmax. Copying from there
		// wrote garbage weights (1e-38 .. 4e+31) into every v19.1-sourced dedi model.
		// Synthesize full weight instead, matching what ConvertRMDL191To17 emits.
		{
			ALIGN4(g_model.pData);
			newSeq->weightlistindex = static_cast<int>(g_model.pData - (char*)newSeq);

			const int numBones = g_model.hdrV54()->numbones;
			float* const pWeights = reinterpret_cast<float*>(g_model.pData);
			for (int w = 0; w < numBones; ++w)
				pWeights[w] = 1.0f;

			g_model.pData += numBones * sizeof(float);
		}

		// Copy posekey data if present
		// Posekey is an array of floats for pose parameter blending
		// Size = (groupsize[0] + groupsize[1]) * sizeof(float) - uses ADDITION not multiplication!
		if (oldSeq->posekeyindex > 0)
		{
			ALIGN4(g_model.pData);
			const int numPoseKeys = oldSeq->groupsize[0] + oldSeq->groupsize[1];
			const size_t copyCount = numPoseKeys * sizeof(float);
			const char* pOldPosekey = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->posekeyindex);

			memcpy(g_model.pData, pOldPosekey, copyCount);
			newSeq->posekeyindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(newSeq));
			g_model.pData += copyCount;
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertLinearBoneTable_191
// Purpose: copies the linear bone table from v19.1 to v10 format
//
static void ConvertLinearBoneTable_191(const r5::v191::studiohdr_t* pOldHdr)
{
	if (pOldHdr->linearboneindex == 0 || pOldHdr->boneCount <= 1)
		return;

	const r5::v191::mstudiolinearbone_t* pOldLinear = r5::v191::GetLinearBone(pOldHdr);

	g_model.hdrV54()->linearboneindex = static_cast<int>(g_model.pData - g_model.pBase);

	// Write v10 linear bone structure
	r5::v8::mstudiolinearbone_t* pNewLinear = reinterpret_cast<r5::v8::mstudiolinearbone_t*>(g_model.pData);

	const int numBones = pOldHdr->boneCount;
	pNewLinear->numbones = numBones;

	char* pDataStart = g_model.pData;
	g_model.pData += sizeof(r5::v8::mstudiolinearbone_t);

	// Flags
	ALIGN4(g_model.pData);
	pNewLinear->flagsindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<int*>(g_model.pData) = pOldLinear->flags(i);
		g_model.pData += sizeof(int);
	}

	// Parent
	ALIGN4(g_model.pData);
	pNewLinear->parentindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<int*>(g_model.pData) = *pOldLinear->pParent(i);
		g_model.pData += sizeof(int);
	}

	// Pos
	ALIGN4(g_model.pData);
	pNewLinear->posindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<Vector*>(g_model.pData) = *pOldLinear->pPos(i);
		g_model.pData += sizeof(Vector);
	}

	// Quat - with special handling for "delta" bones
	// Delta bones (like jx_c_delta) require a special quaternion (0.5, 0.5, 0.5, 0.5)
	// which represents a 120-degree rotation around the (1,1,1) axis.
	// This value is expected by the v10 runtime but is not present in v19.1 linear bone data.
	ALIGN4(g_model.pData);
	pNewLinear->quatindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		// Check if this is a "delta" bone that needs special quaternion
		const r5::v191::mstudiobonehdr_t* boneHdr = r5::v191::GetBoneHdr(pOldHdr, i);
		const char* boneName = boneHdr->pszName();

		// Apply special quaternion for root delta bone (bone 0 with "delta" in name)
		if (i == 0 && strstr(boneName, "delta") != nullptr)
		{
			// Special quaternion for delta bones: 120-degree rotation around (1,1,1)
			Quaternion deltaQuat(0.5f, 0.5f, 0.5f, 0.5f);
			*reinterpret_cast<Quaternion*>(g_model.pData) = deltaQuat;
			printf("  Applied delta bone quaternion fix for bone 0 (%s)\n", boneName);
		}
		else
		{
			// Normal case: copy from v19.1 linear bone data
			*reinterpret_cast<Quaternion*>(g_model.pData) = *pOldLinear->pQuat(i);
		}
		g_model.pData += sizeof(Quaternion);
	}

	// Rot
	ALIGN4(g_model.pData);
	pNewLinear->rotindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<RadianEuler*>(g_model.pData) = *pOldLinear->pRot(i);
		g_model.pData += sizeof(RadianEuler);
	}

	// PoseToBone
	ALIGN4(g_model.pData);
	pNewLinear->posetoboneindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<matrix3x4_t*>(g_model.pData) = *pOldLinear->pPoseToBone(i);
		g_model.pData += sizeof(matrix3x4_t);
	}

	// Note: r5::v8::mstudiolinearbone_t does not have qalignmentindex
	// qAlignment data is stored per-bone in the mstudiobone_t structure instead

	ALIGN4(g_model.pData);
}

//
// ConvertUIPanelMeshes_191
// Purpose: converts v19.1 UI panel (RUI) mesh data to v10 format
// Note: v19.1 uses uint16_t for uiPanelCount/uiPanelOffset vs v10's int
//
static void ConvertUIPanelMeshes_191(const r5::v191::studiohdr_t* const oldHeader)
{
	if (oldHeader->uiPanelCount == 0)
		return;

	printf("Converting %d UI panel meshes...\n", oldHeader->uiPanelCount);

	// Set the output header count
	g_model.hdrV54()->uiPanelCount = oldHeader->uiPanelCount;

	// Calculate source data pointer - v19.1 uses direct byte offsets (FIX_OFFSET is identity)
	const char* pOldBase = reinterpret_cast<const char*>(oldHeader);
	const char* pOldUIPanelData = pOldBase + oldHeader->uiPanelOffset;

	// Copy all RUI headers
	const size_t totalHeaderBufSize = oldHeader->uiPanelCount * sizeof(r5::v8::mstudiorruiheader_t);
	memcpy(g_model.pData, pOldUIPanelData, totalHeaderBufSize);

	// Set the output offset
	g_model.hdrV54()->uiPanelOffset = static_cast<int>(g_model.pData - g_model.pBase);

	r5::v8::mstudiorruiheader_t* const ruiHeaders = reinterpret_cast<r5::v8::mstudiorruiheader_t*>(g_model.pData);
	g_model.pData += totalHeaderBufSize;

	// The RUI mesh data itself must be aligned to 16 bytes
	ALIGN16(g_model.pData);

	for (int i = 0; i < oldHeader->uiPanelCount; i++)
	{
		r5::v8::mstudiorruiheader_t* ruiHeader = &ruiHeaders[i];

		// Calculate source mesh offset - relative to this header's position in the old data
		const size_t oldHeaderOffset = oldHeader->uiPanelOffset + (i * sizeof(r5::v8::mstudiorruiheader_t));
		const char* pOldMesh = pOldBase + oldHeaderOffset + ruiHeader->ruimeshindex;

		// Copy the mesh header
		const r5::v8::mstudioruimesh_t* pOldMeshHdr = reinterpret_cast<const r5::v8::mstudioruimesh_t*>(pOldMesh);
		memcpy(g_model.pData, pOldMesh, sizeof(r5::v8::mstudioruimesh_t));

		// Update the mesh index as it can be different due to header alignment
		ruiHeader->ruimeshindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(ruiHeader));

		const r5::v8::mstudioruimesh_t* const header = reinterpret_cast<r5::v8::mstudioruimesh_t*>(g_model.pData);
		g_model.pData += sizeof(r5::v8::mstudioruimesh_t);

		// Read UI mesh name string and padding - this is within the space between
		// our cursor and the parent index
		const char* pOldMeshName = pOldMesh + sizeof(r5::v8::mstudioruimesh_t);
		memcpy(g_model.pData, pOldMeshName, header->parentindex);
		g_model.pData += header->parentindex;

		// Parents
		const size_t parentBytes = header->numparents * sizeof(short);
		const char* pOldParents = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->parentindex;
		memcpy(g_model.pData, pOldParents, parentBytes);
		g_model.pData += parentBytes;

		// Vertex maps
		const size_t vertMapBytes = header->numfaces * sizeof(r5::v8::mstudioruivertmap_t);
		const char* pOldVertMaps = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->vertmapindex;
		memcpy(g_model.pData, pOldVertMaps, vertMapBytes);
		g_model.pData += vertMapBytes;

		// Fourth vertices (unk data)
		const size_t fourthVertBytes = header->numfaces * sizeof(r5::v8::mstudioruifourthvert_t);
		const char* pOldFourthVerts = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->unkindex;
		memcpy(g_model.pData, pOldFourthVerts, fourthVertBytes);
		g_model.pData += fourthVertBytes;

		// Vertices
		const size_t vertBytes = header->numvertices * sizeof(r5::v8::mstudioruivert_t);
		const char* pOldVerts = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->vertexindex;
		memcpy(g_model.pData, pOldVerts, vertBytes);
		g_model.pData += vertBytes;

		// Faces (bounds/UV data)
		const size_t faceBytes = header->numfaces * sizeof(r5::v8::mstudioruimeshface_t);
		const char* pOldFaces = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->facedataindex;
		memcpy(g_model.pData, pOldFaces, faceBytes);
		g_model.pData += faceBytes;

		printf("  UI Panel %d: %d parents, %d verts, %d faces\n",
			i, header->numparents, header->numvertices, header->numfaces);
	}

	ALIGN4(g_model.pData);
	printf("UI panel conversion complete.\n");
}

//
// ConvertCollisionData_V191
// Purpose: converts v19.1 collision data (40-byte headers) to v10 format (32-byte headers)
// Based on working v16 collision conversion
//
static void ConvertCollisionData_V191(const r5::v191::studiohdr_t* const oldStudioHdr, const char* const pOldBVHData, const size_t fileSize, const std::string& modelNameForLog)
{
	printf("Converting V19.1 collision to V10 format...\n");

	g_model.hdrV54()->bvhOffset = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v8::mstudiocollmodel_t* const pOldCollModel = reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(pOldBVHData);
	r5::v8::mstudiocollmodel_t* const pNewCollModel = reinterpret_cast<r5::v8::mstudiocollmodel_t*>(g_model.pData);

	const int headerCount = pOldCollModel->headerCount;
	pNewCollModel->headerCount = headerCount;

	printf("  V19.1 collision: %d headers\n", headerCount);

	g_model.pData += sizeof(r5::v8::mstudiocollmodel_t);

	// V19.1 uses 40-byte headers (same as v16/v120), V10 uses 32-byte headers
	const r5::v191::mstudiocollheader_t* const pOldCollHeaders = reinterpret_cast<const r5::v191::mstudiocollheader_t*>(pOldBVHData + sizeof(r5::v8::mstudiocollmodel_t));
	r5::v8::mstudiocollheader_t* const pNewCollHeaders = reinterpret_cast<r5::v8::mstudiocollheader_t*>(g_model.pData);

	g_model.pData += headerCount * sizeof(r5::v8::mstudiocollheader_t);

	// MODERN -> LEGACY surface-property transform (see PlanCollisionSurfacePropRemap's
	// unk4_v54 resolve pass in rmdl_160.cpp (ConvertRMDL160To10) for
	// the full reference -- v19.1's mstudiocollheader_t is byte-identical to v160's,
	// just with directly-named fields instead of v120-aliased ones). A part is MODERN
	// iff meshGroupCount>0 && skinCount>0; the model is modern iff ANY part is. Build
	// the per-part planner inputs from the SOURCE (pre-conversion) header/leaf data now,
	// before any table writes, so the plan is ready when we reach the buffer-copy step.
	const r5::v8::dsurfaceproperty_t* const pOldSurfRowsForPlan =
		reinterpret_cast<const r5::v8::dsurfaceproperty_t*>(pOldBVHData + pOldCollModel->surfacePropsIndex);
	const int srcRowCountForPlan =
		(pOldCollModel->contentMasksIndex - pOldCollModel->surfacePropsIndex) / sizeof(r5::v8::dsurfaceproperty_t);

	std::vector<CollSurfPartInput_t> planParts(headerCount);
	for (int i = 0; i < headerCount; ++i)
	{
		const r5::v191::mstudiocollheader_t& h = pOldCollHeaders[i];
		CollSurfPartInput_t& p = planParts[i];
		p.nodeBase = pOldBVHData + h.nodesOfs;
		p.leafBase = pOldBVHData + h.leafDataOfs;
		p.vertBase = pOldBVHData + h.vertsOfs;
		p.skinInfosBase = pOldBVHData + h.skinInfosOfs;
		p.packedVerts = (h.bvhFlags & 1) != 0;
		p.origin[0] = h.origin[0]; p.origin[1] = h.origin[1]; p.origin[2] = h.origin[2];
		p.scaleTrue = h.decodeScale * 65536.0f;
		p.skinCount = h.skinCount;
		p.meshGroupCount = h.meshGroupCount;

		// Same leaf-block bound the existing per-header copy loop below computes.
		__int64 leafSizeBytes;
		if (i != headerCount - 1)
			leafSizeBytes = pOldCollHeaders[i + 1].vertsOfs - h.leafDataOfs;
		else
			leafSizeBytes = pOldCollHeaders[0].nodesOfs - h.leafDataOfs;
		p.leafDataDwords = static_cast<int>(leafSizeBytes / 4);
	}

	const CollSurfPropPlan_t surfPlan = PlanCollisionSurfacePropRemap(
		planParts, pOldSurfRowsForPlan, srcRowCountForPlan, modelNameForLog.c_str());

	// Copy collision buffers: surface props, content masks, and surface names
	if (!surfPlan.anyModern)
	{
		// No modern part in this model -- keep the CURRENT verbatim behavior
		// bit-for-bit (zero behavior change for legacy models, regression safety).
		const char* const oldBase = (char*)pOldCollModel;
		const char* const newBase = (char*)pNewCollModel;

		const int surfacePropsSize = pOldCollModel->contentMasksIndex - pOldCollModel->surfacePropsIndex;
		const int contentMasksSize = pOldCollModel->surfaceNamesIndex - pOldCollModel->contentMasksIndex;

		// Calculate surface names size - ends at first header's vertex data
		// V19.1 doesn't have surfacePropDataIndex, so we use vertsOfs from first header
		const int surfaceNamesSize = pOldCollHeaders[0].vertsOfs - pOldCollModel->surfaceNamesIndex;

		pNewCollModel->surfacePropsIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->surfacePropsIndex, surfacePropsSize);
		g_model.pData += surfacePropsSize;

		pNewCollModel->contentMasksIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->contentMasksIndex, contentMasksSize);
		g_model.pData += contentMasksSize;

		pNewCollModel->surfaceNamesIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->surfaceNamesIndex, surfaceNamesSize);
		g_model.pData += surfaceNamesSize;

		// V19.1 surface props are already in the correct format (no extra indirection
		// layer like v16/v120) -- the memcpy above already copies them verbatim, which
		// is exactly what's needed for a LEGACY-only model. Do not overwrite them.
		const int surfacePropCount = (pOldCollModel->contentMasksIndex - pOldCollModel->surfacePropsIndex) / sizeof(r5::v8::dsurfaceproperty_t);
		printf("  V19.1 surface properties: %d entries copied verbatim\n", surfacePropCount);
	}
	else
	{
		// MODERN model: re-emit the planned dedup'd LEGACY row table (nameOffset=0
		// for every row -- matches native S3 Repak-compiled output), content masks
		// copied verbatim (indices must stay stable -- rows reference them by index,
		// not by value), and a 4-zero-byte surfaceNames block (the S3 engine reads the
		// id byte, not the name string).
		const char* const oldBase = (char*)pOldCollModel;
		const char* const newBase = (char*)pNewCollModel;

		const int contentMasksSize = pOldCollModel->surfaceNamesIndex - pOldCollModel->contentMasksIndex;
		const int outRowCount = static_cast<int>(surfPlan.outRows.size());

		pNewCollModel->surfacePropsIndex = static_cast<int>(g_model.pData - newBase);
		for (int r = 0; r < outRowCount; ++r)
		{
			r5::v8::dsurfaceproperty_t row{};
			row.unk = static_cast<short>(surfPlan.outRows[r].surfFlags);
			row.surfacePropId = surfPlan.outRows[r].finalId;
			row.contentMaskOffset = surfPlan.outRows[r].contentsIdx;
			row.surfaceNameOffset = 0;
			memcpy(g_model.pData, &row, sizeof(row));
			g_model.pData += sizeof(row);
		}

		pNewCollModel->contentMasksIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->contentMasksIndex, contentMasksSize);
		g_model.pData += contentMasksSize;

		pNewCollModel->surfaceNamesIndex = static_cast<int>(g_model.pData - newBase);
		static const char kZeroSurfaceNames[4] = { 0, 0, 0, 0 };
		memcpy(g_model.pData, kZeroSurfaceNames, sizeof(kZeroSurfaceNames));
		g_model.pData += sizeof(kZeroSurfaceNames);

		int modernPartCount = 0;
		for (const CollSurfPartInput_t& p : planParts)
		{
			if (p.meshGroupCount > 0 && p.skinCount > 0)
				++modernPartCount;
		}

		std::string idList;
		for (int r = 0; r < outRowCount; ++r)
		{
			if (r) idList += ",";
			idList += std::to_string(surfPlan.outRows[r].finalId);
		}
		printf("  [COLL-SURF] modern->legacy: %d parts (%d modern), %zu leaves remapped, %d src rows -> %d out rows, ids: %s\n",
			headerCount, modernPartCount, surfPlan.remaps.size(), srcRowCountForPlan, outRowCount, idList.c_str());
	}

	// Convert each collision header and copy its data
	for (int i = 0; i < headerCount; ++i)
	{
		const r5::v191::mstudiocollheader_t* const oldHeader = &pOldCollHeaders[i];
		r5::v8::mstudiocollheader_t* const newHeader = &pNewCollHeaders[i];

		// Map V19.1 40-byte header to V10 32-byte header
		newHeader->unk = oldHeader->bvhFlags;
		memcpy_s(newHeader->origin, sizeof(newHeader->origin), oldHeader->origin, sizeof(oldHeader->origin));
		newHeader->scale = oldHeader->decodeScale;

		// Copy vertex data
		const __int64 vertSize = oldHeader->leafDataOfs - oldHeader->vertsOfs;
		const void* const vertData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->vertsOfs;

		ALIGN64(g_model.pData);
		newHeader->vertIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, vertSize, vertData, vertSize);
		g_model.pData += vertSize;

		// Copy leaf data
		__int64 leafSize;
		if (i != headerCount - 1)
			leafSize = pOldCollHeaders[i + 1].vertsOfs - oldHeader->leafDataOfs;
		else
			leafSize = pOldCollHeaders[0].nodesOfs - oldHeader->leafDataOfs;

		const void* const leafData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->leafDataOfs;

		ALIGN64(g_model.pData);
		newHeader->bvhLeafIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, leafSize, leafData, leafSize);

		// MODERN model: rewrite this part's just-copied leaf block's header dwords
		// (low-12 bits -> new dedup'd row index) in the DESTINATION. Leaf dword index
		// i (relative to source leafDataOfs) lands at newHeader->bvhLeafIndex + i*4
		// relative to the new blob base -- exactly where we just memcpy'd it to.
		if (surfPlan.anyModern)
		{
			ApplyCollisionSurfacePropRemap(surfPlan, i, g_model.pData,
				static_cast<int>(leafSize / 4), modelNameForLog.c_str());
		}

		g_model.pData += leafSize;
	}

	// Second pass: copy node data for each header
	// Nodes are stored contiguously after all vertices and leaves
	for (int i = 0; i < headerCount; ++i)
	{
		const r5::v191::mstudiocollheader_t* const oldHeader = &pOldCollHeaders[i];
		r5::v8::mstudiocollheader_t* const newHeader = &pNewCollHeaders[i];

		__int64 nodeSize;
		if (i != headerCount - 1)
		{
			nodeSize = pOldCollHeaders[i + 1].nodesOfs - oldHeader->nodesOfs;
		}
		else
		{
			// For last header, calculate size using collision data boundary
			size_t collisionOffset = reinterpret_cast<const char*>(pOldBVHData) - reinterpret_cast<const char*>(oldStudioHdr);
			size_t maxNodeEnd = fileSize - collisionOffset - oldHeader->nodesOfs;

			nodeSize = maxNodeEnd;

			// Clamp to reasonable size if needed
			if (nodeSize > 1024 * 1024)
				nodeSize = 1024 * 1024;
		}

		const void* nodeData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->nodesOfs;
		ALIGN64(g_model.pData);
		newHeader->bvhNodeIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, nodeSize, nodeData, nodeSize);
		g_model.pData += nodeSize;
	}

	size_t totalCollSize = g_model.pData - reinterpret_cast<char*>(pNewCollModel);
	printf("  Collision converted: V19.1 -> V10, %zu bytes written at offset 0x%X\n",
		totalCollSize, g_model.hdrV54()->bvhOffset);
}

//
// ConvertRMDL191To10
// Purpose: converts mdl data from rmdl v19.1 (Season 19+) to rmdl v10 (Season 2/3)
//
void ConvertRMDL191To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut)
{
	std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();

	printf("Converting model '%s' from version 54 (subversion 19.1) to version 54 (subversion 10)...\n", rawModelName.c_str());
	printf("Input file size: %zu bytes\n", fileSize);

	TIME_SCOPE(__FUNCTION__);

	const r5::v191::studiohdr_t* oldHeader = reinterpret_cast<const r5::v191::studiohdr_t*>(pMDL);

	// Debug: Print first few bytes to verify format
	printf("First 16 bytes: ");
	for (int i = 0; i < 16 && i < (int)fileSize; i++)
		printf("%02X ", (unsigned char)pMDL[i]);
	printf("\n");

	// Debug: Print header info
	printf("Header info:\n");
	printf("  flags: 0x%08X\n", oldHeader->flags);
	printf("  checksum: 0x%08X\n", oldHeader->checksum);
	printf("  boneCount: %d\n", oldHeader->boneCount);
	printf("  numhitboxsets: %d\n", oldHeader->numhitboxsets);
	printf("  numlocalseq: %d\n", oldHeader->numlocalseq);
	printf("  numbodyparts: %d\n", oldHeader->numbodyparts);
	printf("  numtextures: %d\n", oldHeader->numtextures);

	std::filesystem::path inputPath(pathIn);
	std::filesystem::path outputDir;
	std::string baseOutputPath;
	std::string rmdlPath;

	// If pathOut is different from pathIn, use it directly (batch mode)
	// Otherwise, create rmdlconv_out subfolder (legacy single-file mode)
	if (pathOut != pathIn && !pathOut.empty())
	{
		rmdlPath = pathOut;
		outputDir = std::filesystem::path(pathOut).parent_path();
		// Remove .rmdl extension for baseOutputPath
		baseOutputPath = rmdlPath.substr(0, rmdlPath.length() - 5);
		std::filesystem::create_directories(outputDir);
	}
	else
	{
		outputDir = inputPath.parent_path() / "rmdlconv_out";
		std::filesystem::create_directories(outputDir);
		baseOutputPath = (outputDir / inputPath.stem().string()).string();
		rmdlPath = baseOutputPath + ".rmdl";
	}

	printf("Output: %s\n", rmdlPath.c_str());
	std::ofstream out(rmdlPath, std::ios::out | std::ios::binary);

	// Allocate temp file buffer
	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	// Convert mdl header
	r5::v8::studiohdr_t* pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	ConvertStudioHdr_191(pHdr, oldHeader, pMDL);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// Init string table
	BeginStringTable();

	// v19.1 stores a truncated name in the inline name[33] field (max 32 chars + null)
	// Use the input filename to get the full model name since inline name is often truncated
	std::string inlineName = oldHeader->name;
	std::string originalModelName = rawModelName;

	// Remove .rmdl extension if present
	if (originalModelName.length() > 5 && originalModelName.substr(originalModelName.length() - 5) == ".rmdl")
		originalModelName = originalModelName.substr(0, originalModelName.length() - 5);

	std::string modelName = originalModelName;
	if (modelName.rfind("mdl/", 0) != 0)
		modelName = "mdl/" + modelName;
	if (EndsWith(modelName, ".mdl"))
	{
		modelName = modelName.substr(0, modelName.length() - 4);
		modelName += ".rmdl";
	}

	memcpy_s(&pHdr->name, 64, modelName.c_str(), min(modelName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, modelName.c_str());

	// Surface prop
	const char* surfaceProp = (const char*)oldHeader + FIX_OFFSET(oldHeader->surfacepropindex);
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, surfaceProp);
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// Convert bones
	ConvertBones_191(oldHeader, pMDL, oldHeader->boneCount, false);

	// Convert attachments
	g_model.hdrV54()->localattachmentindex = ConvertAttachments_191(oldHeader, pMDL, oldHeader->numlocalattachments);

	// Convert hitboxsets and hitboxes
	ConvertHitboxes_191(oldHeader, pMDL, oldHeader->numhitboxsets);

	// Copy bonebyname table
	if (oldHeader->bonetablebynameindex > 0)
	{
		const char* pOldBoneTable = (const char*)oldHeader + FIX_OFFSET(oldHeader->bonetablebynameindex);
		memcpy(g_model.pData, pOldBoneTable, oldHeader->boneCount);
		g_model.hdrV54()->bonetablebynameindex = static_cast<int>(g_model.pData - g_model.pBase);
		g_model.pData += oldHeader->boneCount;
		ALIGN4(g_model.pData);
	}

	// Convert bone followers (animated-prop collision). Same fix as ConvertRMDL160To10: the v19.1
	// source stores u16 count/offset + a u16[] of bone indices; the v8 dedi header stores int
	// count/offset + int[]. ConvertBones_191 preserves bone order 1:1, so the indices stay valid.
	// Without this, animated props (loot bins, doors) get boneFollowerCount=0 -> no server collision.
	if (oldHeader->boneFollowerCount > 0 &&
		oldHeader->boneFollowerOffset > 0 && oldHeader->boneFollowerOffset < fileSize)
	{
		const uint16_t* pOldFollowers = reinterpret_cast<const uint16_t*>(
			(const char*)oldHeader + FIX_OFFSET(oldHeader->boneFollowerOffset));
		ALIGN4(g_model.pData);
		g_model.hdrV54()->boneFollowerOffset = static_cast<int>(g_model.pData - g_model.pBase);
		g_model.hdrV54()->boneFollowerCount = oldHeader->boneFollowerCount;
		for (int i = 0; i < oldHeader->boneFollowerCount; ++i)
		{
			*reinterpret_cast<int*>(g_model.pData) = static_cast<int>(pOldFollowers[i]);
			g_model.pData += sizeof(int);
		}
		ALIGN4(g_model.pData);
		printf("  Bone followers: copied %d (v19.1 u16 -> v8 int) for animated-prop collision\n",
			oldHeader->boneFollowerCount);
	}

	// Convert sequences and animations
	ConvertSequences_191(oldHeader, pMDL, oldHeader->numlocalseq);

	// Convert bodyparts, models, and meshes
	ConvertBodyParts_191(oldHeader, pMDL, oldHeader->numbodyparts);

	// Convert pose parameters
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams_191(oldHeader, pMDL, oldHeader->numlocalposeparameters, false);

	// Convert IK chains
	ConvertIkChains_191(oldHeader, pMDL, oldHeader->numikchains, false);

	// Convert textures
	ConvertTextures_191(oldHeader, pMDL, oldHeader->numtextures);

	// Convert skins
	ConvertSkins_191(oldHeader, pMDL, oldHeader->numskinref, oldHeader->numskinfamilies);

	// Convert UI panel meshes (RUI)
	ConvertUIPanelMeshes_191(oldHeader);

	// Write keyvalues
	std::string keyValues = "mdlkeyvalue{prop_data{base \"\"}}\n";
	strcpy_s(g_model.pData, keyValues.length() + 1, keyValues.c_str());
	pHdr->keyvalueindex = static_cast<int>(g_model.pData - g_model.pBase);
	pHdr->keyvaluesize = IALIGN2(static_cast<int>(keyValues.length() + 1));
	g_model.pData += keyValues.length() + 1;
	ALIGN4(g_model.pData);

	// Convert linear bone table
	ConvertLinearBoneTable_191(oldHeader);

	// Write string table
	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN64(g_model.pData);

	// Collision conversion for v19.1
	// NOTE: v19.1 bvhOffset is an ABSOLUTE offset from header start (same as v16)
	if (oldHeader->bvhOffset > 0)
	{
		// v19.1 uses absolute offset for collision data
		const char* pOldCollision = reinterpret_cast<const char*>(oldHeader) +
			FIX_OFFSET(oldHeader->bvhOffset);

		printf("Converting V19.1 collision data...\n");
		printf("  bvhOffset: 0x%04X (absolute)\n", oldHeader->bvhOffset);

		// Validate collision data
		const r5::v8::mstudiocollmodel_t* pCollModel = reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(pOldCollision);
		printf("  headerCount: %d\n", pCollModel->headerCount);

		if (pCollModel->headerCount > 0 && pCollModel->headerCount < 100)
		{
			// Valid collision data - convert it
			ConvertCollisionData_V191(oldHeader, pOldCollision, fileSize, rawModelName);
		}
		else
		{
			printf("  WARNING: Invalid collision headerCount (%d), skipping collision\n", pCollModel->headerCount);
			pHdr->bvhOffset = 0;
		}
	}
	else
	{
		pHdr->bvhOffset = 0;
	}

	// Preserve unk4_v54 verbatim, including negative sentinels. Positive OOB clamps to -1.
	constexpr uint32_t kFlagsStaticPropSel = 0x10;
	if ((pHdr->flags & kFlagsStaticPropSel) && pHdr->bvhOffset != 0)
	{
		const r5::v8::mstudiocollmodel_t* const pFinalCollModel =
			reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(g_model.pBase + pHdr->bvhOffset);
		const int finalHeaderCount = pFinalCollModel->headerCount;

		for (int slot = 0; slot < 2; slot++)
		{
			const int src = static_cast<int>(static_cast<int8_t>(oldHeader->bvhUnk[slot]));
			short resolved = static_cast<short>(src);
			if (src >= finalHeaderCount)
			{
				printf("  unk4_v54: WARNING: source bvhUnk[%d]=%d is OOB for headerCount=%d -- clamping to -1 (class-invisible)\n",
					slot, src, finalHeaderCount);
				resolved = -1;
			}
			pHdr->unk4_v54[slot] = resolved;
		}
		printf("  unk4_v54: (%d,%d) [movement,bullet] preserved from source (headerCount=%d)\n",
			static_cast<int>(pHdr->unk4_v54[0]), static_cast<int>(pHdr->unk4_v54[1]), finalHeaderCount);
	}

	pHdr->length = static_cast<int>(g_model.pData - g_model.pBase);

	out.write(g_model.pBase, pHdr->length);
	out.close();   // CRITICAL: close BEFORE the phy block patches phySize via a second fstream on
	               // the same file -- otherwise out's deferred flush at scope-end races the patch
	               // and zeroes phySize -> no server collision.
	FreeModelBuf(g_model.pBase);

	// RRIG generation disabled - not needed for converted models
	// The game loads animation data from external .rseq files via RPak
#if 0
	///////////////
	// ANIM RIGS //
	///////////////

	std::string rigName = originalModelName;
	if (rigName.rfind("animrig/", 0) != 0)
		rigName = "animrig/" + rigName;
	if (EndsWith(rigName, ".mdl"))
	{
		rigName = rigName.substr(0, rigName.length() - 4);
		rigName += ".rrig";
	}

	printf("Creating rig from model...\n");

	std::string rrigPath = baseOutputPath + ".rrig";
	std::ofstream rigOut(rrigPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	// Generate rig header
	pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	GenerateRigHdr_191(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// Reset string table for rig
	BeginStringTable();

	memcpy_s(&pHdr->name, 64, rigName.c_str(), min(rigName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, rigName.c_str());
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, surfaceProp);
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// Convert bones for rig
	ConvertBones_191(oldHeader, pMDL, oldHeader->boneCount, true);

	// Convert attachments for rig
	g_model.hdrV54()->localattachmentindex = ConvertAttachments_191(oldHeader, pMDL, oldHeader->numlocalattachments);

	// Convert hitboxsets for rig
	ConvertHitboxes_191(oldHeader, pMDL, oldHeader->numhitboxsets);

	// Copy bonebyname table
	if (oldHeader->bonetablebynameindex > 0)
	{
		const char* pOldBoneTable = (const char*)oldHeader + FIX_OFFSET(oldHeader->bonetablebynameindex);
		memcpy(g_model.pData, pOldBoneTable, oldHeader->boneCount);
		g_model.hdrV54()->bonetablebynameindex = static_cast<int>(g_model.pData - g_model.pBase);
		g_model.pData += oldHeader->boneCount;
		ALIGN4(g_model.pData);
	}

	// Convert pose parameters for rig
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams_191(oldHeader, pMDL, oldHeader->numlocalposeparameters, true);

	// Convert IK chains for rig
	ConvertIkChains_191(oldHeader, pMDL, oldHeader->numikchains, true);
	ALIGN4(g_model.pData);

	g_model.pData = WriteStringTable(g_model.pData);

	pHdr->length = static_cast<int>(g_model.pData - g_model.pBase);

	rigOut.write(g_model.pBase, pHdr->length);

	FreeModelBuf(g_model.pBase);

	g_model.stringTable.clear();
#endif

	///////////////
	// VG FILE   //
	///////////////

	// Check for VG file alongside the RMDL
	std::string vgFilePath = ChangeExtension(pathIn, "vg");
	std::string vgOutPath = baseOutputPath + ".vg";

	if (FILE_EXISTS(vgFilePath))
	{
		printf("Found VG file, attempting conversion...\n");

		uintmax_t vgInputSize = GetFileSize(vgFilePath);
		char* vgInputBuf = new char[vgInputSize];

		std::ifstream vgIfs(vgFilePath, std::ios::in | std::ios::binary);
		vgIfs.read(vgInputBuf, vgInputSize);
		vgIfs.close();

		int vgMagic = *(int*)vgInputBuf;

		if (vgMagic == 'GVt0')
		{
			// v12.1+ VG format - use existing converter
			printf("VG file is v12.1+ format (0tVG magic), converting...\n");
			ConvertVGData_12_1(vgInputBuf, vgFilePath, vgOutPath);
		}
		else if (vgMagic == '0GVt' || vgMagic == 0x47567430)
		{
			// Already v8/v9 format - copy as-is
			printf("VG file appears to be v8/v9 format, copying as-is...\n");
			std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
			vgOut.write(vgInputBuf, vgInputSize);
			vgOut.close();
			delete[] vgInputBuf;
		}
		else
		{
			// Check if this is v19.1 rev4 format (no magic, starts with small values for lodIndex, lodCount, etc.)
			// rev4 format: first 4 bytes are lodIndex(1), lodCount(1), groupIndex(1), lodMap(1)
			const vg::rev4::VertexGroupHeader_t* pTestHdr = reinterpret_cast<const vg::rev4::VertexGroupHeader_t*>(vgInputBuf);

			// Heuristic: if lodCount is reasonable (1-8) and lodMap is non-zero, assume rev4 format
			if (pTestHdr->lodCount > 0 && pTestHdr->lodCount <= 8 && pTestHdr->lodMap != 0)
			{
				printf("VG file appears to be v19.1 rev4 format (no magic, detected via header structure)\n");
				ConvertVGData_191(vgInputBuf, vgInputSize, vgOutPath, oldHeader, pMDL, fileSize);
				delete[] vgInputBuf;
			}
			else
			{
				// Unknown format - try to copy anyway
				printf("WARNING: VG file has unknown magic 0x%08X, copying as-is...\n", vgMagic);
				std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
				vgOut.write(vgInputBuf, vgInputSize);
				vgOut.close();
				delete[] vgInputBuf;
			}
		}
	}
	else
	{
		printf("WARNING: No VG file found at '%s'\n", vgFilePath.c_str());
		printf("         v19.1 VG data is typically stored in RPak files.\n");
		printf("         You may need to extract the VG data separately using Legion or similar tools.\n");
	}

	///////////////
	// PHY FILE  //
	///////////////

	// Check for PHY file alongside the RMDL and convert to v10 format
	std::string phyFilePath = ChangeExtension(pathIn, "phy");
	std::string phyOutPath = baseOutputPath + ".phy";

	if (FILE_EXISTS(phyFilePath))
	{
		printf("Found PHY file, converting to v10 format...\n");

		uintmax_t phyInputSize = GetFileSize(phyFilePath);
		char* phyInputBuf = new char[phyInputSize];

		std::ifstream phyIfs(phyFilePath, std::ios::in | std::ios::binary);
		phyIfs.read(phyInputBuf, phyInputSize);
		phyIfs.close();

		// V19.1 PHY format has a compact 4-byte header (phyheader_v16_t):
		//   [0-1]: solidCount       (uint16) - number of surface headers
		//   [2-3]: propertiesOffset (uint16) - offset to text/keyvalue block
		//
		// V10 PHY format has a 20-byte IVPS header:
		//   [0-3]: size (int32) = 20
		//   [4-7]: id (int32) = 1
		//   [8-11]: solidCount (int32) = forwarded from the source, NOT hardcoded
		//   [12-15]: checkSum (int32) = model checksum
		//   [16-19]: keyValuesOffset (int32) = v19_offset + 16
		//
		// Data after header is identical, just offset by 16 bytes.
		// The first u16 is solidCount, not a version -- hardcoding the v10
		// solidCount to 1 flattens every multi-solid phy (jointed props, the
		// 15-solid character ragdolls) down to the first hull. ConvertRMDL160To10
		// already forwards it; this path was left behind.
		const uint16_t v19SolidCount      = *reinterpret_cast<const uint16_t*>(phyInputBuf);
		const uint16_t v19KeyValuesOffset = *reinterpret_cast<const uint16_t*>(phyInputBuf + 2);

		printf("  V19 PHY: solidCount=%u, propertiesOffset=%u\n", v19SolidCount, v19KeyValuesOffset);

		// Create v10 IVPS header
		struct IVPSHeader {
			int32_t size;           // 20
			int32_t id;             // 1
			int32_t solidCount;     // forwarded from the source header
			int32_t checkSum;       // model checksum
			int32_t keyValuesOffset; // v19_offset + 16
		};

		IVPSHeader v10Header;
		v10Header.size = 20;
		v10Header.id = 1;
		v10Header.solidCount = v19SolidCount;
		v10Header.checkSum = oldHeader->checksum;  // Use model's checksum
		v10Header.keyValuesOffset = v19KeyValuesOffset + 16;  // Adjust for header size difference

		printf("  V10 PHY: size=%d, id=%d, solidCount=%d, checkSum=0x%08X, keyValuesOffset=%d\n",
			v10Header.size, v10Header.id, v10Header.solidCount, v10Header.checkSum, v10Header.keyValuesOffset);

		// Calculate output size: 20-byte header + (v19 data - 4-byte v19 header)
		size_t v10PhySize = sizeof(IVPSHeader) + (phyInputSize - 4);

		std::ofstream phyOut(phyOutPath, std::ios::out | std::ios::binary);

		// Write v10 IVPS header
		phyOut.write(reinterpret_cast<char*>(&v10Header), sizeof(IVPSHeader));

		// Copy v19 data (skip the 4-byte v19 header)
		phyOut.write(phyInputBuf + 4, phyInputSize - 4);

		phyOut.close();
		delete[] phyInputBuf;

		// Update phySize in the rmdl header
		std::fstream rmdlUpdate(rmdlPath, std::ios::in | std::ios::out | std::ios::binary);
		if (rmdlUpdate.is_open())
		{
			// Seek to phySize offset in studiohdr_t and write the size
			size_t phySizeOffset = offsetof(r5::v8::studiohdr_t, phySize);
			int phySizeValue = static_cast<int>(v10PhySize);
			rmdlUpdate.seekp(phySizeOffset);
			rmdlUpdate.write(reinterpret_cast<char*>(&phySizeValue), sizeof(int));
			rmdlUpdate.close();
			printf("  PHY converted successfully (v19: %llu bytes -> v10: %zu bytes)\n",
				phyInputSize, v10PhySize);
		}
		else
		{
			printf("  PHY converted but could not update phySize in header.\n");
		}
	}

	printf("Finished converting model '%s', proceeding...\n\n", rawModelName.c_str());
}
