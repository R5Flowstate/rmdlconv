// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/studio_r5_v16.h>
#include <studio/versions.h>
#include <core/utils.h>
#include <collision/phy_parser.h>

/*
	Type:    RMDL / MDL
	Sources: v8  (Apex S0-S6 / S3-legacy 'TSDI' studio 54)
	         v49 (Portal 2 / Source MDL -- chains through existing ConvertMDL49To54)
	Target:  17  (S21 shipping CLIENT mdl_ v17)

	Sibling of ConvertRMDL122To17. Same v170 rebuild contract (228B hdr, split
	bones, material path->StringToGuid, reference seq region, rev4 VG group header).

	v8 differences vs v12.2 that this path must handle:
	  * studiohdr has no vgLODOffset/numVGLods/vgMeshCount -- LOD thresholds and
	    meshCount come from the sibling .vg (or CreateVGFile from .vtx/.vvd).
	  * bones are r5::v8::mstudiobone_t (fat, collisionIndex int).
	  * bodyparts/models/meshes are the fat v8 shapes (name[64] models).
	  * collision blob uses 32B mstudiocollheader_t -- S21 wants the 40B v120
	    shape; ExpandCollisionV8ToV170 inserts the surfaceProp fields + rebases.
	  * VG from CreateVGFile / early S3 is rev1 '0tVG' (global buffers), not rev2.
*/

namespace {

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

inline void SetHdrOff(OutBuf& o, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) = static_cast<uint16_t>(targetAbs);
}

inline void SetHdrOffFieldRel(OutBuf& o, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) = static_cast<uint16_t>(targetAbs - fieldAbs);
}

inline void SetSubOff(OutBuf& o, size_t structBase, size_t fieldAbs, size_t targetAbs)
{
	*reinterpret_cast<uint16_t*>(o.base + fieldAbs) =
		static_cast<uint16_t>(targetAbs - structBase);
}

inline void WriteU16(char* p, size_t off, uint16_t v) { *reinterpret_cast<uint16_t*>(p + off) = v; }
inline uint16_t ReadU16(const char* p, size_t off) { return *reinterpret_cast<const uint16_t*>(p + off); }
inline void WriteI32(char* p, size_t off, int32_t v) { *reinterpret_cast<int32_t*>(p + off) = v; }

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

	unsigned long bit = 0;
	for (uint32_t t = v5; t; t >>= 1) bit++;
	const int32_t v13 = static_cast<int32_t>(bit) - 1;

	const uint32_t idx = currentByte + static_cast<uint32_t>(v13 / 8);
	return v12 + v11 - 0xAE502812AA7333ull * static_cast<uint64_t>(idx);
}

static uint64_t MaterialPathToGuid(const char* const pSrcPath)
{
	std::string path(pSrcPath ? pSrcPath : "");
	for (char& c : path)
		if (c == '\\') c = '/';

	std::string full = "material/" + path + ".rpak";
	full.append(8, '\0');
	return RTech_StringToGuid(full.c_str());
}

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
constexpr size_t kNewSeqStride              = 112;
constexpr int    kStudioAnimExternalFlag   = 0x200000;

// v17 studiohdr bit 19: the model's sequences come from a virtual model. The client's
// model cache returns a null virtual model when it is clear, so the animrig groups are
// never bound and the model can only ever hold its bind pose.
constexpr int kStudioHdrUsesVirtualModel = 0x80000;

// Expand legacy v8 coll (32B headers) -> v120/v170 (40B headers).
// Returns bytes written to dst, or 0 on skip/error.
static size_t ExpandCollisionV8ToV170(const char* const src, const size_t srcSize,
	char* const dst, const size_t dstCap, const char* const modelName)
{
	if (srcSize < sizeof(r5::v8::mstudiocollmodel_t))
		return 0;

	const r5::v8::mstudiocollmodel_t* const cm =
		reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(src);
	const int headerCount = cm->headerCount;
	if (headerCount <= 0 || headerCount > 64)
	{
		printf("[v17/8]   WARNING: '%s' coll headerCount=%d invalid -- skipping bvh.\n",
			modelName, headerCount);
		return 0;
	}

	const size_t hdrEndV8  = sizeof(r5::v8::mstudiocollmodel_t) + static_cast<size_t>(headerCount) * sizeof(r5::v8::mstudiocollheader_t);
	const size_t hdrEndV120 = sizeof(r5::v8::mstudiocollmodel_t) + static_cast<size_t>(headerCount) * sizeof(r5::v120::mstudiocollheader_t);
	const int    delta      = static_cast<int>(hdrEndV120 - hdrEndV8); // +8 * headerCount

	// Already modern (40B headers)? surfacePropsIndex sits right after headers.
	if (cm->surfacePropsIndex == static_cast<int>(hdrEndV120) ||
		(cm->surfacePropsIndex > static_cast<int>(hdrEndV8) &&
			(cm->surfacePropsIndex - static_cast<int>(sizeof(r5::v8::mstudiocollmodel_t))) % 40 == 0 &&
			(cm->surfacePropsIndex - static_cast<int>(sizeof(r5::v8::mstudiocollmodel_t))) / 40 == headerCount))
	{
		if (srcSize > dstCap) return 0;
		memcpy(dst, src, srcSize);
		return srcSize;
	}

	// Confirm v8 layout: surfacePropsIndex should equal hdrEndV8 (common) or be nearby.
	if (cm->surfacePropsIndex < static_cast<int>(hdrEndV8))
	{
		printf("[v17/8]   WARNING: '%s' coll layout not recognised (surfProps=%d hdrEndV8=%zu) -- raw copy.\n",
			modelName, cm->surfacePropsIndex, hdrEndV8);
		if (srcSize > dstCap) return 0;
		memcpy(dst, src, srcSize);
		return srcSize;
	}

	const size_t tailSize = srcSize - hdrEndV8;
	const size_t outSize  = hdrEndV120 + tailSize;
	if (outSize > dstCap)
	{
		printf("[v17/8]   WARNING: '%s' coll expand overflow (%zu > %zu).\n", modelName, outSize, dstCap);
		return 0;
	}

	memset(dst, 0, outSize);

	r5::v8::mstudiocollmodel_t* const ncm = reinterpret_cast<r5::v8::mstudiocollmodel_t*>(dst);
	ncm->headerCount = headerCount;
	auto rebase = [&](int off) -> int {
		if (off <= 0) return off;
		return off >= static_cast<int>(hdrEndV8) ? off + delta : off;
	};
	ncm->contentMasksIndex = rebase(cm->contentMasksIndex);
	ncm->surfacePropsIndex = rebase(cm->surfacePropsIndex);
	ncm->surfaceNamesIndex = rebase(cm->surfaceNamesIndex);

	const r5::v8::mstudiocollheader_t* const oldH =
		reinterpret_cast<const r5::v8::mstudiocollheader_t*>(src + sizeof(r5::v8::mstudiocollmodel_t));
	r5::v120::mstudiocollheader_t* const newH =
		reinterpret_cast<r5::v120::mstudiocollheader_t*>(dst + sizeof(r5::v8::mstudiocollmodel_t));

	for (int i = 0; i < headerCount; i++)
	{
		newH[i].unk                  = oldH[i].unk;
		newH[i].bvhNodeIndex         = rebase(oldH[i].bvhNodeIndex);
		newH[i].vertIndex            = rebase(oldH[i].vertIndex);
		newH[i].bvhLeafIndex         = rebase(oldH[i].bvhLeafIndex);
		newH[i].surfacePropDataIndex = 0;
		newH[i].surfacePropArrayCount = 0;
		newH[i].surfacePropCount     = 0;
		newH[i].padding_maybe        = 0;
		newH[i].origin[0]            = oldH[i].origin[0];
		newH[i].origin[1]            = oldH[i].origin[1];
		newH[i].origin[2]            = oldH[i].origin[2];
		newH[i].scale                = oldH[i].scale;
	}

	memcpy(dst + hdrEndV120, src + hdrEndV8, tailSize);

	printf("[v17/8]   coll expand: %d part(s), %zu -> %zu bytes (+%d header pad)\n",
		headerCount, srcSize, outSize, delta);
	return outSize;
}

