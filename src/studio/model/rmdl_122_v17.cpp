// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/studio_r5_v16.h>
#include <studio/versions.h>
#include <core/utils.h>
#include <collision/phy_parser.h>

/*
	RMDL v12.2 -> v17. Full rebuild: 524B IDST header -> 228B v17, fat bones ->
	hdr+data, texture paths -> StringToGuid("material/<path>.rpak"). Collision
	blob copies verbatim. Animated seqs warn -- supply aseq v11 via R5-AnimConv.
*/

namespace {

// ---- Output cursor helpers (self-contained; the g_model string-table builder
// uses int offsets and is wrong for the uint16 v17 layout, so we roll our own).
struct OutBuf
{
	char*  base;
	size_t cur;
	size_t cap;
};

inline void Align(OutBuf& o, size_t a)
{
	while (o.cur % a) o.cur++;
}

// v17 offset semantics (verified vs a reference model):
//   * HEADER fields resolve from the HEADER BASE (file offset 0). So the stored
//     value is just the absolute output offset.
//   * SUB-STRUCT fields (bonehdr/hitboxset/seqdesc/bodypart/... ) resolve from
//     their containing struct's base. So the stored value is target - structBase.

// Header field at absolute header offset `fieldAbs` -> store absolute target.
inline void SetHdrOff(OutBuf& o, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) = static_cast<uint16_t>(targetAbs);
}

// A handful of v170 studiohdr offset fields resolve from the FIELD's own offset, NOT
// the header base: boneStateOffset, groupHeaderOffset, lodOffset (the engine helpers
// pBoneStates/pLODGroup/pLODThreshold all do `(char*)this + offsetof(field) + value`).
// Verified vs a reference model: fence groupHeaderOffset = 660 = 840(abs) - 180(field).
inline void SetHdrOffFieldRel(OutBuf& o, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) = static_cast<uint16_t>(targetAbs - fieldAbs);
}

// Sub-struct field at absolute offset `fieldAbs`, whose struct begins at
// `structBase` -> store target relative to the struct base.
inline void SetSubOff(OutBuf& o, size_t structBase, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) =
		static_cast<uint16_t>(targetAbs - structBase);
}

inline void WriteU16(char* p, size_t off, uint16_t v) { *reinterpret_cast<uint16_t*>(p + off) = v; }
inline uint16_t ReadU16(const char* p, size_t off) { return *reinterpret_cast<const uint16_t*>(p + off); }
inline void WriteI32(char* p, size_t off, int32_t v) { *reinterpret_cast<int32_t*>(p + off) = v; }

// ----------------------------------------------------------------------------
// RTech::StringToGuid (StringToGuidAligned) -- canonical Apex/Titanfall pak
// asset-name hash. Verified vs named district assets and against S10->S21
// v17 texture pairs. Pure-arithmetic 32/64-bit port; reads up to 4 bytes
// past the terminator, so the caller must pass a padded buffer.
// ----------------------------------------------------------------------------
static uint64_t RTech_StringToGuid(const char* const pString)
{
	uint32_t       currentByte = 0;
	uint64_t       v1 = 0;
	uint64_t       v11 = 0;
	uint64_t       v12 = 0;
	uint32_t       v5 = 0;

	const uint8_t* const p = reinterpret_cast<const uint8_t*>(pString);

	while (true)
	{
		const uint32_t w =
			static_cast<uint32_t>(p[currentByte]) |
			(static_cast<uint32_t>(p[currentByte + 1]) << 8) |
			(static_cast<uint32_t>(p[currentByte + 2]) << 16) |
			(static_cast<uint32_t>(p[currentByte + 3]) << 24);

		const uint32_t v4 = (~w & (w - 0x1010101u)) & 0x80808080u;
		v5 = v4 ^ (v4 - 1u);
		const uint32_t v6 = (v5 & w) ^ 0x5C5C5C5Cu;
		const uint32_t v7 = (~v6 & (v6 - 0x1010101u)) & 0x80808080u;
		uint32_t       v8 = v7 & (0u - v7);

		if (v7 != v8)
		{
			uint32_t v9 = 0xFF000000u;
			while (true)
			{
				const uint32_t v10 = v9;
				if ((v9 & v6) == 0)
					v8 |= v9 & 0x80808080u;
				v9 >>= 8;
				if (v10 < 0x100u)
					break;
			}
		}

		v11 = 0x633D5F1ull * v1;
		const uint32_t inner = ((v5 & w) - 45u * (v8 >> 7)) & 0xDFDFDFDFu;
		v12 = (0xFB8C4D96501ull * static_cast<uint64_t>(inner)) >> 24;

		if (v4)
			break;

		currentByte += 4;
		const uint64_t acc = v11 + v12;
		v1 = (acc >> 61) ^ acc;
	}

	// Bit position of v5's set high bit (terminator lane), /8 gives the byte lane.
	// v5 is never 0 when we exit (v4 != 0 implies a terminator lane was found).
	unsigned long bit = 0;
	for (uint32_t t = v5; t; t >>= 1) bit++;
	const int32_t v13 = static_cast<int32_t>(bit) - 1;

	const uint32_t idx = currentByte + static_cast<uint32_t>(v13 / 8);
	return v12 + v11 - 0xAE502812AA7333ull * static_cast<uint64_t>(idx);
}

// Build the v17 material GUID from a v12.2 texture path. Mirrors the verified
// reference rule: StringToGuid("material/" + path-with-forward-slashes + ".rpak").
static uint64_t MaterialPathToGuid(const char* const pSrcPath)
{
	std::string path(pSrcPath ? pSrcPath : "");
	for (char& c : path)
		if (c == '\\') c = '/';

	std::string full = "material/" + path + ".rpak";
	// pad so the hash's 4-byte over-read past the terminator is in-bounds.
	full.append(8, '\0');
	return RTech_StringToGuid(full.c_str());
}

// seqdesc field byte offsets within the 112B v160/v170 seqdesc (rseq v11).
// Identical to ConvertRMDL191To17's table (same struct).
constexpr size_t kSeq_szlabelindex          = 0;
constexpr size_t kSeq_szactivitynameindex   = 2;
constexpr size_t kSeq_activity              = 8;
constexpr size_t kSeq_actweight             = 10;
constexpr size_t kSeq_paramindex0           = 44;
constexpr size_t kSeq_paramindex1           = 46;
constexpr size_t kSeq_animindexindex        = 42;
constexpr size_t kSeq_weightlistindex       = 82;
constexpr size_t kSeq_groupsize0            = 84;
constexpr size_t kSeq_groupsize1            = 85;
constexpr size_t kSeq_eventindex            = 14;
constexpr size_t kSeq_autolayerindex        = 80;
constexpr size_t kSeq_iklockindex           = 90;
constexpr size_t kSeq_activitymodifierindex = 96;
constexpr size_t kSeq_weightFixupOffset     = 108;

constexpr size_t kNewSeqStride           = 112;       // v16/v17 seqdesc
constexpr int    kStudioAnimExternalFlag = 0x200000;  // cleared (data inline)

// v17 studiohdr bit 19: the model's sequences come from a virtual model. The client's
// model cache returns a null virtual model when it is clear, so the animrig groups are
// never bound and the model can only ever hold its bind pose.
constexpr int kStudioHdrUsesVirtualModel = 0x80000;

} // anonymous namespace

