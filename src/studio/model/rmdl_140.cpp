// Copyright (c) 2023, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/versions.h>
#include <studio/common.h>

/*
	Type:    RMDL
	Version: 14/14.1 (Season 13-14) and 15 (Season 15)
	Game:    Apex Legends

	Files: .rmdl, .vg (rev3 format)
*/

// Forward declarations for helper functions in rmdl_121.cpp
extern void ConvertBones_121(r5::v121::mstudiobone_t* pOldBones, int numBones, bool isRig);
extern void ConvertHitboxes_121(mstudiohitboxset_t* pOldHitboxSets, int numHitboxSets);
extern void ConvertIkChains_121(r5::v8::mstudioikchain_t* pOldIkChains, int numIkChains, bool isRig);
extern void ConvertTextures_121(mstudiotexturedir_t* pCDTextures, int numCDTextures, r5::v8::mstudiotexture_t* pOldTextures, int numTextures, const MaterialShaderType_t* const shaderTypes);
extern void ConvertSkins_121(char* pOldModelBase, char* pOldSkinData, int numSkinRef, int numSkinFamilies);

#define FILEBUFSIZE (32 * 1024 * 1024)

//
// ConvertStudioHdr_140
template <typename T>
static void ConvertStudioHdr_140(r5::v8::studiohdr_t* out, T* hdr)
{
	out->id = 'TSDI';
	out->version = 54;

	out->checksum = hdr->checksum;

	memcpy_s(out->name, 64, hdr->name, 64);

	out->length = 0xbadf00d; // needs to be written later

	out->eyeposition = hdr->eyeposition;
	out->illumposition = hdr->illumposition;
	out->hull_min = hdr->hull_min;
	out->hull_max = hdr->hull_max;

	out->mins = hdr->hull_min;
	out->maxs = hdr->hull_max;

	out->view_bbmin = hdr->view_bbmin;
	out->view_bbmax = hdr->view_bbmax;

	out->flags = hdr->flags;

	out->numbones = hdr->numbones;
	out->numbonecontrollers = hdr->numbonecontrollers;
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalanim = 0; // this is no longer used, force set to 0
	out->numlocalseq = hdr->numlocalseq;
	out->activitylistversion = hdr->activitylistversion;

	out->numtextures = hdr->numtextures;
	out->numcdtextures = hdr->numcdtextures;
	out->numskinref = hdr->numskinref;
	out->numskinfamilies = hdr->numskinfamilies;
	out->numbodyparts = hdr->numbodyparts;
	out->numlocalattachments = hdr->numlocalattachments;

	out->keyvaluesize = hdr->keyvaluesize;
	out->numincludemodels = -1;
	out->numsrcbonetransform = hdr->numsrcbonetransform;

	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->defaultFadeDist;
	out->flVertAnimFixedPointScale = hdr->flVertAnimFixedPointScale;

	out->phyOffset = hdr->phyOffset;
	out->vtxSize = hdr->vtxSize;
	out->vvdSize = hdr->vvdSize;
	out->vvcSize = hdr->vvcSize;
	out->phySize = hdr->phySize;
	out->vvwSize = hdr->vvwSize;
}

template <typename T>
static void GenerateRigHdr_140(r5::v8::studiohdr_t* out, T* hdr)
{
	out->id = 'TSDI';
	out->version = 54;

	memcpy_s(out->name, 64, hdr->name, 64);

	out->numbones = hdr->numbones;
	out->numbonecontrollers = hdr->numbonecontrollers;
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalattachments = hdr->numlocalattachments;
	out->numlocalnodes = hdr->numlocalnodes;
	out->numikchains = hdr->numikchains;
	out->numlocalposeparameters = hdr->numlocalposeparameters;

	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->defaultFadeDist;
}