static int CountMeshesV8(const r5::v8::studiohdr_t* const hdr, const char* const pMDL)
{
	int total = 0;
	const r5::v8::mstudiobodyparts_t* const bps =
		reinterpret_cast<const r5::v8::mstudiobodyparts_t*>(pMDL + hdr->bodypartindex);
	for (int i = 0; i < hdr->numbodyparts; i++)
	{
		const r5::v8::mstudiomodel_t* const models =
			reinterpret_cast<const r5::v8::mstudiomodel_t*>((const char*)&bps[i] + bps[i].modelindex);
		for (int j = 0; j < bps[i].nummodels; j++)
			total += models[j].nummeshes;
	}
	return total;
}

} // anonymous namespace

//
// ConvertRMDL8To17
// Full rebuild of a legacy S3/v8 ('TSDI' v54) .rmdl into the v17 client layout.
//
void ConvertRMDL8To17(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut,
	uint32_t vgDecompressedSize, int vgLodCount, const float* vgLodSwitchPoints,
	const uint8_t* boneStates, int boneStateCount)
{
	const std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();
	printf("[v17/8] Converting '%s' from rmdl v8 -> v17 (input %zu bytes, vg=%u lods=%d)\n",
		rawModelName.c_str(), fileSize, vgDecompressedSize, vgLodCount);

	const r5::v8::studiohdr_t* const oldHdr = reinterpret_cast<const r5::v8::studiohdr_t*>(pMDL);

	if (oldHdr->id != 'TSDI' && oldHdr->id != 'IDST')
		printf("[v17/8]   WARNING: source magic 0x%X -- expected TSDI/IDST, output may be invalid.\n", oldHdr->id);

	const int numbones        = oldHdr->numbones;
	const int numbodyparts    = oldHdr->numbodyparts;
	const int numtextures     = oldHdr->numtextures;
	const int numhitboxsets   = oldHdr->numhitboxsets;
	const int numlocalseq     = oldHdr->numlocalseq;
	const int numskinref      = oldHdr->numskinref;
	const int numskinfamilies = oldHdr->numskinfamilies;
	const int meshCount       = CountMeshesV8(oldHdr, pMDL);
	const int lodCount        = vgLodCount > 0 ? vgLodCount : 1;

	printf("[v17/8]   bones=%d bodyparts=%d textures=%d hitboxsets=%d localseq=%d skin=%dx%d meshes=%d attach=%d linearbone=%s\n",
		numbones, numbodyparts, numtextures, numhitboxsets, numlocalseq,
		numskinref, numskinfamilies, meshCount, oldHdr->numlocalattachments,
		numbones > 0 ? "yes" : "no");

	if (oldHdr->procBoneCount > 0)
		printf("[v17/8]   WARNING: '%s' has %d procedural (jiggle) bones -- proc-rule relocation NOT implemented.\n",
			rawModelName.c_str(), oldHdr->procBoneCount);
	if (oldHdr->numikchains > 0)
		printf("[v17/8]   WARNING: '%s' has %d ikchains -- ikchain/link relocation NOT implemented.\n",
			rawModelName.c_str(), oldHdr->numikchains);
	if (oldHdr->uiPanelCount > 0)
		printf("[v17/8]   WARNING: '%s' has %d uiPanel (RUI) meshes -- dropping.\n",
			rawModelName.c_str(), oldHdr->uiPanelCount);
	if (oldHdr->numlocalnodes > 0)
		printf("[v17/8]   WARNING: '%s' has %d localnodes -- localnode relocation NOT implemented; dropping.\n",
			rawModelName.c_str(), oldHdr->numlocalnodes);

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

	const size_t outCap = fileSize + 0x20000 + static_cast<size_t>(numbones) * 256 + 0x10000;
	std::unique_ptr<char[]> outMem(new char[outCap]{});
	OutBuf o{ outMem.get(), 0, outCap };

	struct StrRef { size_t structBase; size_t fieldAbs; std::string str; };
	std::vector<StrRef> strRefs;
	auto AddStr = [&](size_t structBase, size_t fieldAbs, const char* s) {
		strRefs.push_back({ structBase, fieldAbs, std::string(s ? s : "") });
	};

	// 1) studiohdr_t (228B)
	r5::v170::studiohdr_t* const nh = reinterpret_cast<r5::v170::studiohdr_t*>(o.base);
	o.cur = sizeof(r5::v170::studiohdr_t);

	int newFlags = oldHdr->flags;
	if (numbones > 1 && (newFlags & STUDIOHDR_FLAGS_STATIC_PROP) == 0)
		newFlags |= kStudioHdrUsesVirtualModel;
	else
		newFlags &= ~kStudioHdrUsesVirtualModel;
	if (newFlags != oldHdr->flags)
		printf("[v17/8]   set virtual-model flag (bones=%d, flags 0x%08X -> 0x%08X)\n",
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
	nh->numikchains  = 0;
	nh->numtextures  = static_cast<uint16_t>(numtextures);
	nh->numskinref   = static_cast<uint16_t>(numskinref);
	nh->numskinfamilies = static_cast<uint16_t>(numskinfamilies);
	nh->numbodyparts = static_cast<uint16_t>(numbodyparts);
	nh->numhitboxsets = static_cast<uint8_t>(numhitboxsets);
	nh->uiPanelCount = 0;
	nh->numlocalposeparameters = 0; // set only when poseparam table is emitted
	nh->numsrcbonetransform = 0; // set only when srcbonetransform table is emitted
	nh->fadeDistance = oldHdr->defaultFadeDist;
	nh->gatherSize   = oldHdr->gatherSize;
	nh->activitylistversion = static_cast<char>(oldHdr->activitylistversion);
	nh->meshCount    = static_cast<uint16_t>(meshCount);
	nh->groupHeaderCount = 0;
	nh->lodCount     = static_cast<uint16_t>(lodCount);
	nh->unk_E0       = 0;

	// 2) sourceFilename
	if (oldHdr->sourceFilenameOffset != 0 && oldHdr->boneindex > oldHdr->sourceFilenameOffset)
	{
		const int n = oldHdr->boneindex - oldHdr->sourceFilenameOffset;
		if (n > 0 && n < 4096 && static_cast<size_t>(oldHdr->sourceFilenameOffset) + n <= fileSize)
		{
			SetHdrOff(o, offsetof(r5::v170::studiohdr_t, sourceFilenameOffset), o.cur);
			memcpy(o.base + o.cur, pMDL + oldHdr->sourceFilenameOffset, n);
			o.cur += n;
			Align(o, 4);
		}
	}

	// 3) boneHdr (12B)
	Align(o, 2);
	const size_t boneHdrStart = o.cur;
	if (numbones > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, boneHdrOffset), boneHdrStart);
	for (int i = 0; i < numbones; i++)
	{
		const r5::v8::mstudiobone_t* const ob =
			reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
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

	// 4) hitboxsets
	Align(o, 2);
	const size_t hitboxSetStart = o.cur;
	if (numhitboxsets > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, hitboxsetindex), hitboxSetStart);

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

	// 4b) attachments (v8 source field order is sznameindex/flags/localbone; target is sznameindex/localbone/flags)
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
				printf("[v17/8]   WARNING: '%s' attachment table invalid (count=%d index=%d) -- zeroing count.\n",
					rawModelName.c_str(), numatt, oldHdr->localattachmentindex);
				nh->numlocalattachments = 0;
				SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localattachmentindex), hitboxSetStart);
			}
		}
		else
		{
			if (numatt > 255)
			{
				printf("[v17/8]   WARNING: '%s' has %d attachments -- clamping to 255 (uint8_t).\n",
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
					printf("[v17/8]   WARNING: '%s' attachment[%d] bad localbone=%d (numbones=%d) -- writing 0.\n",
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

	// 5) bonetablebyname
	Align(o, 2);
	if (numbones > 0 && oldHdr->bonetablebynameindex)
	{
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bonetablebynameindex), o.cur);
		memcpy(o.base + o.cur, pMDL + oldHdr->bonetablebynameindex, numbones);
		o.cur += numbones;
	}
	Align(o, 4);

	// 6) SEQ region
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

		memset(newSeq, 0, kNewSeqStride);
		newSeq[kSeq_groupsize0] = static_cast<char>(os->groupsize[0]);
		newSeq[kSeq_groupsize1] = static_cast<char>(os->groupsize[1]);
		WriteI32(newSeq, 4, (os->flags & ~kStudioAnimExternalFlag) | kStudioHdrUsesVirtualModel);
		// Placeholder seqs must not claim pose slots 0/1. 0 is a real pose index;
		// the S21 pose-param guard then zeros every blend and the gun stays on ref.
		WriteU16(newSeq, kSeq_paramindex0, 0xFFFF);
		WriteU16(newSeq, kSeq_paramindex1, 0xFFFF);
		// The placeholder fade is 0.2/0.2. Leave 0 and transitions snap.
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

		if (os->szlabelindex)
			AddStr(newSeqOff, newSeqOff + kSeq_szlabelindex, (const char*)os + os->szlabelindex);
		if (os->szactivitynameindex)
			AddStr(newSeqOff, newSeqOff + kSeq_szactivitynameindex, (const char*)os + os->szactivitynameindex);

		const int gs0 = os->groupsize[0], gs1 = os->groupsize[1];
		const int nanims = (gs0 * gs1) > 0 ? (gs0 * gs1) : 1;

		Align(o, 4);
		const size_t weightAbs = o.cur;
		const int numWeights = numbones > 0 ? numbones : 1;
		for (int w = 0; w < numWeights; w++)
		{
			*reinterpret_cast<float*>(o.base + o.cur) = 1.0f;
			o.cur += sizeof(float);
		}
		WriteU16(newSeq, kSeq_weightlistindex, static_cast<uint16_t>(weightAbs - newSeqOff));

		Align(o, 4);
		const size_t arrAbs = o.cur;
		WriteU16(newSeq, kSeq_animindexindex, static_cast<uint16_t>(arrAbs - newSeqOff));
		uint16_t* const newArr = reinterpret_cast<uint16_t*>(o.base + arrAbs);
		o.cur += static_cast<size_t>(nanims) * sizeof(uint16_t);
		Align(o, 4);

		WriteU16(newSeq, kSeq_eventindex,            static_cast<uint16_t>(weightAbs - newSeqOff));
		WriteU16(newSeq, kSeq_autolayerindex,        static_cast<uint16_t>(weightAbs - newSeqOff));
		WriteU16(newSeq, kSeq_iklockindex,           static_cast<uint16_t>(arrAbs - newSeqOff));
		WriteU16(newSeq, kSeq_activitymodifierindex, static_cast<uint16_t>(arrAbs - newSeqOff));

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
				const r5::v8::mstudioanimdesc_t* const oa =
					reinterpret_cast<const r5::v8::mstudioanimdesc_t*>((const char*)os + oldBlend[a]);
				ad->fps       = oa->fps;
				ad->flags     = oa->flags & ~kStudioAnimExternalFlag;
				ad->numframes = oa->numframes;
				if (oa->sznameindex)
					AddStr(animAbs, animAbs + offsetof(r5::v160::mstudioanimdesc_t, sznameindex),
						(const char*)oa + oa->sznameindex);
				if (oa->numframes > 1 || oa->sectionframes != 0)
					sawSeqAnim = true;
			}
			else
			{
				ad->fps = 30.0f;
				ad->numframes = 1;
			}
			ad->animindex = 0;

			newArr[a] = static_cast<uint16_t>(animAbs - newSeqOff);
			o.cur += sizeof(r5::v160::mstudioanimdesc_t);
		}

		Align(o, 4);
		WriteU16(newSeq, kSeq_weightFixupOffset, static_cast<uint16_t>(o.cur - newSeqOff));
	}

	if (sawSeqAnim)
		printf("[v17/8]   WARNING: '%s' has sequence(s) with real animation -- emit degenerate inline; "
			"supply aseq v11 via R5-AnimConv for animated assets.\n", rawModelName.c_str());

	// 7) bodyparts / models / meshes
	Align(o, 4);
	const size_t bodypartStart = o.cur;
	if (numbodyparts > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bodypartindex), bodypartStart);

	const r5::v8::mstudiobodyparts_t* const oldBodyParts =
		reinterpret_cast<const r5::v8::mstudiobodyparts_t*>(pMDL + oldHdr->bodypartindex);

	std::vector<size_t> bpAbs(numbodyparts);
	for (int i = 0; i < numbodyparts; i++)
	{
		bpAbs[i] = o.cur;
		const r5::v8::mstudiobodyparts_t* const obp = &oldBodyParts[i];
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
	for (int i = 0; i < numbodyparts; i++)
	{
		const r5::v8::mstudiobodyparts_t* const obp = &oldBodyParts[i];
		SetSubOff(o, bpAbs[i], bpAbs[i] + offsetof(r5::v160::mstudiobodyparts_t, modelindex), o.cur);

		const r5::v8::mstudiomodel_t* const oldModels =
			reinterpret_cast<const r5::v8::mstudiomodel_t*>((const char*)obp + obp->modelindex);

		std::vector<size_t> modelAbs(obp->nummodels);
		for (int j = 0; j < obp->nummodels; j++)
		{
			modelAbs[j] = o.cur;
			const r5::v8::mstudiomodel_t* const om = &oldModels[j];
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
			const r5::v8::mstudiomodel_t* const om = &oldModels[j];
			if (om->nummeshes == 0)
				continue;
			SetSubOff(o, modelAbs[j], modelAbs[j] + offsetof(r5::v160::mstudiomodel_t, meshOffset), o.cur);
			const r5::v8::mstudiomesh_t* const oldMeshes =
				reinterpret_cast<const r5::v8::mstudiomesh_t*>((const char*)om + om->meshindex);
			for (int k = 0; k < om->nummeshes; k++)
			{
				const r5::v8::mstudiomesh_t* const ome = &oldMeshes[k];
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

	// 8) textures -> GUID
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
		// v8 texture may already carry a GUID in textureGuid if loaded from rpak;
		// prefer path string when present (non-empty), else keep existing guid.
		const char* const path = ot->pszName();
		uint64_t guid = 0;
		if (path && path[0] && path[0] != '\0' &&
			!(path[0] == '\0' || (static_cast<unsigned char>(path[0]) < 0x20 && path[1] == '\0')))
		{
			// If path looks like a real path (printable), hash it.
			bool printable = true;
			for (const char* c = path; *c; ++c)
			{
				if (static_cast<unsigned char>(*c) < 0x20 && *c != '\t') { printable = false; break; }
			}
			if (printable)
				guid = MaterialPathToGuid(path);
			else
				guid = ot->textureGuid;
		}
		else if (ot->textureGuid != 0)
		{
			guid = ot->textureGuid;
		}
		else
		{
			guid = MaterialPathToGuid(path);
		}

		*reinterpret_cast<uint64_t*>(o.base + o.cur) = guid;
		if (!s_loggedSample)
		{
			printf("[v17/8]   path->guid sample: '%s' -> 0x%016llX\n",
				path ? path : "(null)", static_cast<unsigned long long>(guid));
			s_loggedSample = true;
		}
		o.cur += sizeof(uint64_t);
	}

	// 9) skin table
	Align(o, 2);
	const size_t skinStart = o.cur;
	SetHdrOff(o, offsetof(r5::v170::studiohdr_t, skinindex), skinStart);
	{
		const int skinRefBytes = sizeof(int16_t) * numskinref * numskinfamilies;
		if (oldHdr->skinindex > 0 && static_cast<size_t>(oldHdr->skinindex) + skinRefBytes <= fileSize)
		{
			memcpy(o.base + o.cur, pMDL + oldHdr->skinindex, skinRefBytes);
			o.cur += skinRefBytes;
			Align(o, 4);
			if (numskinfamilies > 1)
			{
				const char* oldSkinNames = reinterpret_cast<const char*>(pMDL + oldHdr->skinindex) + skinRefBytes;
				const int* oldNames = reinterpret_cast<const int*>(((reinterpret_cast<uintptr_t>(oldSkinNames) + 3) & ~uintptr_t(3)));
				for (int i = 0; i < numskinfamilies - 1; i++)
				{
					const size_t nameFieldAbs = o.cur;
					*reinterpret_cast<uint16_t*>(o.base + nameFieldAbs) = 0;
					if (static_cast<const char*>(static_cast<const void*>(&oldNames[i])) < pMDL + fileSize)
						AddStr(skinStart, nameFieldAbs, STRING_FROM_IDX(pMDL, oldNames[i]));
					o.cur += sizeof(uint16_t);
				}
				Align(o, 4);
			}
		}
	}

	// 10) keyvalue
	{
		const std::string kv = "mdlkeyvalue{prop_data{base \"\"}}\n";
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, keyvalueindex), o.cur);
		memcpy(o.base + o.cur, kv.c_str(), kv.length() + 1);
		o.cur += kv.length() + 1;
		Align(o, 4);
	}

	// 11) poseparams
	if (oldHdr->numlocalposeparameters > 0 && oldHdr->localposeparamindex > 0)
	{
		Align(o, 4);
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, localposeparamindex), o.cur);
		const r5::v8::mstudioposeparamdesc_t* const oldPP =
			reinterpret_cast<const r5::v8::mstudioposeparamdesc_t*>(pMDL + oldHdr->localposeparamindex);
		for (int i = 0; i < oldHdr->numlocalposeparameters; i++)
		{
			const size_t ppAbs = o.cur;
			r5::v160::mstudioposeparamdesc_t* const np =
				reinterpret_cast<r5::v160::mstudioposeparamdesc_t*>(o.base + ppAbs);
			const r5::v8::mstudioposeparamdesc_t* const op = &oldPP[i];
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

	// 12) srcbonetransforms
	if (oldHdr->numsrcbonetransform > 0 && oldHdr->srcbonetransformindex > 0)
	{
		Align(o, 4);
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, srcbonetransformindex), o.cur);
		const r5::v8::mstudiosrcbonetransform_t* const oldSBT =
			reinterpret_cast<const r5::v8::mstudiosrcbonetransform_t*>(pMDL + oldHdr->srcbonetransformindex);
		for (int i = 0; i < oldHdr->numsrcbonetransform; i++)
		{
			const size_t sbtAbs = o.cur;
			mstudiosrcbonetransform_t* const ns =
				reinterpret_cast<mstudiosrcbonetransform_t*>(o.base + sbtAbs);
			const r5::v8::mstudiosrcbonetransform_t* const op = &oldSBT[i];
			ns->pretransform  = op->pretransform;
			ns->posttransform = op->posttransform;
			ns->sznameindex   = 0;
			AddStr(sbtAbs, sbtAbs + offsetof(mstudiosrcbonetransform_t, sznameindex), STRING_FROM_IDX(op, op->sznameindex));
			o.cur += sizeof(mstudiosrcbonetransform_t);
		}
		nh->numsrcbonetransform = static_cast<uint16_t>(oldHdr->numsrcbonetransform);
		Align(o, 4);
	}

	// 13) model name / surfaceprop + string block
	{
		std::string modelName = STRING_FROM_IDX(pMDL, oldHdr->sznameindex);
		if (modelName.empty() && oldHdr->name[0])
			modelName = oldHdr->name;
		for (char& c : modelName) if (c == '\\') c = '/';
		if (modelName.rfind("mdl/", 0) != 0)
			modelName = "mdl/" + modelName;
		if (EndsWith(modelName, ".mdl"))
			modelName = modelName.substr(0, modelName.length() - 4) + ".rmdl";
		else if (!EndsWith(modelName, ".rmdl"))
			modelName += ".rmdl";

		memset(nh->name, 0, sizeof(nh->name));
		memcpy(nh->name, modelName.c_str(), min(modelName.length(), sizeof(nh->name) - 1));

		AddStr(0, offsetof(r5::v170::studiohdr_t, sznameindex), modelName.c_str());
		if (oldHdr->surfacepropindex)
			AddStr(0, offsetof(r5::v170::studiohdr_t, surfacepropindex), STRING_FROM_IDX(pMDL, oldHdr->surfacepropindex));
	}

	Align(o, 1);
	const size_t strBlockStart = o.cur;
	std::vector<std::pair<std::string, size_t>> strTable;
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
			printf("[v17/8]   WARNING: '%s' odd string delta for '%s' (struct base odd?)\n",
				rawModelName.c_str(), r.str.c_str());
		if (delta > 0xFFFE)
			printf("[v17/8]   WARNING: '%s' string delta 0x%zX exceeds u16 for '%s'\n",
				rawModelName.c_str(), delta, r.str.c_str());
		WriteU16(o.base, r.fieldAbs, static_cast<uint16_t>(delta));
	}
	(void)strBlockStart;
	Align(o, 4);

	// 14) LOD thresholds + group header
	{
		Align(o, 4);
		SetHdrOffFieldRel(o, offsetof(r5::v170::studiohdr_t, lodOffset), o.cur);
		for (int i = 0; i < lodCount; i++)
		{
			float switchPoint = 0.0f;
			if (vgLodSwitchPoints && i < vgLodCount)
				switchPoint = vgLodSwitchPoints[i];
			*reinterpret_cast<float*>(o.base + o.cur) = switchPoint;
			o.cur += sizeof(float);
		}
		Align(o, 4);
	}
	if (vgDecompressedSize > 0)
	{
		Align(o, 4);
		SetHdrOffFieldRel(o, offsetof(r5::v170::studiohdr_t, groupHeaderOffset), o.cur);
		const int lc = lodCount;
		char* const g = o.base + o.cur;
		*reinterpret_cast<int32_t*>(g + 0) = 0;
		*reinterpret_cast<int32_t*>(g + 4) = static_cast<int32_t>(vgDecompressedSize);
		*reinterpret_cast<int32_t*>(g + 8) = static_cast<int32_t>(vgDecompressedSize);
		g[12] = 0;
		g[13] = 0;
		g[14] = static_cast<uint8_t>(lc);
		g[15] = static_cast<uint8_t>(lc >= 8 ? 0xFF : ((1u << lc) - 1));
		o.cur += 16;
		nh->groupHeaderCount = 1;
		Align(o, 4);
	}

	// Bone-state table: rev1 VG bone remap buffer relocated into the studiohdr.
	// pBoneStates is field-relative -- use SetHdrOffFieldRel, not SetHdrOff.
	Align(o, 2);
	if (boneStateCount > 0 && boneStates)
	{
		const size_t boneStateStart = o.cur;
		SetHdrOffFieldRel(o, offsetof(r5::v170::studiohdr_t, boneStateOffset), boneStateStart);
		memcpy(o.base + boneStateStart, boneStates, static_cast<size_t>(boneStateCount));
		o.cur += static_cast<size_t>(boneStateCount);
		nh->boneStateCount = static_cast<uint16_t>(boneStateCount);
		Align(o, 4);
	}

	// 15) boneData (128B)
	Align(o, 16);
	const size_t boneDataStart = o.cur;
	if (numbones > 0)
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, boneDataOffset), boneDataStart);
	for (int i = 0; i < numbones; i++)
	{
		const r5::v8::mstudiobone_t* const ob =
			reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
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
		bd->collisionIndex = static_cast<uint8_t>(ob->collisionIndex);
		bd->proctype       = static_cast<uint8_t>(ob->proctype);
		bd->procindex      = 0;
		o.cur += sizeof(r5::v160::mstudiobonedata_t);
	}

	// 15b) linearbone
	// linearboneindex is header-relative; sub-offsets are struct-relative to the linearbone base.
	// v16/v17 parent is int16; no scale/qalignment arrays (v19.1+ only).
	if (numbones > 0)
	{
		int lbBones = numbones;
		if (lbBones > 0xFFFF)
		{
			printf("[v17/8]   WARNING: '%s' numbones=%d exceeds u16 -- clamping linearbone count.\n",
				rawModelName.c_str(), numbones);
			lbBones = 0xFFFF;
		}

		Align(o, 16);
		const size_t lbAbs = o.cur;
		SetHdrOff(o, offsetof(r5::v170::studiohdr_t, linearboneindex), lbAbs);

		r5::v160::mstudiolinearbone_t* const lb =
			reinterpret_cast<r5::v160::mstudiolinearbone_t*>(o.base + lbAbs);
		memset(lb, 0, sizeof(r5::v160::mstudiolinearbone_t));
		o.cur = lbAbs + sizeof(r5::v160::mstudiolinearbone_t);
		lb->numbones = static_cast<uint16_t>(lbBones);

		auto setLbSub = [&](uint16_t& field, const char* arrayName) {
			const size_t arrayBase = o.cur;
			const size_t delta = arrayBase - lbAbs;
			if ((delta & 1) != 0 || delta > 0xFFFE)
				printf("[v17/8]   WARNING: '%s' linearbone %s sub-offset 0x%zX invalid.\n",
					rawModelName.c_str(), arrayName, delta);
			field = static_cast<uint16_t>(delta);
		};

		// flags (int)
		Align(o, 4);
		setLbSub(lb->flagsindex, "flags");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<int*>(o.base + o.cur) = ob->flags;
			o.cur += sizeof(int);
		}

		// parent (int16)
		Align(o, 4);
		setLbSub(lb->parentindex, "parent");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<int16_t*>(o.base + o.cur) = static_cast<int16_t>(ob->parent);
			o.cur += sizeof(int16_t);
		}

		// pos (Vector)
		Align(o, 4);
		setLbSub(lb->posindex, "pos");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<Vector*>(o.base + o.cur) = ob->pos;
			o.cur += sizeof(Vector);
		}

		// quat (Quaternion)
		Align(o, 16);
		setLbSub(lb->quatindex, "quat");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<Quaternion*>(o.base + o.cur) = ob->quat;
			o.cur += sizeof(Quaternion);
		}

		// rot (RadianEuler)
		Align(o, 4);
		setLbSub(lb->rotindex, "rot");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<RadianEuler*>(o.base + o.cur) = ob->rot;
			o.cur += sizeof(RadianEuler);
		}

		// posetobone (matrix3x4_t)
		Align(o, 16);
		setLbSub(lb->posetoboneindex, "posetobone");
		for (int i = 0; i < lbBones; i++)
		{
			const r5::v8::mstudiobone_t* const ob =
				reinterpret_cast<const r5::v8::mstudiobone_t*>(pMDL + oldHdr->boneindex) + i;
			*reinterpret_cast<matrix3x4_t*>(o.base + o.cur) = ob->poseToBone;
			o.cur += sizeof(matrix3x4_t);
		}

		Align(o, 4);
	}

	// 16) collision / bvh (expand v8 32B headers -> v120 40B if needed)
	if (oldHdr->bvhOffset > 0 && static_cast<size_t>(oldHdr->bvhOffset) < fileSize)
	{
		Align(o, 16);
		const size_t bvhAbs = o.cur;

		size_t blobEnd = fileSize;
		if (oldHdr->phyOffset > oldHdr->bvhOffset && static_cast<size_t>(oldHdr->phyOffset) < blobEnd)
			blobEnd = static_cast<size_t>(oldHdr->phyOffset);
		auto consider = [&](int off) {
			if (off > oldHdr->bvhOffset && static_cast<size_t>(off) < blobEnd)
				blobEnd = static_cast<size_t>(off);
		};
		consider(oldHdr->vtxOffset);
		consider(oldHdr->vvdOffset);
		consider(oldHdr->vvcOffset);
		consider(oldHdr->vvwOffset);

		const size_t blobSize = blobEnd - static_cast<size_t>(oldHdr->bvhOffset);
		const size_t room = outCap - o.cur;
		const size_t wrote = ExpandCollisionV8ToV170(pMDL + oldHdr->bvhOffset, blobSize,
			o.base + o.cur, room, rawModelName.c_str());
		if (wrote > 0)
		{
			SetHdrOff(o, offsetof(r5::v170::studiohdr_t, bvhOffset), bvhAbs);
			o.cur += wrote;
		}
	}

	// Patch degenerate animdesc.animindex to EOF-relative
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

	printf("[v17/8]   wrote %zu bytes -> %s\n", finalSize, rmdlPath.c_str());
}