//
// ConvertRMDL122To17
// Full rebuild of a v12.2 ('IDST' v54, S10) .rmdl into the v17 (S21-client)
// layout. Emits a fresh 228B v170 studiohdr_t + every section in the v160/v170
// NEW shape, in the reference-verified order.
//
void ConvertRMDL122To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut, uint32_t vgDecompressedSize)
{
	const std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();
	printf("[v17/122] Converting '%s' from mdl_ v12.2 -> v17 (input %zu bytes)\n", rawModelName.c_str(), fileSize);

	const r5::v122::studiohdr_t* const oldHdr = reinterpret_cast<const r5::v122::studiohdr_t*>(pMDL);

	if (oldHdr->id != 'TSDI')
		printf("[v17/122]   WARNING: source magic 0x%X != 'IDST' -- not a v54 model, output may be invalid.\n", oldHdr->id);

	const int numbones        = oldHdr->numbones;
	const int numbodyparts    = oldHdr->numbodyparts;
	const int numtextures     = oldHdr->numtextures;
	const int numhitboxsets   = oldHdr->numhitboxsets;
	const int numlocalseq     = oldHdr->numlocalseq;
	const int numskinref      = oldHdr->numskinref;
	const int numskinfamilies = oldHdr->numskinfamilies;

	printf("[v17/122]   bones=%d bodyparts=%d textures=%d hitboxsets=%d localseq=%d skin=%dx%d attach=%d\n",
		numbones, numbodyparts, numtextures, numhitboxsets, numlocalseq,
		numskinref, numskinfamilies, oldHdr->numlocalattachments);

	// ---- LOUD GUARDS for features we cannot faithfully relocate.
	if (oldHdr->procBoneCount > 0)
		printf("[v17/122]   WARNING: '%s' has %d procedural (jiggle) bones -- proc-rule relocation NOT implemented; "
			"bones emitted without proc data. Verify in-game.\n", rawModelName.c_str(), oldHdr->procBoneCount);
	if (oldHdr->numikchains > 0)
		printf("[v17/122]   WARNING: '%s' has %d ikchains -- ikchain/link relocation NOT implemented; emitting empty. "
			"Verify in-game.\n", rawModelName.c_str(), oldHdr->numikchains);
	if (oldHdr->uiPanelCount > 0)
		printf("[v17/122]   WARNING: '%s' has %d uiPanel (RUI) meshes -- RUI relocation NOT implemented; dropping.\n",
			rawModelName.c_str(), oldHdr->uiPanelCount);
	if (oldHdr->numlocalnodes > 0)
		printf("[v17/122]   WARNING: '%s' has %d localnodes -- localnode relocation NOT implemented; dropping.\n",
			rawModelName.c_str(), oldHdr->numlocalnodes);

	// Resolve output path.
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

	// Output buffer. Generous upper bound: input + headroom for the split-bone
	// expansion (12+128 vs 180 shrinks, but we keep all sections) and string churn.
	const size_t outCap = fileSize + 0x10000 + static_cast<size_t>(numbones) * 256;
	std::unique_ptr<char[]> outMem(new char[outCap]{});
	OutBuf o{ outMem.get(), 0, outCap };

	// ---- Deferred string table: collect (fieldAbs, string) and emit one block at
	// the end (the reference v17 model places all name strings in a contiguous block after
	// the geometry). Dedup identical strings.
	// A string-pointer field at `fieldAbs` whose struct begins at `structBase`.
	// The resolved offset is written as (stringAbs - structBase). For HEADER
	// string fields, pass structBase = 0 (header base).
	struct StrRef { size_t structBase; size_t fieldAbs; std::string str; };
	std::vector<StrRef> strRefs;
	auto AddStr = [&](size_t structBase, size_t fieldAbs, const char* s) {
		strRefs.push_back({ structBase, fieldAbs, std::string(s ? s : "") });
	};

	// =====================================================================
	// 1) studiohdr_t (228B) -- reserve; fill scalar fields now, offsets as we go.
	// =====================================================================
	r5::v170::studiohdr_t* const nh = reinterpret_cast<r5::v170::studiohdr_t*>(o.base);
	o.cur = sizeof(r5::v170::studiohdr_t); // 228

	int newFlags = oldHdr->flags;
	if (numbones > 1 && (newFlags & STUDIOHDR_FLAGS_STATIC_PROP) == 0)
		newFlags |= kStudioHdrUsesVirtualModel;
	else
		newFlags &= ~kStudioHdrUsesVirtualModel;
	if (newFlags != oldHdr->flags)
		printf("[v17/122]   set virtual-model flag (bones=%d, flags 0x%08X -> 0x%08X)\n",
			numbones, oldHdr->flags, newFlags);
	nh->flags        = newFlags;
	nh->checksum     = oldHdr->checksum;
	nh->mass         = oldHdr->mass;
	nh->contents     = oldHdr->contents;
	nh->illumposition = oldHdr->illumposition;
	nh->hull_min     = oldHdr->hull_min;
	nh->hull_max     = oldHdr->hull_max;
	nh->view_bbmin   = oldHdr->view_bbmin;
	nh->view_bbmax   = oldHdr->view_bbmax;
	nh->surfacepropLookup = static_cast<uint8_t>(oldHdr->surfacepropLookup);
	nh->illumpositionattachmentindex = static_cast<uint8_t>(oldHdr->illumpositionattachmentindex);
	nh->boneCount    = static_cast<uint16_t>(numbones);
	nh->numlocalseq  = static_cast<uint16_t>(numlocalseq);
	nh->numlocalattachments = 0; // set only when attachment table is emitted
	nh->numlocalnodes = 0; // no localnode table emitted
	nh->numikchains  = 0; // guarded above
	nh->numtextures  = static_cast<uint16_t>(numtextures);
	nh->numskinref   = static_cast<uint16_t>(numskinref);
	nh->numskinfamilies = static_cast<uint16_t>(numskinfamilies);
	nh->numbodyparts = static_cast<uint16_t>(numbodyparts);
	nh->numhitboxsets = static_cast<uint8_t>(numhitboxsets);
	nh->uiPanelCount = 0; // guarded above
	nh->numlocalposeparameters = 0; // set only when poseparam table is emitted
	nh->numsrcbonetransform = 0; // set only when srcbonetransform table is emitted
	nh->fadeDistance = oldHdr->defaultFadeDist;
	nh->gatherSize   = oldHdr->gatherSize;
	nh->activitylistversion = static_cast<char>(oldHdr->activitylistversion);
	nh->meshCount    = static_cast<uint16_t>(oldHdr->vgMeshCount);
	nh->groupHeaderCount = 0; // set when the regenerated group header is emitted (sect 14)
	nh->lodCount     = static_cast<uint16_t>(oldHdr->numVGLods);
	nh->unk_E0       = 0;

	// =====================================================================
	// 2) sourceFilename (right after the header, like the reference model @228).
	// =====================================================================
	if (oldHdr->sourceFilenameOffset != 0 && oldHdr->boneindex > oldHdr->sourceFilenameOffset)
	{
		const int n = oldHdr->boneindex - oldHdr->sourceFilenameOffset;
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, sourceFilenameOffset), o.cur);
		memcpy(o.base + o.cur, pMDL + oldHdr->sourceFilenameOffset, n);
		o.cur += n;
		Align(o, 4);
	}

	// =====================================================================
	// 3) boneHdr array (12B each). Pose data is split out to boneData later.
	// =====================================================================
	Align(o, 2);
	const size_t boneHdrStart = o.cur;
	if (numbones > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, boneHdrOffset), boneHdrStart);
	for (int i = 0; i < numbones; i++)
	{
		const r5::v121::mstudiobone_t* const ob =
			reinterpret_cast<const r5::v121::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
		const size_t bhAbs = o.cur;
		r5::v160::mstudiobonehdr_t* const bh =
			reinterpret_cast<r5::v160::mstudiobonehdr_t*>(o.base + bhAbs);

		bh->contents          = ob->contents;
		bh->surfacepropLookup = static_cast<uint8_t>(ob->surfacepropLookup);
		bh->physicsbone       = static_cast<uint16_t>(ob->physicsbone);
		bh->surfacepropidx    = 0;
		bh->sznameindex       = 0;
		if (ob->surfacepropidx)
			AddStr(bhAbs, bhAbs + offsetof(r5::v160::mstudiobonehdr_t, surfacepropidx), ob->pszSurfaceProp());
		AddStr(bhAbs, bhAbs + offsetof(r5::v160::mstudiobonehdr_t, sznameindex), ob->pszName());

		o.cur += sizeof(r5::v160::mstudiobonehdr_t);
	}

	// =====================================================================
	// 4) hitboxsets (6B each) followed by their hitbox arrays (mstudiobbox_t 32B).
	// =====================================================================
	Align(o, 2);
	const size_t hitboxSetStart = o.cur;
	if (numhitboxsets > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, hitboxsetindex), hitboxSetStart);

	// emit the hitboxset headers contiguously first
	const r5::v8::mstudiohitboxset_t* const oldSets =
		reinterpret_cast<const r5::v8::mstudiohitboxset_t*>(pMDL + oldHdr->hitboxsetindex);
	std::vector<size_t> setAbs(numhitboxsets);
	for (int i = 0; i < numhitboxsets; i++)
	{
		setAbs[i] = o.cur;
		r5::v160::mstudiohitboxset_t* const ns =
			reinterpret_cast<r5::v160::mstudiohitboxset_t*>(o.base + o.cur);
		const r5::v8::mstudiohitboxset_t* const os = &oldSets[i];
		ns->sznameindex = 0;
		ns->numhitboxes = static_cast<uint16_t>(os->numhitboxes);
		ns->hitboxindex = 0;
		AddStr(setAbs[i], setAbs[i] + offsetof(r5::v160::mstudiohitboxset_t, sznameindex), STRING_FROM_IDX(os, os->sznameindex));
		o.cur += sizeof(r5::v160::mstudiohitboxset_t);
	}
	// then each set's hitbox array
	for (int i = 0; i < numhitboxsets; i++)
	{
		const r5::v8::mstudiohitboxset_t* const os = &oldSets[i];
		if (os->numhitboxes == 0)
			continue;
		Align(o, 2);
		const size_t hbStart = o.cur;
		SetSubOff(o, setAbs[i], setAbs[i] + offsetof(r5::v160::mstudiohitboxset_t, hitboxindex), hbStart);
		const r5::v8::mstudiobbox_t* const oldBoxes =
			reinterpret_cast<const r5::v8::mstudiobbox_t*>((const char*)os + os->hitboxindex);
		for (int j = 0; j < os->numhitboxes; j++)
		{
			const size_t hbAbs = o.cur;
			r5::v160::mstudiobbox_t* const nb =
				reinterpret_cast<r5::v160::mstudiobbox_t*>(o.base + hbAbs);
			const r5::v8::mstudiobbox_t* const obx = &oldBoxes[j];
			nb->bone   = static_cast<uint16_t>(obx->bone);
			nb->group  = static_cast<uint16_t>(obx->group);
			nb->bbmin  = obx->bbmin;
			nb->bbmax  = obx->bbmax;
			nb->szhitboxnameindex = 0;
			nb->hitdataGroupOffset = 0;
			if (obx->szhitboxnameindex)
				AddStr(hbAbs, hbAbs + offsetof(r5::v160::mstudiobbox_t, szhitboxnameindex),
					(const char*)obx + obx->szhitboxnameindex);
			o.cur += sizeof(r5::v160::mstudiobbox_t);
		}
	}

	// =====================================================================
	// 4b) attachments (v8 source field order is sznameindex/flags/localbone; target is sznameindex/localbone/flags)
	// =====================================================================
	Align(o, 2);
	{
		int numatt = oldHdr->numlocalattachments;
		const bool attValid = numatt > 0
			&& oldHdr->localattachmentindex > 0
			&& (static_cast<size_t>(oldHdr->localattachmentindex)
				+ static_cast<size_t>(numatt) * sizeof(r5::v8::mstudioattachment_t) <= fileSize);
		if (!attValid)
		{
			if (numatt > 0)
			{
				printf("[v17/122]   WARNING: '%s' attachment table invalid (count=%d index=%d) -- zeroing count.\n",
					rawModelName.c_str(), numatt, oldHdr->localattachmentindex);
				nh->numlocalattachments = 0;
				SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localattachmentindex), hitboxSetStart);
			}
		}
		else
		{
			if (numatt > 255)
			{
				printf("[v17/122]   WARNING: '%s' has %d attachments -- clamping to 255 (uint8_t).\n",
					rawModelName.c_str(), numatt);
				numatt = 255;
			}
			const size_t attStart = o.cur;
			SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localattachmentindex), attStart);
			const r5::v8::mstudioattachment_t* const oldAtt =
				reinterpret_cast<const r5::v8::mstudioattachment_t*>(pMDL + oldHdr->localattachmentindex);
			for (int i = 0; i < numatt; i++)
			{
				const size_t attAbs = o.cur;
				r5::v160::mstudioattachment_t* const na =
					reinterpret_cast<r5::v160::mstudioattachment_t*>(o.base + attAbs);
				memset(na, 0, sizeof(r5::v160::mstudioattachment_t));
				const r5::v8::mstudioattachment_t* const oa = &oldAtt[i];
				na->flags = oa->flags;
				na->local = oa->localmatrix;
				if (oa->localbone < 0 || oa->localbone >= numbones)
				{
					printf("[v17/122]   WARNING: '%s' attachment[%d] bad localbone=%d (numbones=%d) -- writing 0.\n",
						rawModelName.c_str(), i, oa->localbone, numbones);
					na->localbone = 0;
				}
				else
				{
					na->localbone = static_cast<uint16_t>(oa->localbone);
				}
				na->sznameindex = 0;
				AddStr(attAbs, attAbs + offsetof(r5::v160::mstudioattachment_t, sznameindex),
					oa->sznameindex == 0 ? "" : STRING_FROM_IDX(oa, oa->sznameindex));
				o.cur += sizeof(r5::v160::mstudioattachment_t);
			}
			nh->numlocalattachments = static_cast<uint8_t>(numatt);
		}
	}

	// =====================================================================
	// 5) bonetablebyname (numbones bytes).
	// =====================================================================
	Align(o, 2);
	if (numbones > 0 && oldHdr->bonetablebynameindex)
	{
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bonetablebynameindex), o.cur);
		memcpy(o.base + o.cur, pMDL + oldHdr->bonetablebynameindex, numbones);
		o.cur += numbones;
	}
	Align(o, 4);

	// =====================================================================
	// 6) SEQ region: seqdesc array (112B) + per-seq [weightlist][aii][animdesc].
	//    Reproduces the reference model layout exactly (see ConvertRMDL191To17).
	// =====================================================================
	const size_t seqArrStart = o.cur;
	if (numlocalseq > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localseqindex), seqArrStart);

	const r5::v8::mstudioseqdesc_t* const oldSeqs =
		reinterpret_cast<const r5::v8::mstudioseqdesc_t*>(pMDL + oldHdr->localseqindex);

	o.cur = seqArrStart + static_cast<size_t>(numlocalseq) * kNewSeqStride;
	bool sawSeqAnim = false;

	for (int i = 0; i < numlocalseq; i++)
	{
		const size_t newSeqOff = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		char* const  newSeq    = o.base + newSeqOff;
		const r5::v8::mstudioseqdesc_t* const os = &oldSeqs[i];

		// Copy the common 112B seqdesc body verbatim then fix self-relative fields.
		// v8 seqdesc and v160 seqdesc share field layout for the first 112B used here
		// (both are the Apex rseq-v11 era shape). We copy what fits and re-point the
		// string/array offsets below.
		memset(newSeq, 0, kNewSeqStride);
		newSeq[kSeq_groupsize0] = static_cast<char>(os->groupsize[0]);
		newSeq[kSeq_groupsize1] = static_cast<char>(os->groupsize[1]);
		WriteI32(newSeq, 4, (os->flags & ~kStudioAnimExternalFlag) | kStudioHdrUsesVirtualModel);
		WriteU16(newSeq, kSeq_paramindex0, 0xFFFF);
		WriteU16(newSeq, kSeq_paramindex1, 0xFFFF);
		*reinterpret_cast<float*>(newSeq + 64) = 0.2f;
		*reinterpret_cast<float*>(newSeq + 68) = 0.2f;
		// v17 packs activity as uint16; -1 is the "no activity" sentinel. 0 is a real
		// activity id and would let a placeholder sequence win an activity lookup.
		const uint16_t seqActivity =
			(os->szactivitynameindex != 0 && os->activity >= 0)
				? static_cast<uint16_t>(os->activity)
				: static_cast<uint16_t>(0xFFFF);
		WriteU16(newSeq, kSeq_activity,  seqActivity);
		WriteU16(newSeq, kSeq_actweight, static_cast<uint16_t>(os->actweight));

		// strings
		if (os->szlabelindex)
			AddStr(newSeqOff, newSeqOff + kSeq_szlabelindex, (const char*)os + os->szlabelindex);
		if (os->szactivitynameindex)
			AddStr(newSeqOff, newSeqOff + kSeq_szactivitynameindex, (const char*)os + os->szactivitynameindex);

		const int gs0 = os->groupsize[0], gs1 = os->groupsize[1];
		const int nanims = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;

		// weightlist: boneCount * 1.0f.
		Align(o, 4);
		const size_t weightAbs = o.cur;
		const int numWeights = numbones > 0 ? numbones : 1;
		for (int w = 0; w < numWeights; w++)
		{
			*reinterpret_cast<float*>(o.base + o.cur) = 1.0f;
			o.cur += sizeof(float);
		}
		WriteU16(newSeq, kSeq_weightlistindex, static_cast<uint16_t>(weightAbs - newSeqOff));

		// animindexindex array.
		Align(o, 4);
		const size_t arrAbs = o.cur;
		WriteU16(newSeq, kSeq_animindexindex, static_cast<uint16_t>(arrAbs - newSeqOff));
		uint16_t* const newArr = reinterpret_cast<uint16_t*>(o.base + arrAbs);
		o.cur += static_cast<size_t>(nanims) * sizeof(uint16_t);
		Align(o, 4);

		// count==0 sub-array offsets -> in-region (never dereferenced).
		WriteU16(newSeq, kSeq_eventindex,            static_cast<uint16_t>(weightAbs - newSeqOff));
		WriteU16(newSeq, kSeq_autolayerindex,        static_cast<uint16_t>(weightAbs - newSeqOff));
		WriteU16(newSeq, kSeq_iklockindex,           static_cast<uint16_t>(arrAbs - newSeqOff));
		WriteU16(newSeq, kSeq_activitymodifierindex, static_cast<uint16_t>(arrAbs - newSeqOff));

		// animdescs (40B each), degenerate inline (ref pose) -- the per-frame RLE of
		// an animated v12.2 source is external; warn and emit valid empty anim.
		const int* const oldBlend = os->animindexindex
			? reinterpret_cast<const int*>((const char*)os + os->animindexindex) : nullptr;

		for (int a = 0; a < nanims; a++)
		{
			Align(o, 4);
			const size_t animAbs = o.cur;
			r5::v160::mstudioanimdesc_t* const ad =
				reinterpret_cast<r5::v160::mstudioanimdesc_t*>(o.base + animAbs);
			memset(ad, 0, sizeof(r5::v160::mstudioanimdesc_t));

			if (oldBlend)
			{
				const r5::v121::mstudioanimdesc_t* const oa =
					reinterpret_cast<const r5::v121::mstudioanimdesc_t*>((const char*)os + oldBlend[a]);
				ad->fps       = oa->fps;
				ad->flags     = oa->flags & ~kStudioAnimExternalFlag;
				ad->numframes = oa->numframes;
				if (oa->sznameindex)
					AddStr(animAbs, animAbs + offsetof(r5::v160::mstudioanimdesc_t, sznameindex), (const char*)oa + oa->sznameindex);
				if (oa->numframes > 1 || oa->sectionframes != 0)
					sawSeqAnim = true;
			}
			else
			{
				ad->fps = 30.0f;
				ad->numframes = 1;
			}
			ad->animindex = 0; // patched to EOF-relative degenerate block below.

			newArr[a] = static_cast<uint16_t>(animAbs - newSeqOff);
			o.cur += sizeof(r5::v160::mstudioanimdesc_t);
		}

		Align(o, 4);
		WriteU16(newSeq, kSeq_weightFixupOffset, static_cast<uint16_t>(o.cur - newSeqOff));
	}

	if (sawSeqAnim)
		printf("[v17/122]   WARNING: '%s' has sequence(s) with real animation (numframes>1 / sections). The inline "
			"per-frame RLE is NOT reproduced; supply animation as an external aseq v11 via R5-AnimConv. Emitted "
			"reference-shaped degenerate inline (structurally valid).\n", rawModelName.c_str());

	// =====================================================================
	// 7) bodyparts (16B) -> models (10B) -> meshes (20B).
	// =====================================================================
	Align(o, 4);
	const size_t bodypartStart = o.cur;
	if (numbodyparts > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bodypartindex), bodypartStart);

	const mstudiobodyparts_t* const oldBodyParts =
		reinterpret_cast<const mstudiobodyparts_t*>(pMDL + oldHdr->bodypartindex);

	// pass 1: emit all bodypart headers contiguously.
	std::vector<size_t> bpAbs(numbodyparts);
	for (int i = 0; i < numbodyparts; i++)
	{
		bpAbs[i] = o.cur;
		const mstudiobodyparts_t* const obp = &oldBodyParts[i];
		r5::v160::mstudiobodyparts_t* const nbp =
			reinterpret_cast<r5::v160::mstudiobodyparts_t*>(o.base + o.cur);
		nbp->sznameindex = 0;
		nbp->modelindex  = 0;
		nbp->base        = obp->base;
		nbp->nummodels   = obp->nummodels;
		nbp->meshOffset  = 0;
		AddStr(bpAbs[i], bpAbs[i] + offsetof(r5::v160::mstudiobodyparts_t, sznameindex), obp->pszName());
		o.cur += sizeof(r5::v160::mstudiobodyparts_t);
	}
	// pass 2: per bodypart, models then meshes.
	for (int i = 0; i < numbodyparts; i++)
	{
		const mstudiobodyparts_t* const obp = &oldBodyParts[i];
		SetSubOff(o, bpAbs[i], bpAbs[i] + offsetof(r5::v160::mstudiobodyparts_t, modelindex), o.cur);

		const r5::v121::mstudiomodel_t* const oldModels =
			reinterpret_cast<const r5::v121::mstudiomodel_t*>((const char*)obp + obp->modelindex);

		std::vector<size_t> modelAbs(obp->nummodels);
		for (int j = 0; j < obp->nummodels; j++)
		{
			modelAbs[j] = o.cur;
			const r5::v121::mstudiomodel_t* const om = &oldModels[j];
			r5::v160::mstudiomodel_t* const nm =
				reinterpret_cast<r5::v160::mstudiomodel_t*>(o.base + o.cur);
			nm->unkStringOffset = 0;
			nm->meshCountTotal  = static_cast<uint16_t>(om->nummeshes);
			nm->meshCountBase   = static_cast<uint16_t>(om->nummeshes);
			nm->meshCountBlend  = 0;
			nm->meshOffset      = 0;
			AddStr(modelAbs[j], modelAbs[j] + offsetof(r5::v160::mstudiomodel_t, unkStringOffset), "");
			o.cur += sizeof(r5::v160::mstudiomodel_t);
		}
		for (int j = 0; j < obp->nummodels; j++)
		{
			const r5::v121::mstudiomodel_t* const om = &oldModels[j];
			if (om->nummeshes == 0)
				continue;
			SetSubOff(o, modelAbs[j], modelAbs[j] + offsetof(r5::v160::mstudiomodel_t, meshOffset), o.cur);
			const r5::v121::mstudiomesh_t* const oldMeshes =
				reinterpret_cast<const r5::v121::mstudiomesh_t*>((const char*)om + om->meshindex);
			for (int k = 0; k < om->nummeshes; k++)
			{
				const r5::v121::mstudiomesh_t* const ome = &oldMeshes[k];
				r5::v160::mstudiomesh_t* const nme =
					reinterpret_cast<r5::v160::mstudiomesh_t*>(o.base + o.cur);
				nme->material = static_cast<uint16_t>(ome->material);
				nme->meshid   = static_cast<uint16_t>(ome->meshid);
				memset(nme->unk_4, 0, sizeof(nme->unk_4));
				nme->center   = ome->center;
				o.cur += sizeof(r5::v160::mstudiomesh_t);
			}
		}
	}

	// =====================================================================
	// 8) textures (8B GUID each). The load-bearing path->GUID transform.
	// =====================================================================
	Align(o, 8);
	const size_t texStart = o.cur;
	if (numtextures > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, textureindex), texStart);

	const r5::v8::mstudiotexture_t* const oldTex =
		reinterpret_cast<const r5::v8::mstudiotexture_t*>(pMDL + oldHdr->textureindex);
	static bool s_loggedSample = false;
	for (int i = 0; i < numtextures; i++)
	{
		const r5::v8::mstudiotexture_t* const ot = &oldTex[i];
		const char* const path = ot->pszName();
		const uint64_t guid = MaterialPathToGuid(path);
		*reinterpret_cast<uint64_t*>(o.base + o.cur) = guid;
		if (!s_loggedSample)
		{
			printf("[v17/122]   path->guid sample: 'material/%s.rpak' -> 0x%016llX\n",
				path, static_cast<unsigned long long>(guid));
			s_loggedSample = true;
		}
		o.cur += sizeof(uint64_t);
	}

	// =====================================================================
	// 9) skin table (numskinref*numskinfamilies * uint16) + skin names.
	// =====================================================================
	Align(o, 2);
	const size_t skinStart = o.cur;
	SetHdrOff(o, offsetof(r5::v170::studiohdr_t, skinindex), skinStart);
	{
		const int skinRefBytes = sizeof(int16_t) * numskinref * numskinfamilies;
		memcpy(o.base + o.cur, pMDL + oldHdr->skinindex, skinRefBytes);
		o.cur += skinRefBytes;
		Align(o, 4);
		// skin names follow (skin 0 unnamed) -- v8 stored int offsets; reference v17 model keeps
		// the same uint16 name pointers. Copy the family names into the string block.
		const char* oldSkinNames = reinterpret_cast<const char*>(pMDL + oldHdr->skinindex) + skinRefBytes;
		// align as the source did (4)
		const int* oldNames = reinterpret_cast<const int*>(((reinterpret_cast<uintptr_t>(oldSkinNames) + 3) & ~uintptr_t(3)));
		for (int i = 0; i < numskinfamilies - 1; i++)
		{
			const size_t nameFieldAbs = o.cur;
			*reinterpret_cast<uint16_t*>(o.base + nameFieldAbs) = 0;
			AddStr(skinStart, nameFieldAbs, STRING_FROM_IDX(pMDL, oldNames[i]));
			o.cur += sizeof(uint16_t);
		}
		Align(o, 4);
	}

	// =====================================================================
	// 10) keyvalue block.
	// =====================================================================
	{
		const std::string kv = "mdlkeyvalue{prop_data{base \"\"}}\n";
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, keyvalueindex), o.cur);
		memcpy(o.base + o.cur, kv.c_str(), kv.length() + 1);
		o.cur += kv.length() + 1;
		Align(o, 4);
	}

	// =====================================================================
	// 11) poseparams (mstudioposeparamdesc_t) -- before strings.
	// =====================================================================
	if (oldHdr->numlocalposeparameters > 0 && oldHdr->localposeparamindex > 0)
	{
		Align(o, 4);
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localposeparamindex), o.cur);
		const mstudioposeparamdesc_t* const oldPP =
			reinterpret_cast<const mstudioposeparamdesc_t*>(pMDL + oldHdr->localposeparamindex);
		for (int i = 0; i < oldHdr->numlocalposeparameters; i++)
		{
			const size_t ppAbs = o.cur;
			r5::v160::mstudioposeparamdesc_t* const np =
				reinterpret_cast<r5::v160::mstudioposeparamdesc_t*>(o.base + ppAbs);
			const mstudioposeparamdesc_t* const op = &oldPP[i];
			np->flags = static_cast<uint16_t>(op->flags);
			np->start = op->start;
			np->end   = op->end;
			np->loop  = op->loop;
			np->sznameindex = 0;
			AddStr(ppAbs, ppAbs + offsetof(r5::v160::mstudioposeparamdesc_t, sznameindex), STRING_FROM_IDX(op, op->sznameindex));
			o.cur += sizeof(r5::v160::mstudioposeparamdesc_t);
		}
		nh->numlocalposeparameters = static_cast<uint16_t>(oldHdr->numlocalposeparameters);
		Align(o, 4);
	}

	// =====================================================================
	// 12) srcbonetransforms (mstudiosrcbonetransform_t).
	// =====================================================================
	if (oldHdr->numsrcbonetransform > 0 && oldHdr->srcbonetransformindex > 0)
	{
		Align(o, 4);
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, srcbonetransformindex), o.cur);
		const mstudiosrcbonetransform_t* const oldSBT =
			reinterpret_cast<const mstudiosrcbonetransform_t*>(pMDL + oldHdr->srcbonetransformindex);
		for (int i = 0; i < oldHdr->numsrcbonetransform; i++)
		{
			const size_t sbtAbs = o.cur;
			mstudiosrcbonetransform_t* const ns =
				reinterpret_cast<mstudiosrcbonetransform_t*>(o.base + sbtAbs);
			const mstudiosrcbonetransform_t* const op = &oldSBT[i];
			*ns = *op;
			ns->sznameindex = 0;
			AddStr(sbtAbs, sbtAbs + offsetof(mstudiosrcbonetransform_t, sznameindex), STRING_FROM_IDX(op, op->sznameindex));
			o.cur += sizeof(mstudiosrcbonetransform_t);
		}
		nh->numsrcbonetransform = static_cast<uint16_t>(oldHdr->numsrcbonetransform);
		Align(o, 4);
	}

	// =====================================================================
	// 13) model name / surfaceprop strings + the deferred string block.
	// =====================================================================
	// model name (mdl/..rmdl) and the header surfaceprop go in the block too.
	{
		std::string modelName = STRING_FROM_IDX(pMDL, oldHdr->sznameindex);
		for (char& c : modelName) if (c == '\\') c = '/';
		if (modelName.rfind("mdl/", 0) != 0)
			modelName = "mdl/" + modelName;
		if (EndsWith(modelName, ".mdl"))
			modelName = modelName.substr(0, modelName.length() - 4) + ".rmdl";

		// copy into hdr name[33] (truncated) too.
		memset(nh->name, 0, sizeof(nh->name));
		memcpy(nh->name, modelName.c_str(), min(modelName.length(), sizeof(nh->name) - 1));

		AddStr(0, offsetof(r5::v170::studiohdr_t, sznameindex), modelName.c_str());
		AddStr(0, offsetof(r5::v170::studiohdr_t, surfacepropindex), STRING_FROM_IDX(pMDL, oldHdr->surfacepropindex));
	}

	// Emit the string block (dedup). Then resolve every recorded fieldAbs.
	Align(o, 1);
	const size_t strBlockStart = o.cur;
	std::vector<std::pair<std::string, size_t>> strTable; // string -> abs offset
	// Engine FIX_OFFSET: odd o decodes as (o-1)*16; every string must start even.
	auto EmitStr = [&](const std::string& s) -> size_t {
		for (const auto& e : strTable)
			if (e.first == s) return e.second;
		if (o.cur & 1)
		{
			o.base[o.cur] = '\0';
			o.cur++;
		}
		const size_t at = o.cur;
		memcpy(o.base + o.cur, s.c_str(), s.length() + 1);
		o.cur += s.length() + 1;
		strTable.emplace_back(s, at);
		return at;
	};
	for (const StrRef& r : strRefs)
	{
		const size_t at = EmitStr(r.str);
		const size_t delta = at - r.structBase;
		if ((delta & 1) != 0)
			printf("[v17/122]   WARNING: '%s' odd string delta for '%s' (struct base odd?)\n",
				rawModelName.c_str(), r.str.c_str());
		if (delta > 0xFFFE)
			printf("[v17/122]   WARNING: '%s' string delta 0x%zX exceeds u16 for '%s'\n",
				rawModelName.c_str(), delta, r.str.c_str());
		WriteU16(o.base, r.fieldAbs, static_cast<uint16_t>(delta));
	}
	(void)strBlockStart;
	Align(o, 4);

	// =====================================================================
	// 14) LOD thresholds + group headers (copied from v12.2; same shapes).
	// =====================================================================
	if (oldHdr->numVGLods > 0 && oldHdr->vgLODOffset)
	{
		Align(o, 4);
		SetHdrOffFieldRel(o, offsetof(r5::v170::studiohdr_t, lodOffset), o.cur);
		// Copy the model's LOD switch distances. Zero thresholds make every LOD win, so the lowest always draws.
		const char* const vgLodArray = reinterpret_cast<const char*>(&oldHdr->vgLODOffset) + oldHdr->vgLODOffset;
		const size_t vgLodArrayFileOff = static_cast<size_t>(vgLodArray - pMDL);
		for (int i = 0; i < oldHdr->numVGLods; i++)
		{
			float switchPoint = 0.0f;
			const size_t entryOff = static_cast<size_t>(i) * 16 + 8;
			if (vgLodArrayFileOff + entryOff + sizeof(float) <= fileSize)
				switchPoint = *reinterpret_cast<const float*>(vgLodArray + entryOff);
			else
				printf("[v17/122]   WARNING: '%s' LOD[%d] switchPoint read out of bounds -- writing 0.0f fallback.\n",
					rawModelName.c_str(), i);
			*reinterpret_cast<float*>(o.base + o.cur) = switchPoint;
			o.cur += sizeof(float);
		}
		Align(o, 4);
	}
	// group header (studio_hw_groupdata) -- REGENERATED, not copied. The v12.2 array's
	// dataOffsets index the OLD VG layout; we rebuilt the VG (ConvertVGData_Rev2To17)
	// into ONE rev4 group holding all LODs, so we emit ONE descriptor for it. The
	// on-disk entry is 16 BYTES (the v160 struct's `int dataCompression` is misleading;
	// on disk those 4 bytes are u8 dataCompression + u8 lodIndex + u8 lodCount + u8 lodMap),
	// verified vs a reference v17 model (fence=1 group dec=vgSize lodCount=2 lodMap=0b11).
	// dataCompression=0 (the .vg streams raw); dataOffset=0 (single group); sizes=vgSize.
	if (vgDecompressedSize > 0)
	{
		Align(o, 4);
		SetHdrOffFieldRel(o, offsetof(r5::v170::studiohdr_t, groupHeaderOffset), o.cur);
		const int lc = oldHdr->numVGLods > 0 ? oldHdr->numVGLods : 1;
		char* const g = o.base + o.cur;
		*reinterpret_cast<int32_t*>(g + 0) = 0;                                   // dataOffset
		*reinterpret_cast<int32_t*>(g + 4) = static_cast<int32_t>(vgDecompressedSize); // dataSizeCompressed (raw stream)
		*reinterpret_cast<int32_t*>(g + 8) = static_cast<int32_t>(vgDecompressedSize); // dataSizeDecompressed
		g[12] = 0;                                                               // dataCompression = none
		g[13] = 0;                                                               // lodIndex
		g[14] = static_cast<uint8_t>(lc);                                        // lodCount
		g[15] = static_cast<uint8_t>(lc >= 8 ? 0xFF : ((1u << lc) - 1));         // lodMap
		o.cur += 16;
		nh->groupHeaderCount = 1;
		Align(o, 4);
	}

	// =====================================================================
	// 15) boneData array (128B each).
	// =====================================================================
	Align(o, 16);
	const size_t boneDataStart = o.cur;
	if (numbones > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, boneDataOffset), boneDataStart);
	for (int i = 0; i < numbones; i++)
	{
		const r5::v121::mstudiobone_t* const ob =
			reinterpret_cast<const r5::v121::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
		r5::v160::mstudiobonedata_t* const bd =
			reinterpret_cast<r5::v160::mstudiobonedata_t*>(o.base + o.cur);
		memset(bd, 0, sizeof(r5::v160::mstudiobonedata_t));
		bd->poseToBone     = ob->poseToBone;
		bd->qAlignment     = ob->qAlignment;
		bd->pos            = ob->pos;
		bd->quat           = ob->quat;
		bd->rot            = ob->rot;
		bd->scale          = ob->scale;
		bd->parent         = static_cast<short>(ob->parent);
		bd->flags          = ob->flags;
		bd->collisionIndex = ob->collisionIndex;
		bd->proctype       = static_cast<uint8_t>(ob->proctype);
		bd->procindex      = 0; // proc rule not relocated (guarded above)
		o.cur += sizeof(r5::v160::mstudiobonedata_t);
	}

	// =====================================================================
	// 16) collision / bvh blob (verbatim; internal indices are blob-relative).
	// =====================================================================
	if (oldHdr->bvhOffset)
	{
		Align(o, 16);
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bvhOffset), o.cur);
		// size = from bvhOffset to the next thing after it in the source. The v12.2
		// collision blob ends where the VG/group data begins; use vgLODOffset / file
		// end as the upper bound (the blob's own indices are self-contained).
		size_t blobEnd = fileSize;
		// header fields that may point just past the bvh blob:
		auto consider = [&](int off, size_t base) { if (off && base + off > (size_t)oldHdr->bvhOffset && base + off < blobEnd) blobEnd = base + off; };
		consider(oldHdr->vgMeshOffset, offsetof(r5::v122::studiohdr_t, vgMeshOffset));
		consider(oldHdr->boneStateOffset, offsetof(r5::v122::studiohdr_t, boneStateOffset));
		consider(oldHdr->vgLODOffset, offsetof(r5::v122::studiohdr_t, vgLODOffset));
		const size_t blobSize = blobEnd - (size_t)oldHdr->bvhOffset;
		memcpy(o.base + o.cur, pMDL + oldHdr->bvhOffset, blobSize);
		o.cur += blobSize;
	}

	// ---- Patch every degenerate animdesc.animindex to EOF-relative (zero-length).
	const size_t finalSize = o.cur;
	for (int i = 0; i < numlocalseq; i++)
	{
		const size_t newSeqOff = seqArrStart + static_cast<size_t>(i) * kNewSeqStride;
		char* const  newSeq    = o.base + newSeqOff;
		const uint16_t arrRel  = ReadU16(newSeq, kSeq_animindexindex);
		if (arrRel == 0) continue;
		const int gs0 = (uint8_t)newSeq[kSeq_groupsize0], gs1 = (uint8_t)newSeq[kSeq_groupsize1];
		const int nanims = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;
		const size_t arrAbs = newSeqOff + arrRel;
		for (int a = 0; a < nanims; a++)
		{
			const uint16_t animRel = ReadU16(o.base, arrAbs + static_cast<size_t>(a) * 2);
			if (animRel == 0) continue;
			const size_t animAbs = newSeqOff + animRel;
			WriteI32(o.base, animAbs + offsetof(r5::v160::mstudioanimdesc_t, animindex),
				static_cast<int32_t>(finalSize - animAbs));
		}
	}

	std::ofstream ofs(rmdlPath, std::ios::out | std::ios::binary);
	ofs.write(o.base, static_cast<std::streamsize>(finalSize));
	ofs.close();

	printf("[v17/122]   wrote %zu bytes -> %s\n", finalSize, rmdlPath.c_str());
}