// Convert bodyparts, models, and meshes from v140 format
// Key differences from v121:
// - mstudiomodel_t has mesh count split (nummeshes, unk_v14, unk1_v14)
// - mstudiomesh_t has uint16_t material instead of int
static void ConvertBodyParts_140(mstudiobodyparts_t* pOldBodyParts, int numBodyParts)
{
	printf("converting %i bodyparts...\n", numBodyParts);

	g_model.hdrV54()->bodypartindex = g_model.pData - g_model.pBase;

	mstudiobodyparts_t* bodypartStart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);
	for (int i = 0; i < numBodyParts; ++i)
	{
		mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

		memcpy(g_model.pData, oldbodypart, sizeof(mstudiobodyparts_t));

		printf("%s\n", STRING_FROM_IDX(oldbodypart, oldbodypart->sznameindex));
		AddToStringTable((char*)newbodypart, &newbodypart->sznameindex, STRING_FROM_IDX(oldbodypart, oldbodypart->sznameindex));

		g_model.pData += sizeof(mstudiobodyparts_t);
	}

	for (int i = 0; i < numBodyParts; ++i)
	{
		mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = bodypartStart + i;

		newbodypart->modelindex = g_model.pData - (char*)newbodypart;

		// pointer to old models using v140 structure
		r5::v140::mstudiomodel_t* oldModels = reinterpret_cast<r5::v140::mstudiomodel_t*>((char*)oldbodypart + oldbodypart->modelindex);

		// pointer to start of new model data (in .rmdl)
		r5::v8::mstudiomodel_t* newModels = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);
		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r5::v140::mstudiomodel_t* oldModel = oldModels + j;
			r5::v8::mstudiomodel_t* newModel = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);

			memcpy(&newModel->name, &oldModel->name, sizeof(newModel->name));
			newModel->type = oldModel->type;
			newModel->boundingradius = oldModel->boundingradius;
			// v140 has mesh count split into nummeshes, unk_v14, unk1_v14
			// For conversion to v10, we use nummeshes which is the total mesh count
			newModel->nummeshes = oldModel->nummeshes;
			newModel->numvertices = oldModel->numvertices;
			newModel->vertexindex = oldModel->vertexindex;
			newModel->tangentsindex = oldModel->tangentsindex;
			newModel->numattachments = oldModel->numattachments;
			newModel->attachmentindex = oldModel->attachmentindex;
			newModel->deprecated_numeyeballs = 0;
			newModel->deprecated_eyeballindex = 0;
			newModel->colorindex = oldModel->colorindex;
			newModel->uv2index = oldModel->uv2index;
			// Note: v140 has uv3index (unk) which we ignore

			g_model.pData += sizeof(r5::v8::mstudiomodel_t);
		}

		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r5::v140::mstudiomodel_t* oldModel = oldModels + j;
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			newModel->meshindex = g_model.pData - (char*)newModel;

			// pointer to old meshes using v140 structure
			r5::v140::mstudiomesh_t* oldMeshes = reinterpret_cast<r5::v140::mstudiomesh_t*>((char*)oldModel + oldModel->meshindex);

			// pointer to new meshes for this model (in .rmdl)
			r5::v8::mstudiomesh_t* newMeshes = reinterpret_cast<r5::v8::mstudiomesh_t*>(g_model.pData);

			for (int k = 0; k < newModel->nummeshes; ++k)
			{
				r5::v140::mstudiomesh_t* oldMesh = oldMeshes + k;
				r5::v8::mstudiomesh_t* newMesh = newMeshes + k;

				// v140 has uint16_t material, v8 has int material - cast it
				newMesh->material = static_cast<int>(oldMesh->material);
				newMesh->numvertices = oldMesh->numvertices;
				newMesh->vertexoffset = oldMesh->vertexoffset;
				newMesh->meshid = oldMesh->meshid;
				newMesh->center = oldMesh->center;
				newMesh->vertexloddata = oldMesh->vertexloddata;

				newMesh->modelindex = (char*)newModel - (char*)newMesh;

				g_model.pData += sizeof(r5::v8::mstudiomesh_t);
			}
		}
	}

	ALIGN4(g_model.pData);
}

