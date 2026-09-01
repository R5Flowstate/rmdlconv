// Copyright (c) 2023, rexx
// See LICENSE.txt for licensing information (GPL v3)

#pragma once

enum MdlVersion : int
{
	GARRYSMOD   = 48,
	PORTAL2     = 49,
	TITANFALL   = 52,
	TITANFALL2  = 53,
	APEXLEGENDS = 54
};

enum class eRMdlSubVersion : char
{
	VERSION_UNK = -1,
	VERSION_8,
	VERSION_9,
	VERSION_10,
	VERSION_11,
	VERSION_12,
	VERSION_12_1,
	VERSION_12_2,
	VERSION_13,
	VERSION_14,
	VERSION_15,
	VERSION_16,
	VERSION_17,
	VERSION_18,
	VERSION_19,
	VERSION_19_1
};

// conversion to rmdl v10 (studio version 54)
void ConvertMDL48To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertMDL49To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertMDL53To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL8To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);

void ConvertRMDL120To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL121To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL122To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
// v12.3 uses same structure as v12.2 (only animation format changed) - use ConvertRMDL122To10
void ConvertRMDL124To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL125To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL160To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut, int subversion = 16);
void ConvertRMDL191To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut);

// Shared unk4_v54 static-prop walkable-part selector (format-neutral: operates purely on the
// CONVERTED v8 collision headers, not on any source-version structure). See rmdl_160.cpp's
// definition + the long design-rationale comment above it (ConvertStudioHdr_160's unk4_v54
// block) for the full background: this picks the BVH4 part a player actually lands on across
// the model's own footprint, for the case where the source bvhUnk value is unusable (OOB/
// sentinel, or headerCount==1). Verified on the v160/S21 path; reused
// as-is by the v19.1 path (rmdl_191.cpp) since the struct/bit layout it reads is v8-only.
namespace r5 { namespace v8 { struct studiohdr_t; struct dsurfaceproperty_t; } }

// Shared "modern -> legacy" collision surface-property transform (format-neutral:
// the source-side per-part inputs are passed as plain pointers/scalars so both the
// v160 (rmdl_160.cpp) and v19.1 (rmdl_191.cpp) converters -- whose modern
// mstudiocollheader_t layouts are byte-identical -- can drive it from their own
// header types without a shared source struct). See the long design-rationale
// comment above CollSurfPropPlan_t's definition in rmdl_160.cpp. The S21
// binary still carries both the LEGACY and MODERN resolution branches.
//
// One entry per source collision part (headerCount of them), in header order.
struct CollSurfPartInput_t
{
	const char* nodeBase;      // part's BVH node array base (collBase + nodesOfs)
	const char* leafBase;      // part's BVH leaf array base (collBase + leafDataOfs)
	const char* vertBase;      // part's vertex array base (collBase + vertsOfs)
	const char* skinInfosBase; // part's skinInfos array base (collBase + skinInfosOfs), meshGroupCount*skinCount*4B
	bool        packedVerts;   // bvhFlags bit0
	float       origin[3];
	float       scaleTrue;     // decodeScale * 65536.0f (BvhVertexAt/BvhNodeChild convention)
	int         leafDataDwords;// size of this part's leaf block in dwords (bounds the enumerator)
	uint8_t     skinCount;     // 0 => LEGACY part
	uint8_t     meshGroupCount;// 0 => LEGACY part
};

// One dedupe'd output surface-prop row, ready to write as r5::v8::dsurfaceproperty_t
// (nameOffset is always 0 -- see the format notes above).
struct CollSurfOutRow_t
{
	uint16_t surfFlags;
	uint8_t  finalId;
	uint8_t  contentsIdx;
};

// One BVH leaf dword that must be rewritten in the OUTPUT buffer.
struct CollSurfLeafRemap_t
{
	int part;         // index into the CollSurfPartInput_t array passed to the planner
	int leafDwordIdx; // dword index relative to that part's leafBase
	int newRowIdx;    // index into the planned outRows table (low-12 replacement)
};

struct CollSurfPropPlan_t
{
	bool anyModern = false;
	std::vector<CollSurfOutRow_t> outRows;
	std::vector<CollSurfLeafRemap_t> remaps;
};

// PRE-PASS: enumerate every leaf of every part from the SOURCE (modern encoding),
// resolve each to (surfFlags, finalId, contentsIdx) per the engine semantics, and
// build the deduped output row table + per-leaf rewrite list. `srcRows` is the
// source dsurfaceproperty_t-shaped row array (srcRowCount entries); `modelName` is
// used only for diagnostic printf/Warning context.
CollSurfPropPlan_t PlanCollisionSurfacePropRemap(
	const std::vector<CollSurfPartInput_t>& parts,
	const r5::v8::dsurfaceproperty_t* const srcRows, const int srcRowCount,
	const char* const modelName);