//
// ConvertVGData_Rev2To17
// Rebuild a v12.2 ('0tVG' rev2) vertex-group container into the rev4 raw format
// used by v16+/v19.1/v17 (no magic). One vertex group, ALL source LODs preserved.
// The per-vertex packed format and the uint16 index buffer are IDENTICAL across
// revs, so the vertex/index/extraBoneWeight bytes copy over verbatim; only the
// header/LOD/mesh structures are rebuilt with relative offsets, the VTX strip
// topology is dropped (rev4 uses the index buffer directly), and the blendShape
// fields are zeroed.
//
size_t ConvertVGData_Rev2To17(char* inputBuf, const size_t inputSize, const std::string& filePath, const std::string& pathOut)
{
	const std::string rawName = std::filesystem::path(filePath).filename().u8string();

	const vg::rev2::VertexGroupHeader_t* const srcHdr =
		reinterpret_cast<const vg::rev2::VertexGroupHeader_t*>(inputBuf);

	if (srcHdr->id != 'GVt0')
	{
		printf("[v17/vg]   WARNING: '%s' is not a rev2 '0tVG' container (id=0x%X) -- skipping VG.\n",
			rawName.c_str(), srcHdr->id);
		return 0;
	}

	const int lodCount = srcHdr->lodCount;
	printf("[v17/vg] Converting '%s' rev2 -> rev4 (lodCount=%d, %zu bytes)\n", rawName.c_str(), lodCount, inputSize);

	// ---- Pass 1: enumerate LODs/meshes, total counts and buffer sizes.
	struct MeshRec
	{
		const vg::rev2::MeshHeader_t* src;
		const char* base; // absolute base of this mesh header (for its relative offsets)
		uint32_t vertBufSize, indexBytes, extraBytes;
		int lod;
	};
	std::vector<MeshRec> meshes;
	std::vector<int> lodMeshCount(lodCount, 0);

	size_t totalVert = 0, totalIndex = 0, totalExtra = 0;

	for (int i = 0; i < lodCount; i++)
	{
		const size_t lodOff = 0x18 + static_cast<size_t>(i) * sizeof(vg::rev2::ModelLODHeader_t) + srcHdr->lodOffset;
		const vg::rev2::ModelLODHeader_t* const lod =
			reinterpret_cast<const vg::rev2::ModelLODHeader_t*>(inputBuf + lodOff);
		lodMeshCount[i] = lod->meshCount;
		for (int j = 0; j < lod->meshCount; j++)
		{
			const size_t meshOff = lodOff + offsetof(vg::rev2::ModelLODHeader_t, meshOffset) +
				static_cast<size_t>(lod->meshOffset) + static_cast<size_t>(j) * sizeof(vg::rev2::MeshHeader_t);
			const vg::rev2::MeshHeader_t* const m =
				reinterpret_cast<const vg::rev2::MeshHeader_t*>(inputBuf + meshOff);

			MeshRec rec{};
			rec.src        = m;
			rec.base       = reinterpret_cast<const char*>(m);
			rec.vertBufSize = static_cast<uint32_t>(m->vertBufferSize);
			rec.indexBytes  = static_cast<uint32_t>(m->indexCount) * sizeof(uint16_t);
			rec.extraBytes  = static_cast<uint32_t>(m->externalWeightSize);
			rec.lod        = i;
			meshes.push_back(rec);

			totalVert  += rec.vertBufSize;
			totalIndex += rec.indexBytes;
			totalExtra += rec.extraBytes;
		}
	}

	const size_t totalMeshes = meshes.size();

	// ---- rev4 layout -- MUST match the genuine S21 streamed-VG layout exactly, or the
	// engine's "LoadHardwareDataForStreamedModelJob" reads a wrong mesh header and creates
	// a GPU vertex/index buffer larger than the streamed source (amdxx64 over-read AV).
	// Verified vs genuine S21 v17 .vg (district extract: box_small_02,
	// canyonlands_zone_sign_03b, citymodern_planter_*): per-LOD INTERLEAVED blocks --
	//   [VGHdr 8][lod array 8*lodCount]
	//   then for each LOD (16-aligned): [mesh headers meshCount*48]
	//        then for each mesh: [16-align -> index][16-align -> vertex][16-align -> extra]
	// and ModelLODHeader.meshIndex is 0 (LOD-LOCAL), NOT a cumulative global index.
	// (The earlier separated layout -- all headers, then all indices, then all vertices,
	// with cumulative meshIndex -- crashed every multi-LOD/multi-mesh model on stream.)
	auto Align16 = [](size_t c) -> size_t { return (c + 15) & ~static_cast<size_t>(15); };

	// generous upper bound: data + 16B slack per buffer (3 per mesh) + per-LOD align.
	const size_t outCap = sizeof(vg::rev4::VertexGroupHeader_t)
		+ static_cast<size_t>(lodCount) * sizeof(vg::rev4::ModelLODHeader_t)
		+ totalMeshes * sizeof(vg::rev4::MeshHeader_t)
		+ totalIndex + totalVert + totalExtra
		+ 16u * (totalMeshes * 3u + static_cast<size_t>(lodCount) + 4u) + 64u;

	std::unique_ptr<char[]> outMem(new char[outCap]{});
	char* const out = outMem.get();

	// ---- Group header.
	vg::rev4::VertexGroupHeader_t* const gh = reinterpret_cast<vg::rev4::VertexGroupHeader_t*>(out);
	gh->lodIndex   = 0;
	gh->lodCount   = static_cast<uint8_t>(lodCount);
	gh->groupIndex = 0;
	gh->lodMap     = static_cast<uint8_t>((lodCount >= 8) ? 0xFF : ((1u << lodCount) - 1));
	const size_t lodArrOff = sizeof(vg::rev4::VertexGroupHeader_t); // 8
	gh->lodOffset  = static_cast<uint32_t>(lodArrOff - offsetof(vg::rev4::VertexGroupHeader_t, lodOffset));

	// ---- Per-LOD interleaved blocks.
	size_t cur = lodArrOff + static_cast<size_t>(lodCount) * sizeof(vg::rev4::ModelLODHeader_t);
	size_t globalMeshIdx = 0;

	for (int i = 0; i < lodCount; i++)
	{
		cur = Align16(cur); // each LOD block (its mesh-header array) starts 16-aligned.

		const size_t lodAbs = lodArrOff + static_cast<size_t>(i) * sizeof(vg::rev4::ModelLODHeader_t);
		vg::rev4::ModelLODHeader_t* const ol = reinterpret_cast<vg::rev4::ModelLODHeader_t*>(out + lodAbs);
		ol->meshCount  = static_cast<uint8_t>(lodMeshCount[i]);
		ol->meshIndex  = 0; // LOD-local (genuine S21 convention), NOT globalMeshIdx.
		ol->lodLevel   = static_cast<uint8_t>(i);
		ol->groupIndex = 0;

		const size_t meshArrAbs = cur;
		ol->meshOffset = static_cast<uint32_t>(meshArrAbs - (lodAbs + offsetof(vg::rev4::ModelLODHeader_t, meshOffset)));
		cur += static_cast<size_t>(lodMeshCount[i]) * sizeof(vg::rev4::MeshHeader_t); // reserve mesh headers

		for (int j = 0; j < lodMeshCount[i]; j++)
		{
			const MeshRec& rec = meshes[globalMeshIdx];
			const size_t meshAbs = meshArrAbs + static_cast<size_t>(j) * sizeof(vg::rev4::MeshHeader_t);
			vg::rev4::MeshHeader_t* const nm = reinterpret_cast<vg::rev4::MeshHeader_t*>(out + meshAbs);

			nm->flags         = static_cast<uint64_t>(rec.src->flags);
			nm->vertCount     = static_cast<uint32_t>(rec.src->vertCount);
			nm->vertCacheSize = static_cast<uint16_t>(rec.src->vertCacheSize);

			// vertBoneCount = max bones referenced by this mesh's vertices. The rev4
			// reader (ConvertVGData_160) reads this straight into the strip header. For
			// the packed Apex vertex format, the bone count lives in the packed-bones
			// field; we derive the max across the mesh (>=1).
			uint16_t maxBones = 1;
			if (rec.src->flags & VERTEX_HAS_WEIGHT_BONES)
			{
				const char* const vtx = rec.base + offsetof(vg::rev2::MeshHeader_t, vertOffset) + rec.src->vertOffset;
				const size_t stride = rec.src->vertCacheSize;
				if (stride > 0)
				{
					for (uint32_t v = 0; v < rec.src->vertCount; v++)
					{
						// packed bones live just after the packed position (8B) + packed
						// weights (4B); numbones is the 4th byte of mstudiopackedbones_t.
						// Conservative scan: find the highest numbones+1 in the mesh.
						const vg::mstudiopackedbones_t* const pb =
							reinterpret_cast<const vg::mstudiopackedbones_t*>(vtx + static_cast<size_t>(v) * stride + 12);
						const uint16_t nb = static_cast<uint16_t>(pb->numbones + 1);
						if (nb > maxBones) maxBones = nb;
					}
				}
			}
			nm->vertBoneCount = maxBones;

			nm->indexCount = static_cast<uint32_t>(rec.src->indexCount) & 0x0FFFFFFFu;
			nm->indexType  = static_cast<uint32_t>(rec.src->indexType) & 0xFu;

			// index buffer (16-aligned), immediately after this LOD's mesh headers/prev mesh.
			cur = Align16(cur);
			nm->indexOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, indexOffset)));
			memcpy(out + cur,
				rec.base + offsetof(vg::rev2::MeshHeader_t, indexOffset) + rec.src->indexOffset, rec.indexBytes);
			cur += rec.indexBytes;

			// vertex buffer (16-aligned).
			cur = Align16(cur);
			nm->vertOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, vertOffset)));
			nm->vertBufferSize = rec.vertBufSize;
			memcpy(out + cur,
				rec.base + offsetof(vg::rev2::MeshHeader_t, vertOffset) + rec.src->vertOffset, rec.vertBufSize);
			cur += rec.vertBufSize;

			if (rec.extraBytes > 0)
			{
				cur = Align16(cur);
				nm->extraBoneWeightOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, extraBoneWeightOffset)));
				nm->extraBoneWeightSize = rec.extraBytes;
				memcpy(out + cur,
					rec.base + offsetof(vg::rev2::MeshHeader_t, externalWeightOffset) + rec.src->externalWeightOffset, rec.extraBytes);
				cur += rec.extraBytes;
			}
			else
			{
				nm->extraBoneWeightOffset = 0;
				nm->extraBoneWeightSize = 0;
			}

			// rev4 uses the index buffer directly; no VTX strips. blendShape unused.
			nm->blendShapeVertOffset = 0;
			nm->blendShapeVertBufferSize = 0;

			globalMeshIdx++;
		}
	}

	const size_t outSize = cur;

	std::ofstream ofs(pathOut, std::ios::out | std::ios::binary);
	ofs.write(out, static_cast<std::streamsize>(outSize));
	ofs.close();

	printf("[v17/vg]   wrote %zu bytes (%zu meshes, vert=%zu index=%zu extra=%zu) -> %s\n",
		outSize, totalMeshes, totalVert, totalIndex, totalExtra, pathOut.c_str());

	return outSize;
}