// Convert bodyparts for v15 which has larger mstudiobodyparts_t
static void ConvertBodyParts_150(r5::v150::mstudiobodyparts_t* pOldBodyParts, int numBodyParts)
{
	printf("converting %i bodyparts (v15)...\n", numBodyParts);

	g_model.hdrV54()->bodypartindex = g_model.pData - g_model.pBase;

	mstudiobodyparts_t* bodypartStart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);
	for (int i = 0; i < numBodyParts; ++i)
	{
		r5::v150::mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

		// Copy only the common fields (ignore v15's extra unk_10 and meshOffset)
		newbodypart->sznameindex = oldbodypart->sznameindex;
		newbodypart->nummodels = oldbodypart->nummodels;
		newbodypart->base = oldbodypart->base;
		newbodypart->modelindex = oldbodypart->modelindex;

		const char* bodypartName = STRING_FROM_IDX(oldbodypart, oldbodypart->sznameindex);
		printf("%s\n", bodypartName);
		AddToStringTable((char*)newbodypart, &newbodypart->sznameindex, bodypartName);

		g_model.pData += sizeof(mstudiobodyparts_t);
	}

	for (int i = 0; i < numBodyParts; ++i)
	{
		r5::v150::mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = bodypartStart + i;

		newbodypart->modelindex = g_model.pData - (char*)newbodypart;

		// pointer to old models using v140 structure (v15 uses same model struct as v14)
		r5::v140::mstudiomodel_t* oldModels = reinterpret_cast<r5::v140::mstudiomodel_t*>((char*)oldbodypart + oldbodypart->modelindex);

		// pointer to start of new model data (in .rmdl)
		r5::v8::mstudiomodel_t* newModels = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);
		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r5::v140::mstudiomodel_t* oldModel = oldModels + j;
			r5::v8::mstudiomodel_t* newModel = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);

			memcpy(&newModel->name, &oldModel->name, sizeof(newModel->name));
			newModel->type = oldModel->type;
			newModel->boundingradius = oldModel->boundingradius;
			newModel->nummeshes = oldModel->nummeshes;
			newModel->numvertices = oldModel->numvertices;
			newModel->vertexindex = oldModel->vertexindex;
			newModel->tangentsindex = oldModel->tangentsindex;
			newModel->numattachments = oldModel->numattachments;
			newModel->attachmentindex = oldModel->attachmentindex;
			newModel->deprecated_numeyeballs = 0;
			newModel->deprecated_eyeballindex = 0;
			newModel->colorindex = oldModel->colorindex;
			newModel->uv2index = oldModel->uv2index;

			g_model.pData += sizeof(r5::v8::mstudiomodel_t);
		}

		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r5::v140::mstudiomodel_t* oldModel = oldModels + j;
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			newModel->meshindex = g_model.pData - (char*)newModel;

			// pointer to old meshes using v140 structure
			r5::v140::mstudiomesh_t* oldMeshes = reinterpret_cast<r5::v140::mstudiomesh_t*>((char*)oldModel + oldModel->meshindex);

			// pointer to new meshes for this model (in .rmdl)
			r5::v8::mstudiomesh_t* newMeshes = reinterpret_cast<r5::v8::mstudiomesh_t*>(g_model.pData);

			for (int k = 0; k < newModel->nummeshes; ++k)
			{
				r5::v140::mstudiomesh_t* oldMesh = oldMeshes + k;
				r5::v8::mstudiomesh_t* newMesh = newMeshes + k;

				newMesh->material = static_cast<int>(oldMesh->material);
				newMesh->numvertices = oldMesh->numvertices;
				newMesh->vertexoffset = oldMesh->vertexoffset;
				newMesh->meshid = oldMesh->meshid;
				newMesh->center = oldMesh->center;
				newMesh->vertexloddata = oldMesh->vertexloddata;

				newMesh->modelindex = (char*)newModel - (char*)newMesh;

				g_model.pData += sizeof(r5::v8::mstudiomesh_t);
			}
		}
	}

	ALIGN4(g_model.pData);
}

static int CopyAttachmentsData_140(r5::v8::mstudioattachment_t* pOldAttachments, int numAttachments)
{
	int index = g_model.pData - g_model.pBase;

	printf("converting %i attachments...\n", numAttachments);

	for (int i = 0; i < numAttachments; ++i)
	{
		r5::v8::mstudioattachment_t* oldAttach = &pOldAttachments[i];

		r5::v8::mstudioattachment_t* attach = reinterpret_cast<r5::v8::mstudioattachment_t*>(g_model.pData) + i;

		AddToStringTable((char*)attach, &attach->sznameindex, STRING_FROM_IDX(oldAttach, oldAttach->sznameindex));
		attach->flags = oldAttach->flags;
		attach->localbone = oldAttach->localbone;
		memcpy(&attach->localmatrix, &oldAttach->localmatrix, sizeof(oldAttach->localmatrix));
	}
	g_model.pData += numAttachments * sizeof(r5::v8::mstudioattachment_t);

	return index;

	ALIGN4(g_model.pData);
}

