// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/studio_r5_v19.h>
#include <studio/studio_r5_v16.h>
#include <studio/versions.h>
#include <core/utils.h>

/*
	RMDL v19.1 -> v17. studiohdr is the same 228B; bone data is not
	(16B -> 128B stride via ExpandBoneDataToV16). seqdesc 116->112, animdesc 48->40
	with inline animindex (clear flags 0x200000). External asqd animation is a
	warning -- supply aseq v11 via R5-AnimConv.
*/

namespace {

// All studiohdr u16 "offset from header base" fields. DERIVED FROM THE STRUCT
// (concern #3) -- no hand-transcribed literals -> zero transcription risk.
// localseqindex is handled explicitly (it is the SEQ-region boundary), so it is
// NOT in this shift table.
#define HDR_OFF(field) { #field, offsetof(r5::v191::studiohdr_t, field) }
struct HdrOffsetField { const char* name; size_t fieldOff; };

const HdrOffsetField s_hdrOffsetFields[] = {
	HDR_OFF(boneHdrOffset),
	HDR_OFF(boneDataOffset),
	// localseqindex handled explicitly.
	HDR_OFF(localattachmentindex),
	HDR_OFF(localnodenameindex),
	HDR_OFF(localNodeDataOffset),
	HDR_OFF(ikchainindex),
	HDR_OFF(textureindex),
	HDR_OFF(skinindex),
	HDR_OFF(bodypartindex),
	HDR_OFF(uiPanelOffset),
	HDR_OFF(localposeparamindex),
	HDR_OFF(surfacepropindex),
	HDR_OFF(keyvalueindex),
	HDR_OFF(bonetablebynameindex),
	HDR_OFF(boneStateOffset),
	HDR_OFF(groupHeaderOffset),
	HDR_OFF(lodOffset),
	HDR_OFF(srcbonetransformindex),
	HDR_OFF(sourceFilenameOffset),
	HDR_OFF(linearboneindex),
	HDR_OFF(procBoneOffset),
	HDR_OFF(linearProcBoneOffset),
	HDR_OFF(boneFollowerOffset),
	HDR_OFF(bvhOffset),
	HDR_OFF(unkDataOffset),
	HDR_OFF(unkStrcOffset),
};
#undef HDR_OFF

// The v19.1 "external animation data" flag. The v17 compiler clears it because
// the anim data is inline (verified vs a reference model: v19.1 0x220000 -> v17 0x20000;
// v19.1 0x200000 -> v17 0x0). Source flags above 0x200000 are preserved.
constexpr int kStudioAnimExternalFlag = 0x200000;

// seqdesc field byte offsets (within the 112B v17 / first-112B-of-116B v19.1
// seqdesc -- identical for these fields). Verified vs studio_r5_v16.h and the
// live reference layout dump.
constexpr size_t kSeq_szlabelindex          = 0;
constexpr size_t kSeq_szactivitynameindex   = 2;
constexpr size_t kSeq_numevents             = 12;
constexpr size_t kSeq_eventindex            = 14;
constexpr size_t kSeq_animindexindex        = 42;
constexpr size_t kSeq_numautolayers         = 78;
constexpr size_t kSeq_autolayerindex        = 80;
constexpr size_t kSeq_weightlistindex       = 82;
constexpr size_t kSeq_groupsize0            = 84;
constexpr size_t kSeq_groupsize1            = 85;
constexpr size_t kSeq_numiklocks            = 88;
constexpr size_t kSeq_iklockindex           = 90;
constexpr size_t kSeq_activitymodifierindex = 96;
constexpr size_t kSeq_numactivitymodifiers  = 98;
constexpr size_t kSeq_weightFixupOffset     = 108;
constexpr size_t kSeq_weightFixupCount      = 110;

constexpr size_t kOldSeqStride = 116; // v18/v19.1 seqdesc
constexpr size_t kNewSeqStride = 112; // v16/v17  seqdesc
constexpr size_t kOldAnimSize  = 48;  // v19.1 animdesc
constexpr size_t kNewAnimSize  = 40;  // v16/v17 animdesc

// The engine decodes a studiohdr u16 offset as (o & 0xFFFE) << (4 * (o & 1)) --
// the low bit is a x16 scale flag, which is how a 16-bit field addresses up to
// ~1MB. (studio_r5_v*.h define FIX_OFFSET as identity; that is only correct for
// even offsets below 64K, so encode/decode explicitly here.)
inline size_t DecodeHdrOffset(uint16_t o)
{
	return static_cast<size_t>(o & 0xFFFE) << (4 * (o & 1));
}

inline bool EncodeHdrOffset(size_t off, uint16_t& out)
{
	if (off < 0x10000 && (off & 1) == 0)
	{
		out = static_cast<uint16_t>(off);
		return true;
	}
	if ((off & 0xF) == 0 && (off >> 4) <= 0xFFFE)
	{
		out = static_cast<uint16_t>((off >> 4) | 1);
		return true;
	}
	return false;
}

inline bool NearlyOne(float v) { return v > 0.98f && v < 1.02f; }

// Does a boneCount-strided 128-byte array at `base` look like real v16 bone
// data? Bone 0 of every stock S21 model has parent -1, unit scale and a
// normalized quat.
bool LooksLikeV16BoneData(const char* buf, size_t size, size_t base, uint16_t boneCount)
{
	if (boneCount == 0 || base + static_cast<size_t>(boneCount) * 128 > size)
		return false;
	const r5::v160::mstudiobonedata_t* const b =
		reinterpret_cast<const r5::v160::mstudiobonedata_t*>(buf + base);
	if (b->parent != -1)
		return false;
	if (!NearlyOne(b->scale.x) || !NearlyOne(b->scale.y) || !NearlyOne(b->scale.z))
		return false;
	const float q = b->quat.x * b->quat.x + b->quat.y * b->quat.y
		+ b->quat.z * b->quat.z + b->quat.w * b->quat.w;
	return NearlyOne(q);
}

//
// ExpandBoneDataToV16
//
// v19.1 keeps only parent/flags/proc per bone and stores the pose in the
// linear-bone arrays; v16/v17 wants the pose INLINE in a 128-byte struct. A
// converted model that keeps the 16-byte array is parsed by the S21 engine at a
// 128-byte stride, so g_gpuBones -- the buffer the foliage wind vertex shader
// reads its sway pivot and branch matrices from -- is filled with garbage and
// the geometry is displaced out of the frustum.
//
// The expanded array is APPENDED at the end of the blob and boneDataOffset is
// re-pointed at it, so nothing in the middle of the file moves and exactly one
// header field changes. Returns the new file size (== `size` if nothing to do).
//
// Set by ExpandBoneDataToV16 to the source's 16-byte-per-bone array, which the
// re-point below orphans. Dead space inside the blob, big enough for the
// reference anim blocks, so using it keeps the converted model the same size as
// the one already packed into a pak.
static size_t s_orphanBoneOff  = 0;
static size_t s_orphanBoneSize = 0;

size_t ExpandBoneDataToV16(char* buf, size_t size, size_t capacity, const char* modelName)
{
	r5::v191::studiohdr_t* const hdr = reinterpret_cast<r5::v191::studiohdr_t*>(buf);
	const uint16_t boneCount = hdr->boneCount;
	if (boneCount == 0)
		return size;

	const size_t oldBoneOff = DecodeHdrOffset(hdr->boneDataOffset);

	if (LooksLikeV16BoneData(buf, size, oldBoneOff, boneCount))
		return size; // already expanded -- idempotent

	if (oldBoneOff + static_cast<size_t>(boneCount) * 16 > size)
	{
		printf("[v17]   WARNING: '%s' boneDataOffset %zu + %u*16 is past EOF -- bone data NOT expanded.\n",
			modelName, oldBoneOff, boneCount);
		return size;
	}

	const size_t newOff = (size + 15) & ~static_cast<size_t>(15);
	const size_t newEnd = newOff + static_cast<size_t>(boneCount) * 128;
	uint16_t encoded = 0;
	if (newEnd > capacity || !EncodeHdrOffset(newOff, encoded))
	{
		printf("[v17]   WARNING: '%s' cannot append %u expanded bones at %zu (capacity %zu) -- "
			"bone data NOT expanded; foliage wind on this model will be wrong.\n",
			modelName, boneCount, newOff, capacity);
		return size;
	}

	// The linear-bone block carries the pose arrays. It is optional: a model
	// without one gets the identity pose, which is what the shipping game uses for the
	// single-bone static props that make up almost all map content.
	const r5::v191::mstudiolinearbone_t* lb = nullptr;
	const size_t lbOff = DecodeHdrOffset(hdr->linearboneindex);
	if (hdr->linearboneindex != 0 && lbOff + sizeof(r5::v191::mstudiolinearbone_t) <= size)
	{
		const r5::v191::mstudiolinearbone_t* const cand =
			reinterpret_cast<const r5::v191::mstudiolinearbone_t*>(buf + lbOff);
		if (cand->numbones == boneCount)
			lb = cand;
	}
	if (!lb)
		printf("[v17]   NOTE: '%s' has no usable linear-bone block; expanding %u bone(s) with the identity pose.\n",
			modelName, boneCount);

	memset(buf + size, 0, newOff - size);

	const r5::v191::mstudiobonedata_t* const src =
		reinterpret_cast<const r5::v191::mstudiobonedata_t*>(buf + oldBoneOff);
	r5::v160::mstudiobonedata_t* const dst =
		reinterpret_cast<r5::v160::mstudiobonedata_t*>(buf + newOff);

	for (uint16_t i = 0; i < boneCount; i++)
	{
		r5::v160::mstudiobonedata_t& o = dst[i];
		memset(&o, 0, sizeof(o));

		if (lb)
		{
			o.poseToBone = *lb->pPoseToBone(i);
			o.qAlignment = *lb->pQAlignment(i);
			o.pos        = *lb->pPos(i);
			o.quat       = *lb->pQuat(i);
			o.rot        = *lb->pRot(i);
			o.scale      = *lb->pScale(i);
		}
		else
		{
			o.poseToBone.m_flMatVal[0][0] = 1.0f;
			o.poseToBone.m_flMatVal[1][1] = 1.0f;
			o.poseToBone.m_flMatVal[2][2] = 1.0f;
			o.quat.w = 1.0f;
			o.scale.x = o.scale.y = o.scale.z = 1.0f;
		}

		o.parent         = src[i].parent;
		o.unk_76         = src[i].unk_76;
		o.flags          = src[i].flags;
		o.collisionIndex = src[i].collisionIndex;
		o.proctype       = src[i].proctype;
		o.procindex      = src[i].procindex;
	}

	hdr->boneDataOffset = encoded;
	s_orphanBoneOff  = oldBoneOff;
	s_orphanBoneSize = static_cast<size_t>(boneCount) * 16;
	printf("[v17]   bone data: %u bone(s) expanded 16B -> 128B, appended at %zu (boneDataOffset=0x%04X), file %zu -> %zu\n",
		boneCount, newOff, encoded, size, newEnd);
	return newEnd;
}

//
// SourceBone0Quat
// Bone 0's reference rotation, read back out of the expanded v16 bone data.
// Falls back to identity when the bone array could not be expanded.
//
Quaternion SourceBone0Quat(const char* buf, size_t size)
{
	const r5::v191::studiohdr_t* const hdr = reinterpret_cast<const r5::v191::studiohdr_t*>(buf);
	const size_t off = DecodeHdrOffset(hdr->boneDataOffset);
	if (!LooksLikeV16BoneData(buf, size, off, hdr->boneCount))
		return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);