// APPLIER: given the plan and a part index + that part's leaf block already copied
// into the destination buffer (dstLeafBase, dstLeafDwords), rewrite the low-12 bits
// of every remapped leaf header dword in place.
void ApplyCollisionSurfacePropRemap(const CollSurfPropPlan_t& plan, int part,
	char* const dstLeafBase, const int dstLeafDwords, const char* const modelName);

// compact downgrade: mdl_ v19.1 -> v17 (S21 client). Keeps the
// v19.1 on-disk shape; only shrinks seqdesc 116B->112B and animdesc 48B->40B.
void ConvertRMDL191To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut);

// OLD->NEW upgrade: mdl_ v12.2 (S9-11 == "S10") -> v17 (S21 shipping client).
// Full rebuild into the v160/v170 layout (split bones, uint16 offsets, no magic,
// material path->StringToGuid). Sibling .vg (rev2 0tVG) and .phy (20B IVPS) are
// upgraded via ConvertVGData_Rev2To17 / ConvertPhy_122To17.
// vgDecompressedSize = the size of the produced rev4 .vg (from ConvertVGData_Rev2To17);
// the rmdl groupHeader (studio_hw_groupdata) must describe that single-group VG. Pass 0
// if there is no .vg (no group header emitted).
void ConvertRMDL122To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut, uint32_t vgDecompressedSize);
// returns the size in bytes of the produced rev4 .vg (0 on skip/error).
size_t ConvertVGData_Rev2To17(char* inputBuf, const size_t inputSize, const std::string& filePath, const std::string& pathOut);
void ConvertPhy_122To17(char* phyBuf, const size_t phySize, const std::string& filePath, const std::string& pathOut);

// OLD->NEW upgrade: rmdl v8 (S0-S6 / S3-legacy TSDI) -> v17 (S21 client).
// Full rebuild (same contract as ConvertRMDL122To17). Collision 32B headers are
// expanded to the v120 40B shape. LOD thresholds / meshCount come from the VG.
// vgLodCount + vgLodSwitchPoints describe the produced rev4 VG (pass 0 / nullptr
// when there is no VG). boneStates/boneStateCount is the rev1 VG bone remap
// table relocated into the v17 studiohdr (pass nullptr / 0 when absent).
void ConvertRMDL8To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut,
	uint32_t vgDecompressedSize, int vgLodCount, const float* vgLodSwitchPoints,
	const uint8_t* boneStates, int boneStateCount);
// rev1 '0tVG' (CreateVGFile / early Apex) -> rev4. Optionally returns lodCount +
// per-LOD switchPoint floats (up to maxLods) and the bone remap buffer
// (outBoneStates / outBoneStateCount, up to maxBoneStates).
size_t ConvertVGData_Rev1To17(char* inputBuf, const size_t inputSize, const std::string& filePath,
	const std::string& pathOut, int* outLodCount, float* outSwitchPoints, int maxLods,
	uint8_t* outBoneStates, int* outBoneStateCount, int maxBoneStates);
// Orchestrators: sibling VG/phy, optional vtx/vvd -> CreateVGFile.
void ConvertClientModel_8To17(const std::string& inputFile, const std::string& outputFile);
// Portal 2 MDL v49 -> v8 intermediate (ConvertMDL49To54) -> v17.
void ConvertClientModel_49To17(const std::string& inputFile, const std::string& outputFile);

// conversion to mdl v53
void ConvertMDL52To53(char* pMDL, const std::string& pathIn, const std::string& pathOut);

// VG
void ConvertVGData_12_1(char* inputBuf, const std::string& filePath, const std::string& pathOut);
void ConvertVGData_Rev3(char* inputBuf, const std::string& filePath, const std::string& pathOut);

// RMDL v14/v15 conversion
void ConvertRMDL140To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL150To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);

// deprecated
//void CreateVGFile_v8(const std::string& filePath);


// rseq conversion
void ConvertRSEQFrom71To7(char* inputBuf, char* inputExternalBuf, const std::string& filePath);
void ConvertRSEQFrom10To7(char* inputBuf, char* inputExternalBuf, const std::string& filePath);

// model conversion handlers
void UpgradeStudioModelTo53(std::string& modelPath, const char* outputDir);
void UpgradeStudioModelTo54(std::string& modelPath, const char* outputDir);
void UpgradeStudioModel(std::string& modelPath, int targetVersion, const char* outputDir);