template <typename mstudioanimdesc_type_t>
static void CopyAnimDesc_140(const r5::v8::mstudioseqdesc_t* const curOldSeqDesc, r5::v8::mstudioseqdesc_t* const curNewSeqDesc,
						 const int* const oldBlendGroups, int* const newBlendGroups, const int numAnims)
{
	for (int i = 0; i < numAnims; i++)
	{
		const mstudioanimdesc_type_t* const oldAnimDesc = PTR_FROM_IDX(mstudioanimdesc_type_t, curOldSeqDesc, oldBlendGroups[i]);
		r5::v8::mstudioanimdesc_t* const newAnimDesc = reinterpret_cast<r5::v8::mstudioanimdesc_t*>(g_model.pData);

		newBlendGroups[i] = g_model.pData - reinterpret_cast<const char*>(curNewSeqDesc);
		g_model.pData += sizeof(r5::v8::mstudioanimdesc_t);

		newAnimDesc->baseptr = oldAnimDesc->baseptr;
		AddToStringTable((char*)newAnimDesc, &newAnimDesc->sznameindex, STRING_FROM_IDX(oldAnimDesc, oldAnimDesc->sznameindex));
		newAnimDesc->fps = oldAnimDesc->fps;
		newAnimDesc->flags = oldAnimDesc->flags;
		newAnimDesc->numframes = oldAnimDesc->numframes;

		newAnimDesc->animindex = ConvertAnimation(PTR_FROM_IDX(char, oldAnimDesc, oldAnimDesc->animindex), newAnimDesc, g_model.hdrV54()->numbones);
	}
}

template <typename mstudioanimdesc_type_t>
static void ConvertAnims_140(const char* const oldData, const int numlocalseq)
{
	g_model.hdrV54()->localseqindex = g_model.pData - g_model.pBase;
	g_model.hdrV54()->numlocalseq = numlocalseq;

	CopyAnimRefData(oldData, g_model.pData, numlocalseq);

	const r5::v8::mstudioseqdesc_t* const oldSeqDescBase = reinterpret_cast<const r5::v8::mstudioseqdesc_t*>(oldData);
	r5::v8::mstudioseqdesc_t* const newSeqDescBase = reinterpret_cast<r5::v8::mstudioseqdesc_t*>(g_model.pData);

	g_model.pData += numlocalseq * sizeof(r5::v8::mstudioseqdesc_t);

	for (int i = 0; i < numlocalseq; i++)
	{
		const r5::v8::mstudioseqdesc_t* const curOldSeqDesc = &oldSeqDescBase[i];
		r5::v8::mstudioseqdesc_t* const curNewSeqDesc = &newSeqDescBase[i];

		const int numAnims = curOldSeqDesc->groupsize[0] + curOldSeqDesc->groupsize[1];

		if (numAnims)
		{
			const size_t copyCount = numAnims * sizeof(int);

			const int* const oldBlendGroups = PTR_FROM_IDX(int, curOldSeqDesc, curOldSeqDesc->animindexindex);
			int* const newBlendGroups = reinterpret_cast<int*>(g_model.pData);

			curNewSeqDesc->animindexindex = g_model.pData - reinterpret_cast<const char*>(curNewSeqDesc);
			g_model.pData += copyCount;

			CopyAnimDesc_140<mstudioanimdesc_type_t>(curOldSeqDesc, curNewSeqDesc, oldBlendGroups, newBlendGroups, numAnims);
		}

		if (curOldSeqDesc->weightlistindex)
		{
			const size_t copyCount = g_model.hdrV54()->numbones * sizeof(float);
			memcpy(g_model.pData, PTR_FROM_IDX(char, curOldSeqDesc, curOldSeqDesc->weightlistindex), copyCount);

			curNewSeqDesc->weightlistindex = g_model.pData - reinterpret_cast<const char*>(curNewSeqDesc);
			g_model.pData += copyCount;
		}

		if (curOldSeqDesc->posekeyindex)
		{
			const size_t copyCount = numAnims * sizeof(float);
			memcpy(g_model.pData, PTR_FROM_IDX(char, curOldSeqDesc, curOldSeqDesc->posekeyindex), copyCount);

			curNewSeqDesc->posekeyindex = g_model.pData - reinterpret_cast<const char*>(curNewSeqDesc);
			g_model.pData += copyCount;
		}
	}

	ALIGN4(g_model.pData);
}