	return reinterpret_cast<const r5::v160::mstudiobonedata_t*>(buf + off)->quat;
}

inline uint16_t ReadU16(const char* p, size_t off) { return *reinterpret_cast<const uint16_t*>(p + off); }
inline int32_t  ReadI32(const char* p, size_t off) { return *reinterpret_cast<const int32_t*>(p + off); }
inline void     WriteU16(char* p, size_t off, uint16_t v) { *reinterpret_cast<uint16_t*>(p + off) = v; }
inline void     WriteI32(char* p, size_t off, int32_t  v) { *reinterpret_cast<int32_t*>(p + off) = v; }

} // anonymous namespace

//
// ConvertRMDL191To17
// Downgrade a v19.1 .rmdl to the v17 (S21-client) compact format, matching the
// structure of a real v17 model.
//
void ConvertRMDL191To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut)
{
	const std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();
	printf("[v17] Converting '%s' from mdl_ v19.1 -> v17 (input %zu bytes)\n", rawModelName.c_str(), fileSize);

	const r5::v191::studiohdr_t* const oldHdr =
		reinterpret_cast<const r5::v191::studiohdr_t*>(pMDL);

	const uint16_t numlocalseq   = oldHdr->numlocalseq;
	const uint16_t localseqindex = oldHdr->localseqindex; // offset from header base
	const uint16_t boneCount     = oldHdr->boneCount;

	printf("[v17]   numlocalseq=%u localseqindex=%u boneCount=%u numbodyparts=%u\n",
		numlocalseq, localseqindex, boneCount, oldHdr->numbodyparts);

	// Resolve the output path.
	std::filesystem::path inputPath(pathIn);
	std::string rmdlPath;
	if (pathOut != pathIn && !pathOut.empty())
	{
		rmdlPath = pathOut;
		std::filesystem::create_directories(std::filesystem::path(pathOut).parent_path());
	}
	else
	{
		std::filesystem::path outputDir = inputPath.parent_path() / "rmdlconv_out";
		std::filesystem::create_directories(outputDir);
		rmdlPath = (outputDir / inputPath.filename()).string();
	}

	// Output buffer. v17 is never larger than v19.1 (structs only shrink and the
	// inline anim block is degenerate/zero-length), so the input size is a safe
	// upper bound -- EXCEPT the seq-region rebuild now pads its end to preserve
	// oldRegionEnd's 16-byte residue (see the newRegionEnd comment below), and
	// that padding can need a few more bytes than the region's own natural
	// shrink saved (observed: a single-sequence model can save as few as 4-8
	// bytes). +32 slack is generous headroom for the worst case (at most 15
	// bytes of padding) so the rebuild can never write past the end of `out`.
	// +bone slack: ExpandBoneDataToV16 appends boneCount*128 bytes (16-byte
	// aligned) past the end of the converted blob.
	const size_t boneSlack = 16 + static_cast<size_t>(boneCount) * 128;
	// +anim slack: one 16-byte-aligned reference anim block per animdesc, appended
	// after the bone array. 16 + AnimRefBlockSize covers alignment and the block.
	const size_t animSlack = static_cast<size_t>(numlocalseq) * 4 *
		(16 + static_cast<size_t>(AnimRefBlockSize(boneCount > 0 ? boneCount : 1)));
	const size_t outCap = fileSize + 32 + boneSlack + animSlack;
	std::unique_ptr<char[]> outBuf(new char[outCap]{});
	char* const out = outBuf.get();

	// ---- Degenerate case: no sequences -> the whole file is a pure passthrough.
	if (numlocalseq == 0 || localseqindex == 0)
	{
		memcpy(out, pMDL, fileSize);
		const size_t outSize = ExpandBoneDataToV16(out, fileSize, outCap, rawModelName.c_str());
		std::ofstream ofs(rmdlPath, std::ios::out | std::ios::binary);
		ofs.write(out, static_cast<std::streamsize>(outSize));
		printf("[v17]   no sequences -> passthrough. wrote %zu bytes to %s\n", outSize, rmdlPath.c_str());
		return;
	}

	// ---- Measure the OLD seq/anim region so we know where Zone C begins.
	const size_t seqArrStart = localseqindex;
	const size_t seqArrEnd   = seqArrStart + static_cast<size_t>(numlocalseq) * kOldSeqStride;

	// The end of the per-seq anim-data region in the source = the highest byte
	// touched by any seqdesc's animindexindex array / animdescs.
	size_t oldRegionEnd = seqArrEnd;
	bool   sawGuid      = false;
	bool   sawBlends    = false;
	bool   sawCounts    = false;
	bool   sawSections  = false;

	for (uint16_t i = 0; i < numlocalseq; i++)
	{
		const size_t so  = seqArrStart + static_cast<size_t>(i) * kOldSeqStride;
		const uint16_t aii = ReadU16(pMDL, so + kSeq_animindexindex);
		const uint8_t gs0  = static_cast<uint8_t>(pMDL[so + kSeq_groupsize0]);
		const uint8_t gs1  = static_cast<uint8_t>(pMDL[so + kSeq_groupsize1]);
		const int nanims   = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;
		if (nanims > 1) sawBlends = true;

		// Guard: any non-zero count on a sub-array whose relocation we do not
		// implement.
		if (ReadU16(pMDL, so + kSeq_numevents) > 0 ||
			ReadU16(pMDL, so + kSeq_numautolayers) > 0 ||
			ReadU16(pMDL, so + kSeq_numiklocks) > 0 ||
			ReadU16(pMDL, so + kSeq_numactivitymodifiers) > 0 ||
			ReadU16(pMDL, so + kSeq_weightFixupCount) > 0)
			sawCounts = true;

		if (aii == 0)
			continue;

		const size_t aiiAbs = so + aii;
		if (aiiAbs + static_cast<size_t>(nanims) * 2 > fileSize)
			continue;
		oldRegionEnd = max(oldRegionEnd, aiiAbs + static_cast<size_t>(nanims) * 2);

		for (int a = 0; a < nanims; a++)
		{
			const uint16_t rel = ReadU16(pMDL, aiiAbs + static_cast<size_t>(a) * 2);
			if (rel == 0)
				continue;
			const size_t animAbs = so + rel;
			if (animAbs + kOldAnimSize > fileSize)
				continue;
			oldRegionEnd = max(oldRegionEnd, animAbs + kOldAnimSize);

			const r5::v191::mstudioanimdesc_t* const src =
				reinterpret_cast<const r5::v191::mstudioanimdesc_t*>(pMDL + animAbs);
			if (src->animDataAsset != 0) sawGuid = true;
			if (src->sectionindex != 0 || src->sectionframes != 0) sawSections = true;
		}
	}

	// ---- LOUD GUARDS (concerns #1 and #2). These announce themselves so a model
	// we cannot faithfully reproduce is never silently mishandled.
	if (sawGuid)
		printf("[v17]   WARNING: '%s' has external animDataAsset GUID(s) -- this is an ANIMATED model. "
			"The inline per-frame RLE is NOT reproduced; the animation MUST be supplied as an external "
			"aseq v11 asset via R5-AnimConv. Emitting an reference-shaped degenerate inline block (structurally valid).\n",
			rawModelName.c_str());
	if (sawSections)
		printf("[v17]   WARNING: '%s' carries inline anim SECTIONS (sectionindex/sectionframes != 0). "
			"Streamed sections are NOT relocated; clearing them. Verify animation in-game.\n",
			rawModelName.c_str());
	if (sawBlends)
		printf("[v17]   WARNING: '%s' has a sequence with blends (nanims>1). The multi-cell weightlist "
			"packing is best-effort and may not byte-match the reference model. Verify in-game.\n",
			rawModelName.c_str());
	if (sawCounts)
		printf("[v17]   WARNING: '%s' has a seqdesc with a non-zero event/autolayer/iklock/activitymodifier/"
			"weightFixup count -- that sub-array relocation is NOT implemented. Offsets kept in-bounds; "
			"verify the sequence in-game.\n",
			rawModelName.c_str());

	// ---- Zone A: verbatim copy [0 .. seqArrStart).
	memcpy(out, pMDL, seqArrStart);

	// ---- Rebuild the seq region in the canonical reference layout.
	// All seqdescs first (numlocalseq * 112), then per-seq:
	//   [weightlist boneCount*float(1.0)] [aii array nanims*u16, pad to 4] [animdesc 40].
	const size_t newSeqArrEnd = seqArrStart + static_cast<size_t>(numlocalseq) * kNewSeqStride;

	char*  writePtr = out + newSeqArrEnd;

	// Record self-relative string offsets that must be re-pointed once the final
	// byte shift is known (their targets live in Zone C).
	struct StrFix { size_t newBaseOff; size_t fieldOff; size_t oldTargetAbs; };
	std::vector<StrFix> strFixups;

	for (uint16_t i = 0; i < numlocalseq; i++)
	{
		const size_t oldSeqOff = seqArrStart + static_cast<size_t>(i) * kOldSeqStride;
		const size_t newSeqOff = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		char* const  newSeq    = out + newSeqOff;

		// seqdesc body: copy the first 112 bytes verbatim (drop the v18 tail).
		memcpy(newSeq, pMDL + oldSeqOff, kNewSeqStride);

		const uint8_t gs0    = static_cast<uint8_t>(pMDL[oldSeqOff + kSeq_groupsize0]);
		const uint8_t gs1    = static_cast<uint8_t>(pMDL[oldSeqOff + kSeq_groupsize1]);
		const int     nanims = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;

		// 1) weightlist: boneCount * float(1.0f). Matches the reference model exactly.
		ALIGN4(writePtr);
		const size_t weightOff = static_cast<size_t>(writePtr - out);
		const uint16_t numWeights = boneCount > 0 ? boneCount : 1;
		for (uint16_t w = 0; w < numWeights; w++)
		{
			*reinterpret_cast<float*>(writePtr) = 1.0f;
			writePtr += sizeof(float);
		}
		const uint16_t weightRel = static_cast<uint16_t>(weightOff - newSeqOff);
		WriteU16(newSeq, kSeq_weightlistindex, weightRel);

		// 2) animindexindex array: nanims * uint16, padded so the animdesc is 4B
		//    aligned (the reference model keeps animdescs 4-byte aligned).
		ALIGN4(writePtr);
		const size_t arrOff = static_cast<size_t>(writePtr - out);
		const uint16_t arrRel = static_cast<uint16_t>(arrOff - newSeqOff);
		WriteU16(newSeq, kSeq_animindexindex, arrRel);
		uint16_t* const newArr = reinterpret_cast<uint16_t*>(writePtr);
		writePtr += static_cast<size_t>(nanims) * sizeof(uint16_t);
		ALIGN4(writePtr);

		// 3) The count==0 sub-array offset fields. The reference model points them at the
		//    in-region weightlist / animindexindex array (NOT at garbage) so they
		//    stay in-bounds. Counts are 0 so they are never dereferenced.
		WriteU16(newSeq, kSeq_eventindex,            weightRel); // numevents==0
		WriteU16(newSeq, kSeq_autolayerindex,        weightRel); // numautolayers==0
		WriteU16(newSeq, kSeq_iklockindex,           arrRel);    // numiklocks==0
		WriteU16(newSeq, kSeq_activitymodifierindex, arrRel);    // numactivitymodifiers==0

		// 4) animdescs (40B v16 each), one per blend cell.
		const size_t oldAiiAbs = oldSeqOff + ReadU16(pMDL, oldSeqOff + kSeq_animindexindex);
		const bool   haveOldAii = ReadU16(pMDL, oldSeqOff + kSeq_animindexindex) != 0;

		for (int a = 0; a < nanims; a++)
		{
			ALIGN4(writePtr);
			const size_t animOff = static_cast<size_t>(writePtr - out);

			r5::v160::mstudioanimdesc_t* const dst =
				reinterpret_cast<r5::v160::mstudioanimdesc_t*>(writePtr);
			memset(dst, 0, sizeof(r5::v160::mstudioanimdesc_t));

			const uint16_t oldRel = (haveOldAii && oldAiiAbs + static_cast<size_t>(a) * 2 + 2 <= fileSize)
				? ReadU16(pMDL, oldAiiAbs + static_cast<size_t>(a) * 2) : 0;

			if (oldRel != 0)
			{
				const size_t oldAnimAbs = oldSeqOff + oldRel;
				const r5::v191::mstudioanimdesc_t* const src =
					reinterpret_cast<const r5::v191::mstudioanimdesc_t*>(pMDL + oldAnimAbs);

				// Shared head: fps/flags/numframes/sznameindex/framemovementindex.
				dst->fps       = src->fps;
				// Clear the 0x200000 "external data" bit -> matches the reference model.
				dst->flags     = src->flags & ~kStudioAnimExternalFlag;
				dst->numframes = src->numframes;
				dst->numikrules = 0; // count guarded above; not relocated
				dst->ikruleindex = 0;
				dst->sectionDataExternal = 0;
				dst->unk1 = 0;
				dst->sectionindex = 0;
				dst->sectionstallframes = 0;
				dst->sectionframes = 0;

				// sznameindex (+0xC) -> string table (Zone C). Record for re-point.
				const uint16_t oldNameRel = ReadU16(pMDL, oldAnimAbs + 12);
				if (oldNameRel != 0)
					strFixups.push_back({ animOff, 12, oldAnimAbs + oldNameRel });
				dst->sznameindex = 0;
			}
			else
			{
				// Synthesise a minimal valid animdesc (numframes==1 reference).
				dst->fps = 30.0f;
				dst->numframes = 1;
			}

			// v17 inline animindex: the source carries the per-frame data
			// externally (asqd) and every district seq is a numframes==1 reference
			// with NO inline RLE, so the reference model points animindex just past the
			// animdesc structure -> a zero-length / EOF block. We do the same:
			// animindex resolves to the end of the file once relocation completes.
			// Stamp a sentinel here and patch it in the size-aware post-pass.
			dst->animindex = 0; // patched below to (newFileSize - animOff)

			newArr[a] = static_cast<uint16_t>(animOff - newSeqOff);
			writePtr += sizeof(r5::v160::mstudioanimdesc_t);
		}

		// weightFixupOffset (count==0): the reference model points it just past THIS seq's
		// per-seq block (i.e. at the next seq's region start, or Zone C for the
		// last seq) -> set it to the current write cursor. Never dereferenced
		// (weightFixupCount==0) but reproduces the reference model's value exactly.
		ALIGN4(writePtr);
		WriteU16(newSeq, kSeq_weightFixupOffset,
			static_cast<uint16_t>(static_cast<size_t>(writePtr - out) - newSeqOff));
	}

	// End of the rebuilt seq region in the OUTPUT buffer.
	//
	// Every header offset field that resolves into Zone C (copied verbatim below,
	// including the collision/BVH data) is patched below as `v -= bytesSaved`
	// (bytesSaved = oldRegionEnd - newRegionEnd). So a field that was N-byte
	// aligned in the SOURCE stays N-byte aligned in the OUTPUT iff bytesSaved
	// itself is a multiple of N -- NOT iff newRegionEnd lands on an absolute
	// N-byte file offset (a boundary the source's own Zone-C layout has no
	// relationship to; rounding newRegionEnd to an absolute 16 REGRESSED
	// rib_giant_05 from mod16==8 to mod16==4, since its oldRegionEnd (492) isn't
	// itself a multiple of 16). The correct target is: match newRegionEnd's
	// residue mod 16 to oldRegionEnd's residue mod 16.
	//
	// This must be exactly 16, not merely a divisor of it: studiohdr_v17_t::
	// bvhOffset is a HEADER-BASE offset (STUDIO_FIX_OFFSET only -- see the
	// client's pLODGroup vs pTexture distinction in studio.h), and the S21
	// engine's collision-tree BVH walk reads each 64-byte node
	// with a legacy (non-VEX) SSE2 punpcklwd against a 128-bit memory operand,
	// which requires 16-byte alignment or the CPU raises #GP(0).
	// Native S21 models with collision data have bvhOffset mod 16 == 0 --
	// a hard engine invariant. The v19.1 source's own oldRegionEnd is itself
	// already 16-aligned in almost every sampled file, so for the overwhelming
	// majority of models "match oldRegionEnd's residue" and "round up to an
	// absolute 16" agree -- but only the residue-match is correct in general,
	// and only it is what the shift arithmetic below actually requires.
	// A converted model that lands at mod16==8 will AV in the job-worker BVH
	// walk; the collision DATA is fine, only its absolute placement is wrong.
	size_t newRegionEnd = static_cast<size_t>(writePtr - out);
	{
		const size_t wantResidue = oldRegionEnd % 16;
		const size_t haveResidue = newRegionEnd % 16;
		const size_t pad = (wantResidue >= haveResidue)
			? (wantResidue - haveResidue)
			: (16 - haveResidue + wantResidue);
		newRegionEnd += pad;
	}
	writePtr = out + newRegionEnd;

	// ---- Zone C: verbatim copy [oldRegionEnd .. fileSize) shifted to newRegionEnd.
	const size_t zoneCSize  = fileSize - oldRegionEnd;
	memcpy(out + newRegionEnd, pMDL + oldRegionEnd, zoneCSize);

	const size_t newFileSize = newRegionEnd + zoneCSize;
	const long   bytesSaved  = static_cast<long>(oldRegionEnd) - static_cast<long>(newRegionEnd);

	// ---- Fix studiohdr offset fields that point into the shifted Zone C.
	// localseqindex is unchanged (Zone A boundary did not move).
	for (const HdrOffsetField& f : s_hdrOffsetFields)
	{
		const uint16_t v = ReadU16(out, f.fieldOff);
		if (v == 0)
			continue;
		if (v >= oldRegionEnd)
			WriteU16(out, f.fieldOff, static_cast<uint16_t>(static_cast<long>(v) - bytesSaved));
		// Offsets resolving into Zone A (< seqArrStart) are untouched.
		// (No district header field resolves INTO the seq region.)
	}

	// ---- Re-point the seqdesc self-relative string offsets (szlabelindex,
	// szactivitynameindex). Their targets live in Zone C; both the seqdesc and the
	// target moved.
	for (uint16_t i = 0; i < numlocalseq; i++)
	{
		const size_t oldSeqOff = seqArrStart + static_cast<size_t>(i) * kOldSeqStride;
		const size_t newSeqOff = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		char* const  newSeq    = out + newSeqOff;

		for (const size_t fo : { kSeq_szlabelindex, kSeq_szactivitynameindex })
		{
			const uint16_t oldRel = ReadU16(pMDL, oldSeqOff + fo);
			if (oldRel == 0)
				continue;
			const size_t oldTargetAbs = oldSeqOff + oldRel;
			size_t newTargetAbs = oldTargetAbs;
			if (oldTargetAbs >= oldRegionEnd) // Zone C
				newTargetAbs = oldTargetAbs - static_cast<size_t>(bytesSaved);
			else if (oldTargetAbs >= seqArrStart) // (should not happen for strings)
				continue;
			WriteU16(newSeq, fo, static_cast<uint16_t>(newTargetAbs - newSeqOff));
		}
		// weightFixupOffset is set in the seq-region build loop above (reference model: points
		// just past this seq's per-seq block); nothing to do here.
	}

	// ---- Re-point each animdesc.sznameindex at its relocated string entry, and
	// stamp the degenerate inline animindex (-> end of file).
	for (const StrFix& nf : strFixups)
	{
		size_t newTargetAbs = nf.oldTargetAbs;
		if (nf.oldTargetAbs >= oldRegionEnd)
			newTargetAbs = nf.oldTargetAbs - static_cast<size_t>(bytesSaved);
		WriteU16(out, nf.newBaseOff + nf.fieldOff, static_cast<uint16_t>(newTargetAbs - nf.newBaseOff));
	}

	// Bone data first: it appends past the current end, so the animindex EOF
	// markers below must be computed against the FINAL size. Patching them before
	// the append leaves them pointing at the first expanded bone matrix instead of
	// at a zero-length block.
	const size_t outSize = ExpandBoneDataToV16(out, newFileSize, outCap, rawModelName.c_str());

	// Append one reference anim block per animdesc and point animindex at it. The
	// reference model stores each block 16-byte aligned in the model's tail; "animindex ->
	// EOF" is NOT equivalent, because inside a pak the byte after the blob is the
	// next asset's header, which the runtime then reads as the bone-flag array.
	// Prefer the orphaned bone array: writing there keeps the blob the same size,
	// which is the only way a converted model can replace one already packed into
	// a pak. Fall back to appending when it is absent or too small.
	const int blockStride = (AnimRefBlockSize(boneCount > 0 ? boneCount : 1) + 3) & ~3;
	int animBlocksNeeded = 0;
	for (uint16_t i = 0; i < numlocalseq; i++)
	{
		const size_t so = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		if (ReadU16(out + so, kSeq_animindexindex) == 0)
			continue;
		const uint8_t g0 = static_cast<uint8_t>(out[so + kSeq_groupsize0]);
		const uint8_t g1 = static_cast<uint8_t>(out[so + kSeq_groupsize1]);
		animBlocksNeeded += (g0 * g1) > 0 ? (g0 * g1) : 1;
	}
	const bool bUseOrphan = s_orphanBoneSize >= static_cast<size_t>(animBlocksNeeded) * blockStride;
	size_t animBlockCur = bUseOrphan ? ((s_orphanBoneOff + 3) & ~static_cast<size_t>(3)) : outSize;
	if (!bUseOrphan && animBlocksNeeded > 0)
		printf("[v17]   NOTE: orphaned bone array (%zu B at %zu) too small for %d ref block(s) "
			"of %d B -- appending, blob grows.\n",
			s_orphanBoneSize, s_orphanBoneOff, animBlocksNeeded, blockStride);
	const Quaternion bone0 = SourceBone0Quat(out, outSize);
	for (uint16_t i = 0; i < numlocalseq; i++)
	{
		const size_t newSeqOff = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		char* const  newSeq    = out + newSeqOff;
		const uint16_t arrRel  = ReadU16(newSeq, kSeq_animindexindex);
		if (arrRel == 0)
			continue;
		const uint8_t gs0    = static_cast<uint8_t>(newSeq[kSeq_groupsize0]);
		const uint8_t gs1    = static_cast<uint8_t>(newSeq[kSeq_groupsize1]);
		const int     nanims = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;
		const size_t  arrAbs = newSeqOff + arrRel;
		for (int a = 0; a < nanims; a++)
		{
			const uint16_t animRel = ReadU16(out, arrAbs + static_cast<size_t>(a) * 2);
			if (animRel == 0)
				continue;
			const size_t animAbs = newSeqOff + animRel;
			if (!bUseOrphan)
				animBlockCur = (animBlockCur + 15) & ~static_cast<size_t>(15);
			memset(out + animBlockCur, 0, static_cast<size_t>(blockStride));
			WriteAnimRefBlock(out + animBlockCur, boneCount > 0 ? boneCount : 1, bone0);
			// animindex @ +0x10 (int), self-relative to the animdesc.
			WriteI32(out, animAbs + 16, static_cast<int32_t>(animBlockCur) - static_cast<int32_t>(animAbs));
			animBlockCur += static_cast<size_t>(blockStride);
		}
	}
	const size_t finalSize = bUseOrphan ? outSize : ((animBlockCur + 15) & ~static_cast<size_t>(15));

	std::ofstream ofs(rmdlPath, std::ios::out | std::ios::binary);
	ofs.write(out, static_cast<std::streamsize>(finalSize));
	ofs.close();

	printf("[v17]   rebuilt seq region: oldEnd=%zu newEnd=%zu saved=%ld bytes\n",
		oldRegionEnd, newRegionEnd, bytesSaved);
	printf("[v17]   wrote %zu bytes (was %zu) -> %s\n", finalSize, fileSize, rmdlPath.c_str());
}