//
// ConvertVGData_Rev1To17
// Rebuild a rev1 '0tVG' (CreateVGFile / early Apex) into rev4 raw (S21 client).
// Rev1 stores global index/vertex/extra buffers; mesh headers hold indices into
// those buffers. Same packed vertex bytes as rev2/rev4.
//
// Out-params: lodCount + switchPoints (from rev1 ModelLODHeader) for the rmdl
// group/LOD threshold emit; bone remap buffer for studiohdr boneStateOffset.
//
size_t ConvertVGData_Rev1To17(char* inputBuf, const size_t inputSize, const std::string& filePath,
	const std::string& pathOut, int* outLodCount, float* outSwitchPoints, int maxLods,
	uint8_t* outBoneStates, int* outBoneStateCount, int maxBoneStates)
{
	const std::string rawName = std::filesystem::path(filePath).filename().u8string();

	if (outBoneStateCount)
		*outBoneStateCount = 0;

	const vg::rev1::VertexGroupHeader_t* const srcHdr =
		reinterpret_cast<const vg::rev1::VertexGroupHeader_t*>(inputBuf);

	if (srcHdr->id != 'GVt0')
	{
		printf("[v17/vg1]  WARNING: '%s' is not a rev1 '0tVG' container (id=0x%X) -- skipping.\n",
			rawName.c_str(), srcHdr->id);
		return 0;
	}

	const int lodCount = static_cast<int>(srcHdr->lodCount);
	if (lodCount <= 0 || lodCount > 16)
	{
		printf("[v17/vg1]  WARNING: '%s' lodCount=%d invalid -- skipping.\n", rawName.c_str(), lodCount);
		return 0;
	}

	printf("[v17/vg1] Converting '%s' rev1 -> rev4 (lodCount=%d, meshes=%lld, %zu bytes)\n",
		rawName.c_str(), lodCount, static_cast<long long>(srcHdr->meshCount), inputSize);

	// Bone remap (1 byte/entry); v17 carries this in the studiohdr, not the rev4 VG.
	{
		const __int64 boneOff = srcHdr->boneStateChangeOffset;
		const __int64 boneCnt = srcHdr->boneStateChangeCount;
		if (boneCnt > 0 && boneOff > 0 &&
			static_cast<size_t>(boneOff) + static_cast<size_t>(boneCnt) <= inputSize)
		{
			if (outBoneStates && outBoneStateCount && maxBoneStates > 0)
			{
				int n = static_cast<int>(boneCnt);
				if (n > maxBoneStates)
				{
					printf("[v17/vg1]  WARNING: '%s' boneStateChangeCount=%d exceeds maxBoneStates=%d -- clamping.\n",
						rawName.c_str(), n, maxBoneStates);
					n = maxBoneStates;
				}
				memcpy(outBoneStates, inputBuf + boneOff, static_cast<size_t>(n));
				*outBoneStateCount = n;
				printf("[v17/vg1]  bone remap: %d entries\n", n);
			}
			else
			{
				printf("[v17/vg1]  WARNING: '%s' bone remap present (count=%lld) but no out buffer -- dropping.\n",
					rawName.c_str(), static_cast<long long>(boneCnt));
			}
		}
		else if (boneCnt > 0)
		{
			printf("[v17/vg1]  WARNING: '%s' bone remap invalid (offset=%lld count=%lld size=%zu) -- dropping.\n",
				rawName.c_str(), static_cast<long long>(boneOff), static_cast<long long>(boneCnt), inputSize);
		}
	}

	const vg::rev1::MeshHeader_t* const srcMeshes =
		reinterpret_cast<const vg::rev1::MeshHeader_t*>(inputBuf + srcHdr->meshOffset);
	const vg::rev1::ModelLODHeader_t* const srcLods =
		reinterpret_cast<const vg::rev1::ModelLODHeader_t*>(inputBuf + srcHdr->lodOffset);
	const char* const indexBase = inputBuf + srcHdr->indexOffset;
	const char* const vertBase  = inputBuf + srcHdr->vertOffset;
	const char* const extraBase = inputBuf + srcHdr->extraBoneWeightOffset;

	// Compute vertCacheSize if zero: from flags.
	auto VertStride = [](uint64_t flags, unsigned int reported) -> uint32_t {
		if (reported > 0) return reported;
		// fall back to flag-derived size (position + nml + uv minimum)
		uint32_t s = 0;
		if (flags & VERTEX_HAS_POSITION_PACKED) s += 8;
		else if (flags & VERTEX_HAS_POSITION) s += 12;
		if (flags & VERTEX_HAS_WEIGHT_VALUES_2) s += 4;
		if (flags & VERTEX_HAS_WEIGHT_BONES) s += 4;
		if (flags & VERTEX_HAS_NORMAL_PACKED) s += 4;
		if (flags & VERTEX_HAS_COLOR) s += 4;
		if (flags & VERTEX_HAS_UV1) s += 8;
		if (flags & VERTEX_HAS_UV2) s += 8;
		return s > 0 ? s : 28;
	};

	struct MeshRec
	{
		const vg::rev1::MeshHeader_t* src;
		uint32_t vertBufSize, indexBytes, extraBytes;
		int lod;
	};
	std::vector<MeshRec> meshes;
	std::vector<int> lodMeshCount(lodCount, 0);
	size_t totalVert = 0, totalIndex = 0, totalExtra = 0;

	for (int i = 0; i < lodCount; i++)
	{
		lodMeshCount[i] = srcLods[i].meshCount;
		if (outSwitchPoints && i < maxLods)
			outSwitchPoints[i] = srcLods[i].switchPoint;

		for (int j = 0; j < srcLods[i].meshCount; j++)
		{
			const int meshIdx = static_cast<int>(srcLods[i].meshOffset) + j;
			if (meshIdx < 0 || meshIdx >= static_cast<int>(srcHdr->meshCount))
			{
				printf("[v17/vg1]  WARNING: LOD %d mesh %d OOB (meshOffset=%u count=%u)\n",
					i, j, srcLods[i].meshOffset, srcLods[i].meshCount);
				continue;
			}
			const vg::rev1::MeshHeader_t* const m = &srcMeshes[meshIdx];
			const uint32_t stride = VertStride(static_cast<uint64_t>(m->flags), m->vertCacheSize);

			MeshRec rec{};
			rec.src = m;
			rec.vertBufSize = stride * m->vertCount;
			rec.indexBytes  = static_cast<uint32_t>(m->indexCount) * sizeof(uint16_t);
			rec.extraBytes  = static_cast<uint32_t>(m->extraBoneWeightSize > 0 ? m->extraBoneWeightSize : 0);
			rec.lod = i;
			meshes.push_back(rec);

			totalVert  += rec.vertBufSize;
			totalIndex += rec.indexBytes;
			totalExtra += rec.extraBytes;
		}
	}

	if (outLodCount)
		*outLodCount = lodCount;

	const size_t totalMeshes = meshes.size();
	auto Align16 = [](size_t c) -> size_t { return (c + 15) & ~static_cast<size_t>(15); };

	const size_t outCap = sizeof(vg::rev4::VertexGroupHeader_t)
		+ static_cast<size_t>(lodCount) * sizeof(vg::rev4::ModelLODHeader_t)
		+ totalMeshes * sizeof(vg::rev4::MeshHeader_t)
		+ totalIndex + totalVert + totalExtra
		+ 16u * (totalMeshes * 3u + static_cast<size_t>(lodCount) + 4u) + 64u;

	std::unique_ptr<char[]> outMem(new char[outCap]{});
	char* const out = outMem.get();

	vg::rev4::VertexGroupHeader_t* const gh = reinterpret_cast<vg::rev4::VertexGroupHeader_t*>(out);
	gh->lodIndex   = 0;
	gh->lodCount   = static_cast<uint8_t>(lodCount);
	gh->groupIndex = 0;
	gh->lodMap     = static_cast<uint8_t>((lodCount >= 8) ? 0xFF : ((1u << lodCount) - 1));
	const size_t lodArrOff = sizeof(vg::rev4::VertexGroupHeader_t);
	gh->lodOffset  = static_cast<uint32_t>(lodArrOff - offsetof(vg::rev4::VertexGroupHeader_t, lodOffset));

	size_t cur = lodArrOff + static_cast<size_t>(lodCount) * sizeof(vg::rev4::ModelLODHeader_t);
	size_t globalMeshIdx = 0;

	for (int i = 0; i < lodCount; i++)
	{
		cur = Align16(cur);

		const size_t lodAbs = lodArrOff + static_cast<size_t>(i) * sizeof(vg::rev4::ModelLODHeader_t);
		vg::rev4::ModelLODHeader_t* const ol = reinterpret_cast<vg::rev4::ModelLODHeader_t*>(out + lodAbs);
		ol->meshCount  = static_cast<uint8_t>(lodMeshCount[i]);
		ol->meshIndex  = 0;
		ol->lodLevel   = static_cast<uint8_t>(i);
		ol->groupIndex = 0;

		const size_t meshArrAbs = cur;
		ol->meshOffset = static_cast<uint32_t>(meshArrAbs - (lodAbs + offsetof(vg::rev4::ModelLODHeader_t, meshOffset)));
		cur += static_cast<size_t>(lodMeshCount[i]) * sizeof(vg::rev4::MeshHeader_t);

		for (int j = 0; j < lodMeshCount[i]; j++)
		{
			if (globalMeshIdx >= meshes.size())
				break;
			const MeshRec& rec = meshes[globalMeshIdx];
			const size_t meshAbs = meshArrAbs + static_cast<size_t>(j) * sizeof(vg::rev4::MeshHeader_t);
			vg::rev4::MeshHeader_t* const nm = reinterpret_cast<vg::rev4::MeshHeader_t*>(out + meshAbs);

			const uint32_t stride = VertStride(static_cast<uint64_t>(rec.src->flags), rec.src->vertCacheSize);
			nm->flags         = static_cast<uint64_t>(rec.src->flags);
			nm->vertCount     = rec.src->vertCount;
			nm->vertCacheSize = static_cast<uint16_t>(stride);

			uint16_t maxBones = 1;
			if ((rec.src->flags & VERTEX_HAS_WEIGHT_BONES) && rec.src->vertCount > 0)
			{
				const char* const vtx = vertBase + rec.src->vertOffset;
				for (uint32_t v = 0; v < rec.src->vertCount; v++)
				{
					const vg::mstudiopackedbones_t* const pb =
						reinterpret_cast<const vg::mstudiopackedbones_t*>(vtx + static_cast<size_t>(v) * stride + 12);
					const uint16_t nb = static_cast<uint16_t>(pb->numbones + 1);
					if (nb > maxBones) maxBones = nb;
				}
			}
			nm->vertBoneCount = maxBones;
			nm->indexCount = static_cast<uint32_t>(rec.src->indexCount) & 0x0FFFFFFFu;
			nm->indexType  = 0;

			cur = Align16(cur);
			nm->indexOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, indexOffset)));
			memcpy(out + cur, indexBase + static_cast<size_t>(rec.src->indexOffset) * sizeof(uint16_t), rec.indexBytes);
			cur += rec.indexBytes;

			cur = Align16(cur);
			nm->vertOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, vertOffset)));
			nm->vertBufferSize = rec.vertBufSize;
			memcpy(out + cur, vertBase + rec.src->vertOffset, rec.vertBufSize);
			cur += rec.vertBufSize;

			if (rec.extraBytes > 0)
			{
				cur = Align16(cur);
				nm->extraBoneWeightOffset = static_cast<uint32_t>(cur - (meshAbs + offsetof(vg::rev4::MeshHeader_t, extraBoneWeightOffset)));
				nm->extraBoneWeightSize = rec.extraBytes;
				memcpy(out + cur, extraBase + rec.src->extraBoneWeightOffset, rec.extraBytes);
				cur += rec.extraBytes;
			}
			else
			{
				nm->extraBoneWeightOffset = 0;
				nm->extraBoneWeightSize = 0;
			}

			nm->blendShapeVertOffset = 0;
			nm->blendShapeVertBufferSize = 0;

			globalMeshIdx++;
		}
	}

	const size_t outSize = cur;
	std::ofstream ofs(pathOut, std::ios::out | std::ios::binary);
	ofs.write(out, static_cast<std::streamsize>(outSize));
	ofs.close();

	printf("[v17/vg1]  wrote %zu bytes (%zu meshes) -> %s\n", outSize, totalMeshes, pathOut.c_str());
	return outSize;
}