template <typename T>
static void ConvertUIPanelMeshes_140(const T* const oldHeader, rmem& input)
{
	if (oldHeader->uiPanelCount == 0)
		return;

	g_model.hdrV54()->uiPanelCount = oldHeader->uiPanelCount;
	input.seek(oldHeader->uiPanelOffset, rseekdir::beg);

	const size_t totalHeaderBufSize = oldHeader->uiPanelCount * sizeof(r5::v8::mstudiorruiheader_t);
	input.read(g_model.pData, totalHeaderBufSize, true);

	g_model.hdrV54()->uiPanelOffset = static_cast<int>(g_model.pData - g_model.pBase);

	r5::v8::mstudiorruiheader_t* const ruiHeaders = reinterpret_cast<r5::v8::mstudiorruiheader_t*>(g_model.pData);
	g_model.pData += totalHeaderBufSize;

	ALIGN16(g_model.pData);

	for (int i = 0; i < oldHeader->uiPanelCount; i++)
	{
		r5::v8::mstudiorruiheader_t* ruiHeader = &ruiHeaders[i];
		const size_t seekOffset = (oldHeader->uiPanelOffset + (i * sizeof(r5::v8::mstudiorruiheader_t))) + ruiHeader->ruimeshindex;

		input.seek(seekOffset, rseekdir::beg);
		input.read(g_model.pData, sizeof(r5::v8::mstudioruimesh_t), true);

		ruiHeader->ruimeshindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(ruiHeader));

		const r5::v8::mstudioruimesh_t* const header = reinterpret_cast<r5::v8::mstudioruimesh_t*>(g_model.pData);
		g_model.pData += sizeof(r5::v8::mstudioruimesh_t);

		input.read(g_model.pData, header->parentindex, true);
		g_model.pData += header->parentindex;

		const size_t parentBytes = header->numparents * sizeof(short);
		input.read(g_model.pData, parentBytes, true);
		g_model.pData += parentBytes;

		const size_t vertMapBytes = header->numfaces * sizeof(r5::v8::mstudioruivertmap_t);
		input.read(g_model.pData, vertMapBytes, true);
		g_model.pData += vertMapBytes;

		const size_t fourthVertBytes = header->numfaces * sizeof(r5::v8::mstudioruifourthvert_t);
		input.read(g_model.pData, fourthVertBytes, true);
		g_model.pData += fourthVertBytes;

		const size_t vertBytes = header->numvertices * sizeof(r5::v8::mstudioruivert_t);
		input.read(g_model.pData, vertBytes, true);
		g_model.pData += vertBytes;

		const size_t faceBytes = header->numfaces * sizeof(r5::v8::mstudioruimeshface_t);
		input.read(g_model.pData, faceBytes, true);
		g_model.pData += faceBytes;
	}

	ALIGN4(g_model.pData);
}