//
// ConvertPhy_122To17
// Reshape the S10 .phy 20-byte IVPS header to the v17 compact 4-byte header
// (phyheader_v16_t). The collision body (IVPS surface/solid data, with
// blob-relative internal offsets) copies over verbatim; only the header shrinks
// 20B->4B and the propertiesOffset is rebased by -16. Verified header-exact
// against genuine S21 v17 .phy reference models (the body float deltas vs the reference are a
// re-bake, not a layout change).
//
void ConvertPhy_122To17(char* phyBuf, const size_t phySize, const std::string& filePath, const std::string& pathOut)
{
	const std::string rawName = std::filesystem::path(filePath).filename().u8string();

	if (phySize < sizeof(collision::phyheader_t))
	{
		printf("[v17/phy]   WARNING: '%s' too small (%zu bytes) -- skipping.\n", rawName.c_str(), phySize);
		return;
	}

	const collision::phyheader_t* const oldHdr =
		reinterpret_cast<const collision::phyheader_t*>(phyBuf);

	if (oldHdr->size != 0x14)
	{
		printf("[v17/phy]   WARNING: '%s' header size=%d (expected 0x14 = 20B S10 IVPS) -- passing through unchanged.\n",
			rawName.c_str(), oldHdr->size);
		std::ofstream ofs(pathOut, std::ios::out | std::ios::binary);
		ofs.write(phyBuf, static_cast<std::streamsize>(phySize));
		ofs.close();
		return;
	}

	const size_t bodySize = phySize - sizeof(collision::phyheader_t); // drop the 20B header
	const size_t outSize  = sizeof(collision::phyheader_v16_t) + bodySize;

	std::unique_ptr<char[]> outMem(new char[outSize]{});
	char* const out = outMem.get();

	collision::phyheader_v16_t* const nh = reinterpret_cast<collision::phyheader_v16_t*>(out);
	nh->solidCount       = static_cast<uint16_t>(oldHdr->solidCount);
	// propertiesOffset rebased: it indexed from file base past the 20B header; the
	// header is now 16B smaller (20 -> 4), so the offset shrinks by 16.
	nh->propertiesOffset = static_cast<uint16_t>(oldHdr->propertiesOffset - 16);

	memcpy(out + sizeof(collision::phyheader_v16_t),
		phyBuf + sizeof(collision::phyheader_t), bodySize);

	std::ofstream ofs(pathOut, std::ios::out | std::ios::binary);
	ofs.write(out, static_cast<std::streamsize>(outSize));
	ofs.close();

	printf("[v17/phy]   '%s' solidCount=%d propOff %d->%d, wrote %zu bytes -> %s\n",
		rawName.c_str(), oldHdr->solidCount, oldHdr->propertiesOffset, nh->propertiesOffset,
		outSize, pathOut.c_str());
}