//
// ConvertClientModel_8To17
// Orchestrates: optional CreateVGFile from .vtx/.vvd, rev1 (or rev2) VG -> rev4,
// ConvertRMDL8To17, optional .phy pass-through / compact.
//
void ConvertClientModel_8To17(const std::string& inputFile, const std::string& outputFile)
{
	uintmax_t fileSize = GetFileSize(inputFile);
	std::unique_ptr<char[]> pMDL(new char[fileSize]);
	{
		std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
		ifs.read(pMDL.get(), fileSize);
	}
	std::filesystem::create_directories(std::filesystem::path(outputFile).parent_path());

	// Ensure a .vg exists: prefer existing, else build rev1 from vtx/vvd.
	std::string vgIn = ChangeExtension(inputFile, "vg");
	const std::string vgOut = ChangeExtension(outputFile, "vg");
	std::string tempVgFromVtx;

	if (!FILE_EXISTS(vgIn))
	{
		std::string pathVTX = ChangeExtension(inputFile, "vtx");
		if (!FILE_EXISTS(pathVTX))
			pathVTX = ChangeExtension(inputFile, "dx11.vtx");
		const std::string pathVVD = ChangeExtension(inputFile, "vvd");
		const std::string pathVVC = ChangeExtension(inputFile, "vvc");
		const std::string pathVVW = ChangeExtension(inputFile, "vvw");

		if (FILE_EXISTS(pathVTX) && FILE_EXISTS(pathVVD))
		{
			printf("[v17/8]   no .vg -- building rev1 VG from .vtx/.vvd\n");
			tempVgFromVtx = ChangeExtension(outputFile, "vg.tmp_rev1");

			std::unique_ptr<char[]> pVTX(new char[GetFileSize(pathVTX)]);
			std::unique_ptr<char[]> pVVD(new char[GetFileSize(pathVVD)]);
			{
				std::ifstream ifs(pathVTX, std::ios::binary);
				ifs.read(pVTX.get(), GetFileSize(pathVTX));
			}
			{
				std::ifstream ifs(pathVVD, std::ios::binary);
				ifs.read(pVVD.get(), GetFileSize(pathVVD));
			}
			std::unique_ptr<char[]> pVVC;
			std::unique_ptr<char[]> pVVW;
			if (FILE_EXISTS(pathVVC))
			{
				pVVC.reset(new char[GetFileSize(pathVVC)]);
				std::ifstream ifs(pathVVC, std::ios::binary);
				ifs.read(pVVC.get(), GetFileSize(pathVVC));
			}
			if (FILE_EXISTS(pathVVW))
			{
				pVVW.reset(new char[GetFileSize(pathVVW)]);
				std::ifstream ifs(pathVVW, std::ios::binary);
				ifs.read(pVVW.get(), GetFileSize(pathVVW));
			}

			CreateVGFile(tempVgFromVtx, reinterpret_cast<r5::v8::studiohdr_t*>(pMDL.get()),
				pVTX.get(), pVVD.get(), pVVC.get(), pVVW.get());
			vgIn = tempVgFromVtx;
		}
		else
		{
			printf("[v17/8]   WARNING: no .vg and no .vtx/.vvd -- rmdl-only convert (no group header).\n");
		}
	}

	uint32_t vgDecompSize = 0;
	int vgLodCount = 0;
	float vgSwitch[16] = {};
	uint8_t boneStates[256] = {};
	int boneStateCount = 0;
	const std::string rawModelName = std::filesystem::path(inputFile).filename().u8string();

	if (FILE_EXISTS(vgIn))
	{
		uintmax_t vgSize = GetFileSize(vgIn);
		std::unique_ptr<char[]> vgBuf(new char[vgSize]);
		{
			std::ifstream vgIfs(vgIn, std::ios::in | std::ios::binary);
			vgIfs.read(vgBuf.get(), vgSize);
		}

		const int id = *reinterpret_cast<const int*>(vgBuf.get());
		if (id == 'GVt0')
		{
			// rev1: dataSize at +12 is ~filesize. rev2: lodCount at +12 is 1..16.
			const int at12 = *reinterpret_cast<const int*>(vgBuf.get() + 12);
			const bool looksRev1 = (static_cast<size_t>(at12) == vgSize) ||
				(at12 > 64 && static_cast<size_t>(at12) + 64 >= vgSize);
			const bool looksRev2 = !looksRev1 && (at12 >= 1 && at12 <= 16);

			if (looksRev2)
			{
				vgDecompSize = static_cast<uint32_t>(ConvertVGData_Rev2To17(
					vgBuf.get(), vgSize, vgIn, vgOut));
				const vg::rev2::VertexGroupHeader_t* const rh =
					reinterpret_cast<const vg::rev2::VertexGroupHeader_t*>(vgBuf.get());
				vgLodCount = rh->lodCount;
				printf("[v17/8]   WARNING: '%s' rev2 VG -- bone remap table not carried.\n",
					rawModelName.c_str());
			}
			else
			{
				vgDecompSize = static_cast<uint32_t>(ConvertVGData_Rev1To17(
					vgBuf.get(), vgSize, vgIn, vgOut, &vgLodCount, vgSwitch, 16,
					boneStates, &boneStateCount, 256));
			}
		}
		else
		{
			printf("[v17/8]   VG has no 0tVG magic -- copying as rev4-ish passthrough.\n");
			std::filesystem::copy_file(vgIn, vgOut, std::filesystem::copy_options::overwrite_existing);
			vgDecompSize = static_cast<uint32_t>(vgSize);
			vgLodCount = 1;
			printf("[v17/8]   WARNING: '%s' VG passthrough -- bone remap table not carried.\n",
				rawModelName.c_str());
		}

		if (vgDecompSize > 0)
		{
			std::error_code ec;
			std::filesystem::copy_file(vgOut, ChangeExtension(outputFile, "vg_static"),
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
				printf("[v17/8]   WARNING: could not write .vg_static (%s)\n", ec.message().c_str());
		}
	}

	ConvertRMDL8To17(pMDL.get(), fileSize, inputFile, outputFile, vgDecompSize, vgLodCount,
		vgLodCount > 0 ? vgSwitch : nullptr, boneStates, boneStateCount);

	const std::string phyIn = ChangeExtension(inputFile, "phy");
	if (FILE_EXISTS(phyIn))
	{
		uintmax_t phySize = GetFileSize(phyIn);
		std::unique_ptr<char[]> phyBuf(new char[phySize]);
		{
			std::ifstream phyIfs(phyIn, std::ios::in | std::ios::binary);
			phyIfs.read(phyBuf.get(), phySize);
		}
		ConvertPhy_122To17(phyBuf.get(), phySize, phyIn, ChangeExtension(outputFile, "phy"));
	}

	if (!tempVgFromVtx.empty())
	{
		std::error_code ec;
		std::filesystem::remove(tempVgFromVtx, ec);
	}
}

//
// ConvertClientModel_49To17
// Portal 2 (MDL v49) -> v8 intermediate via existing ConvertMDL49To54, then v8->v17.
//
void ConvertClientModel_49To17(const std::string& inputFile, const std::string& outputFile)
{
	printf("[v17/49] Portal 2 / MDL49 -> v17 via v8 intermediate: %s\n", inputFile.c_str());

	const std::filesystem::path outPath(outputFile);
	const std::filesystem::path tempDir = outPath.parent_path() / "_v8_tmp";
	std::filesystem::create_directories(tempDir);

	const std::string tempRmdl = (tempDir / outPath.filename()).string();

	uintmax_t fileSize = GetFileSize(inputFile);
	std::unique_ptr<char[]> pMDL(new char[fileSize]);
	{
		std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
		ifs.read(pMDL.get(), fileSize);
	}

	// ConvertMDL49To54 writes rmdl + CreateVGFile(.vg) next to pathOut.
	ConvertMDL49To54(pMDL.get(), inputFile, tempRmdl);

	if (!FILE_EXISTS(tempRmdl) && !FILE_EXISTS(ChangeExtension(tempRmdl, "rmdl")))
	{
		// ConvertMDL49To54 may keep extension from pathOut; try .rmdl
		printf("[v17/49]   WARNING: intermediate missing at '%s' -- trying .rmdl extension.\n", tempRmdl.c_str());
	}

	std::string mid = tempRmdl;
	if (!FILE_EXISTS(mid))
		mid = ChangeExtension(tempRmdl, "rmdl");
	if (!FILE_EXISTS(mid))
		Error("[v17/49] intermediate v8 model not produced: %s\n", tempRmdl.c_str());

	// Sibling .vg from CreateVGFile is next to intermediate.
	ConvertClientModel_8To17(mid, outputFile);

	// Cleanup temp tree (best-effort).
	std::error_code ec;
	std::filesystem::remove_all(tempDir, ec);
}