//
// ConvertRMDL140To10
// Purpose: converts mdl data from rmdl v54 subversion 14/14.1 to rmdl v10 (Season 2/3)
//
void ConvertRMDL140To10(char* pMDL, const std::string& pathIn, const std::string& pathOut)
{
	std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();

	printf("Converting model '%s' from version 54 (subversion 14/14.1) to version 54 (subversion 10)...\n", rawModelName.c_str());

	TIME_SCOPE(__FUNCTION__);

	rmem input(pMDL);

	r5::v140::studiohdr_t* oldHeader = input.get<r5::v140::studiohdr_t>();

	std::filesystem::path inputPath(pathIn);
	std::filesystem::path outputDir;
	std::string rmdlPath;

	if (pathOut != pathIn && !pathOut.empty())
	{
		rmdlPath = pathOut;
		outputDir = std::filesystem::path(pathOut).parent_path();
		std::filesystem::create_directories(outputDir);
	}
	else
	{
		outputDir = inputPath.parent_path() / "rmdlconv_out";
		std::filesystem::create_directories(outputDir);
		rmdlPath = (outputDir / inputPath.filename()).string();
	}

	printf("Output: %s\n", rmdlPath.c_str());
	std::ofstream out(rmdlPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	r5::v8::studiohdr_t* pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	ConvertStudioHdr_140(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	if (oldHeader->sourceFilenameOffset != 0 && oldHeader->boneindex > oldHeader->sourceFilenameOffset)
	{
		input.seek(oldHeader->sourceFilenameOffset, rseekdir::beg);
		const int sourceNameSize = oldHeader->boneindex - oldHeader->sourceFilenameOffset;

		input.read(g_model.pData, sourceNameSize);
		g_model.hdrV54()->sourceFilenameOffset = static_cast<int>(g_model.pData - g_model.pBase);

		g_model.pData += sourceNameSize;
	}

	BeginStringTable();

	std::string originalModelName = STRING_FROM_IDX(pMDL, oldHeader->sznameindex);

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
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, STRING_FROM_IDX(pMDL, oldHeader->surfacepropindex));
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// convert bones - v140 uses same bone structure as v121
	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_121((r5::v121::mstudiobone_t*)input.getPtr(), oldHeader->numbones, false);

	// convert attachments
	input.seek(oldHeader->localattachmentindex, rseekdir::beg);
	g_model.hdrV54()->localattachmentindex = CopyAttachmentsData_140((r5::v8::mstudioattachment_t*)input.getPtr(), oldHeader->numlocalattachments);

	// convert hitboxsets and hitboxes
	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_121((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	// copy bonebyname table
	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	input.seek(oldHeader->localseqindex, rseekdir::beg);
	ConvertAnims_140<r5::v121::mstudioanimdesc_t>((const char*)input.getPtr(), oldHeader->numlocalseq);

	// convert bodyparts, models, and meshes
	input.seek(oldHeader->bodypartindex, rseekdir::beg);
	ConvertBodyParts_140((mstudiobodyparts_t*)input.getPtr(), oldHeader->numbodyparts);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, false);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_121((r5::v8::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, false);

	ConvertUIPanelMeshes_140(oldHeader, input);

	// get cdtextures pointer for converting textures
	input.seek(oldHeader->cdtextureindex, rseekdir::beg);
	void* pOldCDTextures = input.getPtr();

	MaterialShaderType_t* matTypes = nullptr;

	if (oldHeader->materialtypesindex > 0)
		matTypes = reinterpret_cast<MaterialShaderType_t*>(&pMDL[oldHeader->materialtypesindex]);

	// convert textures
	input.seek(oldHeader->textureindex, rseekdir::beg);
	ConvertTextures_121((mstudiotexturedir_t*)pOldCDTextures, oldHeader->numcdtextures, (r5::v8::mstudiotexture_t*)input.getPtr(), oldHeader->numtextures, matTypes);

	// convert skin data
	input.seek(oldHeader->skinindex, rseekdir::beg);
	ConvertSkins_121(pMDL, (char*)input.getPtr(), oldHeader->numskinref, oldHeader->numskinfamilies);

	// write base keyvalues
	std::string keyValues = "mdlkeyvalue{prop_data{base \"\"}}\n";
	strcpy_s(g_model.pData, keyValues.length() + 1, keyValues.c_str());

	pHdr->keyvalueindex = g_model.pData - g_model.pBase;
	pHdr->keyvaluesize = IALIGN2(keyValues.length() + 1);

	g_model.pData += keyValues.length() + 1;
	ALIGN4(g_model.pData);

	// SrcBoneTransforms
	input.seek(oldHeader->srcbonetransformindex, rseekdir::beg);
	g_model.hdrV54()->srcbonetransformindex = ConvertSrcBoneTransforms((mstudiosrcbonetransform_t*)input.getPtr(), oldHeader->numsrcbonetransform);

	if (oldHeader->linearboneindex && oldHeader->numbones > 1)
	{
		input.seek(oldHeader->linearboneindex, rseekdir::beg);
		CopyLinearBoneTableTo54(reinterpret_cast<const r5::v8::mstudiolinearbone_t* const>(input.getPtr()));
	}

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN64(g_model.pData);

	if (oldHeader->bvhOffset)
	{
		g_model.hdrV54()->bvhOffset = g_model.pData - g_model.pBase;

		input.seek(oldHeader->bvhOffset, rseekdir::beg);
		ConvertCollisionData_V120(oldHeader, (char*)input.getPtr());
	}

	pHdr->length = g_model.pData - g_model.pBase;

	out.write(g_model.pBase, pHdr->length);

	// convert v14/v14.1 vg to v9 vg using rev3 conversion
	std::string vgFilePath = ChangeExtension(pathIn, "vg");

	if (FILE_EXISTS(vgFilePath))
	{
		uintmax_t vgInputSize = GetFileSize(vgFilePath);

		char* vgInputBuf = new char[vgInputSize];

		std::ifstream ifs(vgFilePath, std::ios::in | std::ios::binary);

		ifs.read(vgInputBuf, vgInputSize);

		if (*(int*)vgInputBuf == 'GVt0')
		{
			std::string vgOutputPath = (outputDir / std::filesystem::path(vgFilePath).filename()).string();
			printf("VG Output: %s\n", vgOutputPath.c_str());
			ConvertVGData_Rev3(vgInputBuf, vgFilePath, vgOutputPath);
		}

		delete[] vgInputBuf;
	}

	FreeModelBuf(g_model.pBase);

	// RRIG generation disabled - not needed for converted models
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

	std::string rrigPath = (outputDir / (inputPath.stem().string() + ".rrig")).string();
	std::ofstream rigOut(rrigPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	GenerateRigHdr_140(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	BeginStringTable();

	memcpy_s(&pHdr->name, 64, rigName.c_str(), min(rigName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, rigName.c_str());
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, STRING_FROM_IDX(pMDL, oldHeader->surfacepropindex));
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// convert bones
	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_121((r5::v121::mstudiobone_t*)input.getPtr(), oldHeader->numbones, true);

	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_121((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	input.seek(oldHeader->localseqindex, rseekdir::beg);
	ConvertAnims_140<r5::v121::mstudioanimdesc_t>((const char*)input.getPtr(), oldHeader->numlocalseq);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, false);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_121((r5::v8::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, true);

	input.seek(oldHeader->srcbonetransformindex, rseekdir::beg);
	g_model.hdrV54()->srcbonetransformindex = ConvertSrcBoneTransforms((mstudiosrcbonetransform_t*)input.getPtr(), oldHeader->numsrcbonetransform);

	if (oldHeader->linearboneindex && oldHeader->numbones > 1)
	{
		input.seek(oldHeader->linearboneindex, rseekdir::beg);
		CopyLinearBoneTableTo54(reinterpret_cast<const r5::v8::mstudiolinearbone_t* const>(input.getPtr()));
	}

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN4(g_model.pData);

	pHdr->length = g_model.pData - g_model.pBase;

	rigOut.write(g_model.pBase, pHdr->length);

	FreeModelBuf(g_model.pBase);
#endif

	g_model.stringTable.clear();

	printf("Finished converting model '%s', proceeding...\n\n", rawModelName.c_str());
}

//
// ConvertRMDL150To10
// Purpose: converts mdl data from rmdl v54 subversion 15 to rmdl v10 (Season 2/3)
// Note: v15 differs from v14 only in mstudiobodyparts_t (has 2 extra fields)
//
void ConvertRMDL150To10(char* pMDL, const std::string& pathIn, const std::string& pathOut)
{
	std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();

	printf("Converting model '%s' from version 54 (subversion 15) to version 54 (subversion 10)...\n", rawModelName.c_str());

	TIME_SCOPE(__FUNCTION__);

	rmem input(pMDL);

	r5::v140::studiohdr_t* oldHeader = input.get<r5::v140::studiohdr_t>();

	std::filesystem::path inputPath(pathIn);
	std::filesystem::path outputDir;
	std::string rmdlPath;

	if (pathOut != pathIn && !pathOut.empty())
	{
		rmdlPath = pathOut;
		outputDir = std::filesystem::path(pathOut).parent_path();
		std::filesystem::create_directories(outputDir);
	}
	else
	{
		outputDir = inputPath.parent_path() / "rmdlconv_out";
		std::filesystem::create_directories(outputDir);
		rmdlPath = (outputDir / inputPath.filename()).string();
	}

	printf("Output: %s\n", rmdlPath.c_str());
	std::ofstream out(rmdlPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	r5::v8::studiohdr_t* pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	ConvertStudioHdr_140(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	if (oldHeader->sourceFilenameOffset != 0 && oldHeader->boneindex > oldHeader->sourceFilenameOffset)
	{
		input.seek(oldHeader->sourceFilenameOffset, rseekdir::beg);
		const int sourceNameSize = oldHeader->boneindex - oldHeader->sourceFilenameOffset;

		input.read(g_model.pData, sourceNameSize);
		g_model.hdrV54()->sourceFilenameOffset = static_cast<int>(g_model.pData - g_model.pBase);

		g_model.pData += sourceNameSize;
	}

	BeginStringTable();

	std::string originalModelName = STRING_FROM_IDX(pMDL, oldHeader->sznameindex);

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
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, STRING_FROM_IDX(pMDL, oldHeader->surfacepropindex));
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_121((r5::v121::mstudiobone_t*)input.getPtr(), oldHeader->numbones, false);

	input.seek(oldHeader->localattachmentindex, rseekdir::beg);
	g_model.hdrV54()->localattachmentindex = CopyAttachmentsData_140((r5::v8::mstudioattachment_t*)input.getPtr(), oldHeader->numlocalattachments);

	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_121((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	input.seek(oldHeader->localseqindex, rseekdir::beg);
	ConvertAnims_140<r5::v121::mstudioanimdesc_t>((const char*)input.getPtr(), oldHeader->numlocalseq);

	// Key difference: use v150 bodyparts conversion for v15 models
	input.seek(oldHeader->bodypartindex, rseekdir::beg);
	ConvertBodyParts_150((r5::v150::mstudiobodyparts_t*)input.getPtr(), oldHeader->numbodyparts);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, false);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_121((r5::v8::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, false);

	ConvertUIPanelMeshes_140(oldHeader, input);

	input.seek(oldHeader->cdtextureindex, rseekdir::beg);
	void* pOldCDTextures = input.getPtr();

	MaterialShaderType_t* matTypes = nullptr;

	if (oldHeader->materialtypesindex > 0)
		matTypes = reinterpret_cast<MaterialShaderType_t*>(&pMDL[oldHeader->materialtypesindex]);

	input.seek(oldHeader->textureindex, rseekdir::beg);
	ConvertTextures_121((mstudiotexturedir_t*)pOldCDTextures, oldHeader->numcdtextures, (r5::v8::mstudiotexture_t*)input.getPtr(), oldHeader->numtextures, matTypes);

	input.seek(oldHeader->skinindex, rseekdir::beg);
	ConvertSkins_121(pMDL, (char*)input.getPtr(), oldHeader->numskinref, oldHeader->numskinfamilies);

	std::string keyValues = "mdlkeyvalue{prop_data{base \"\"}}\n";
	strcpy_s(g_model.pData, keyValues.length() + 1, keyValues.c_str());

	pHdr->keyvalueindex = g_model.pData - g_model.pBase;
	pHdr->keyvaluesize = IALIGN2(keyValues.length() + 1);

	g_model.pData += keyValues.length() + 1;
	ALIGN4(g_model.pData);

	input.seek(oldHeader->srcbonetransformindex, rseekdir::beg);
	g_model.hdrV54()->srcbonetransformindex = ConvertSrcBoneTransforms((mstudiosrcbonetransform_t*)input.getPtr(), oldHeader->numsrcbonetransform);

	if (oldHeader->linearboneindex && oldHeader->numbones > 1)
	{
		input.seek(oldHeader->linearboneindex, rseekdir::beg);
		CopyLinearBoneTableTo54(reinterpret_cast<const r5::v8::mstudiolinearbone_t* const>(input.getPtr()));
	}

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN64(g_model.pData);

	if (oldHeader->bvhOffset)
	{
		g_model.hdrV54()->bvhOffset = g_model.pData - g_model.pBase;

		input.seek(oldHeader->bvhOffset, rseekdir::beg);
		ConvertCollisionData_V120(oldHeader, (char*)input.getPtr());
	}

	pHdr->length = g_model.pData - g_model.pBase;

	out.write(g_model.pBase, pHdr->length);

	std::string vgFilePath = ChangeExtension(pathIn, "vg");

	if (FILE_EXISTS(vgFilePath))
	{
		uintmax_t vgInputSize = GetFileSize(vgFilePath);

		char* vgInputBuf = new char[vgInputSize];

		std::ifstream ifs(vgFilePath, std::ios::in | std::ios::binary);

		ifs.read(vgInputBuf, vgInputSize);

		if (*(int*)vgInputBuf == 'GVt0')
		{
			std::string vgOutputPath = (outputDir / std::filesystem::path(vgFilePath).filename()).string();
			printf("VG Output: %s\n", vgOutputPath.c_str());
			ConvertVGData_Rev3(vgInputBuf, vgFilePath, vgOutputPath);
		}

		delete[] vgInputBuf;
	}

	FreeModelBuf(g_model.pBase);

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

	std::string rrigPath = (outputDir / (inputPath.stem().string() + ".rrig")).string();
	std::ofstream rigOut(rrigPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	GenerateRigHdr_140(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	BeginStringTable();

	memcpy_s(&pHdr->name, 64, rigName.c_str(), min(rigName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, rigName.c_str());
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, STRING_FROM_IDX(pMDL, oldHeader->surfacepropindex));
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_121((r5::v121::mstudiobone_t*)input.getPtr(), oldHeader->numbones, true);

	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_121((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	input.seek(oldHeader->localseqindex, rseekdir::beg);
	ConvertAnims_140<r5::v121::mstudioanimdesc_t>((const char*)input.getPtr(), oldHeader->numlocalseq);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, false);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_121((r5::v8::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, true);

	input.seek(oldHeader->srcbonetransformindex, rseekdir::beg);
	g_model.hdrV54()->srcbonetransformindex = ConvertSrcBoneTransforms((mstudiosrcbonetransform_t*)input.getPtr(), oldHeader->numsrcbonetransform);

	if (oldHeader->linearboneindex && oldHeader->numbones > 1)
	{
		input.seek(oldHeader->linearboneindex, rseekdir::beg);
		CopyLinearBoneTableTo54(reinterpret_cast<const r5::v8::mstudiolinearbone_t* const>(input.getPtr()));
	}

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN4(g_model.pData);

	pHdr->length = g_model.pData - g_model.pBase;

	rigOut.write(g_model.pBase, pHdr->length);

	FreeModelBuf(g_model.pBase);

	g_model.stringTable.clear();

	printf("Finished converting model '%s', proceeding...\n\n", rawModelName.c_str());
}
