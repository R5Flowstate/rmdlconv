// Copyright (c) 2022, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/versions.h>
#include "../../collision/phy_parser.h"
#include "../../collision/phy_to_bvh4.h"
#include "../../collision/collision_generator.h"

//
// ConvertStudioHdr
// Purpose: converts the mdl v53 (Titanfall 2) studiohdr_t struct to rmdl v8 compatible (Apex Legends Season 0-6)
void ConvertStudioHdr(r5::v8::studiohdr_t* out, r2::studiohdr_t* hdr)
{
	out->id = 'TSDI';
	out->version = 54;
	
	out->checksum = hdr->checksum;

	// :)
	if ((_time64(NULL) % 69420) == 0)
		out->checksum = 0xDEADBEEF;

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

	// these will probably have to be modified at some point
	out->flags = hdr->flags;

	//-| begin count vars
	out->numbones = hdr->numbones;
	out->numbonecontrollers = hdr->numbonecontrollers;
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalanim = 0; // this is no longer used, force set to 0
	out->numlocalseq = hdr->numlocalseq;
	out->activitylistversion = hdr->activitylistversion;
	// eventsindexed --> materialtypesindex

	out->numtextures = hdr->numtextures;
	out->numcdtextures = hdr->numcdtextures;
	out->numskinref = hdr->numskinref;
	out->numskinfamilies = hdr->numskinfamilies;
	out->numbodyparts = hdr->numbodyparts;
	out->numlocalattachments = hdr->numlocalattachments;

	// next few comments are mostly for rigs

	//out->numlocalnodes = hdr->numlocalnodes;
	
	// skipping all the deprecated flex vars

	//out->numikchains = hdr->numikchains;
	//out->numruimeshes = hdr->numruimeshes;
	//out->numlocalposeparameters = hdr->numlocalposeparameters;
	out->keyvaluesize = hdr->keyvaluesize;
	//out->numlocalikautoplaylocks = hdr->numlocalikautoplaylocks; // cut?

	out->numincludemodels = -1;

	// why did i add this?
	//out->numincludemodels = hdr->numincludemodels;

	out->numsrcbonetransform = hdr->numsrcbonetransform;
	//-| end count vars

	//-| begin misc vars
	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->constdirectionallightdot = hdr->constdirectionallightdot;
	out->rootLOD = hdr->rootLOD;
	out->numAllowedRootLODs = hdr->numAllowedRootLODs;
	out->defaultFadeDist = hdr->defaultFadeDist;
	out->flVertAnimFixedPointScale = hdr->flVertAnimFixedPointScale;
	//-| end misc vars

	//-| begin for giggles
	/*out->vtxindex = -1;
	out->vvdindex = hdr->vtxSize;
	out->vvcindex = hdr->vtxSize + hdr->vvdSize;
	out->vphyindex = -123456;*/
	out->phyOffset = -123456;

	out->vtxSize = hdr->vtxSize;
	out->vvdSize = hdr->vvdSize;
	out->vvcSize = hdr->vvcSize;
	out->phySize = hdr->phySize;
	//-| end for giggles
}

void GenerateRigHdr(r5::v8::studiohdr_t* out, r2::studiohdr_t* hdr)
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

	// hard to tell if the first three are required
	out->constdirectionallightdot = hdr->constdirectionallightdot;
	out->rootLOD = hdr->rootLOD;
	out->numAllowedRootLODs = hdr->numAllowedRootLODs;
	out->defaultFadeDist = hdr->defaultFadeDist;
}

void ConvertBones_53(r2::mstudiobone_t* pOldBones, int numBones, bool isRig)
{
	printf("converting %i bones...\n", numBones);
	std::vector<r5::v8::mstudiobone_t*> proceduralBones;

	char* pBoneStart = g_model.pData;
	for (int i = 0; i < numBones; ++i)
	{
		r2::mstudiobone_t* oldBone = &pOldBones[i];

		r5::v8::mstudiobone_t* newBone = reinterpret_cast<r5::v8::mstudiobone_t*>(g_model.pData) + i;

		AddToStringTable((char*)newBone, &newBone->sznameindex, STRING_FROM_IDX(oldBone, oldBone->sznameindex));
		//newBone.name = boneName;

		AddToStringTable((char*)newBone, &newBone->surfacepropidx, STRING_FROM_IDX(oldBone, oldBone->surfacepropidx));
		//newBone.surfaceprop = surfaceprop;

		newBone->parent = oldBone->parent;
		memcpy(&newBone->bonecontroller, &oldBone->bonecontroller, sizeof(oldBone->bonecontroller));
		newBone->pos = oldBone->pos;
		newBone->quat = oldBone->quat;
		newBone->rot = oldBone->rot;
		newBone->scale = oldBone->scale;
		newBone->poseToBone = oldBone->poseToBone;
		newBone->qAlignment = oldBone->qAlignment;
		newBone->flags = oldBone->flags; // rigs should only have certain flags
		//newBone->proctype = oldBone->proctype;
		//newBone->procindex = oldBone->procindex;
		//newBone->physicsbone = oldBone->physicsbone;
		newBone->contents = oldBone->contents;
		newBone->surfacepropLookup = oldBone->surfacepropLookup;

		if (!isRig)
		{
			newBone->proctype = oldBone->proctype;
			newBone->procindex = oldBone->procindex;
			newBone->physicsbone = oldBone->physicsbone;

			if (oldBone->proctype != 0)
				proceduralBones.push_back(newBone);
		}
	}
	g_model.hdrV54()->boneindex = g_model.pData - g_model.pBase;
	g_model.pData += numBones * sizeof(r5::v8::mstudiobone_t);

	ALIGN4(g_model.pData);

	// rigs do not have proc bones
	if (isRig)
		return;

	if (proceduralBones.size() > 0)
		printf("converting %lld procedural bones (jiggle bones)...\n", proceduralBones.size());

	std::map<uint8_t, uint8_t> linearprocbones;

	for (auto bone : proceduralBones)
	{
		int boneid = ((char*)bone - pBoneStart) / sizeof(r5::v8::mstudiobone_t);
		r2::mstudiobone_t* oldBone = &pOldBones[boneid];
		mstudiojigglebone_t* oldJBone = PTR_FROM_IDX(mstudiojigglebone_t, oldBone, oldBone->procindex);

		r5::v8::mstudiojigglebone_t* jBone = reinterpret_cast<r5::v8::mstudiojigglebone_t*>(g_model.pData);

		bone->procindex = (char*)jBone - (char*)bone;
		jBone->flags = ConvertJiggleBoneFlags_R5(oldJBone->flags);
		jBone->bone = boneid;
		jBone->length = oldJBone->length;
		jBone->tipMass = oldJBone->tipMass;
		jBone->yawStiffness = oldJBone->yawStiffness;
		jBone->yawDamping = oldJBone->yawDamping;
		jBone->pitchStiffness = oldJBone->pitchStiffness;
		jBone->pitchDamping = oldJBone->pitchDamping;
		jBone->alongStiffness = oldJBone->alongStiffness;
		jBone->alongDamping = oldJBone->alongDamping;
		jBone->angleLimit = oldJBone->angleLimit;
		jBone->minYaw = oldJBone->minYaw;
		jBone->maxYaw = oldJBone->maxYaw;
		jBone->yawFriction = oldJBone->yawFriction;
		jBone->yawBounce = oldJBone->yawBounce;
		jBone->baseMass = oldJBone->baseMass;
		jBone->baseStiffness = oldJBone->baseStiffness;
		jBone->baseDamping = oldJBone->baseDamping;
		jBone->baseMinLeft = oldJBone->baseMinLeft;
		jBone->baseMaxLeft = oldJBone->baseMaxLeft;
		jBone->baseLeftFriction = oldJBone->baseLeftFriction;
		jBone->baseMinUp = oldJBone->baseMinUp;
		jBone->baseMaxUp = oldJBone->baseMaxUp;
		jBone->baseUpFriction = oldJBone->baseUpFriction;
		jBone->baseMinForward = oldJBone->baseMinForward;
		jBone->baseMaxForward = oldJBone->baseMaxForward;
		jBone->baseForwardFriction = oldJBone->baseForwardFriction;

		jBone->minPitch = oldJBone->minPitch;
		jBone->maxPitch = oldJBone->maxPitch;
		jBone->pitchFriction = oldJBone->pitchFriction;
		jBone->pitchBounce = oldJBone->pitchBounce;

		linearprocbones.emplace(jBone->bone, linearprocbones.size());

		g_model.pData += sizeof(r5::v8::mstudiojigglebone_t);
	}

	ALIGN4(g_model.pData);

	if (linearprocbones.size() == 0)
		return;

	g_model.hdrV54()->procBoneCount = linearprocbones.size();
	g_model.hdrV54()->procBoneTableOffset = g_model.pData - g_model.pBase;

	for (auto& it : linearprocbones)
	{
		*g_model.pData = it.first;
		g_model.pData += sizeof(uint8_t);
	}

	g_model.hdrV54()->linearProcBoneOffset = g_model.pData - g_model.pBase;

	for (int i = 0; i < numBones; i++)
	{
		*g_model.pData = linearprocbones.count(i) ? linearprocbones.find(i)->second : 0xff;
		g_model.pData += sizeof(uint8_t);
	}

	ALIGN4(g_model.pData);
}

void ConvertHitboxes_53(mstudiohitboxset_t* pOldHitboxSets, int numHitboxSets)
{
	printf("converting %i hitboxsets...\n", numHitboxSets);

	g_model.hdrV54()->hitboxsetindex = g_model.pData - g_model.pBase;

	mstudiohitboxset_t* hboxsetStart = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);
	for (int i = 0; i < numHitboxSets; ++i)
	{
		mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);

		memcpy(g_model.pData, oldhboxset, sizeof(mstudiohitboxset_t));

		AddToStringTable((char*)newhboxset, &newhboxset->sznameindex, STRING_FROM_IDX(oldhboxset, oldhboxset->sznameindex));

		g_model.pData += sizeof(mstudiohitboxset_t);
	}

	for (int i = 0; i < numHitboxSets; ++i)
	{
		mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = hboxsetStart + i;

		newhboxset->hitboxindex = g_model.pData - (char*)newhboxset;

		r2::mstudiobbox_t* oldHitboxes = reinterpret_cast<r2::mstudiobbox_t*>((char*)oldhboxset + oldhboxset->hitboxindex);

		for (int j = 0; j < newhboxset->numhitboxes; ++j)
		{
			r2::mstudiobbox_t* oldHitbox = oldHitboxes + j;
			r5::v8::mstudiobbox_t* newHitbox = reinterpret_cast<r5::v8::mstudiobbox_t*>(g_model.pData);

			memcpy(g_model.pData, oldHitbox, sizeof(r5::v8::mstudiobbox_t));

			AddToStringTable((char*)newHitbox, &newHitbox->szhitboxnameindex, STRING_FROM_IDX(oldHitbox, oldHitbox->szhitboxnameindex));
			AddToStringTable((char*)newHitbox, &newHitbox->hitdataGroupOffset, STRING_FROM_IDX(oldHitbox, oldHitbox->keyvalueindex));

			g_model.pData += sizeof(r5::v8::mstudiobbox_t);
		}
	}

	ALIGN4(g_model.pData);
}

void ConvertBodyParts_53(mstudiobodyparts_t* pOldBodyParts, int numBodyParts)
{
	printf("converting %i bodyparts...\n", numBodyParts);

	g_model.hdrV54()->bodypartindex = g_model.pData - g_model.pBase;

	mstudiobodyparts_t* bodypartStart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);
	for (int i = 0; i < numBodyParts; ++i)
	{
		mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

		memcpy(g_model.pData, oldbodypart, sizeof(mstudiobodyparts_t));

		AddToStringTable((char*)newbodypart, &newbodypart->sznameindex, STRING_FROM_IDX(oldbodypart, oldbodypart->sznameindex));

		g_model.pData += sizeof(mstudiobodyparts_t);
	}

	for (int i = 0; i < numBodyParts; ++i)
	{
		mstudiobodyparts_t* oldbodypart = &pOldBodyParts[i];
		mstudiobodyparts_t* newbodypart = bodypartStart + i;

		newbodypart->modelindex = g_model.pData - (char*)newbodypart;

		// pointer to old models (in .mdl) - use r2 for TF2 v53
		r2::mstudiomodel_t* oldModels = reinterpret_cast<r2::mstudiomodel_t*>((char*)oldbodypart + oldbodypart->modelindex);

		// pointer to start of new model data (in .rmdl)
		r5::v8::mstudiomodel_t* newModels = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);
		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r2::mstudiomodel_t* oldModel = oldModels + j;
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
			newModel->deprecated_numeyeballs = oldModel->deprecated_numeyeballs;
			newModel->deprecated_eyeballindex = oldModel->deprecated_eyeballindex;
			newModel->colorindex = oldModel->colorindex;
			newModel->uv2index = oldModel->uv2index;

			g_model.pData += sizeof(r5::v8::mstudiomodel_t);
		}

		for (int j = 0; j < newbodypart->nummodels; ++j)
		{
			r2::mstudiomodel_t* oldModel = oldModels + j;
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			newModel->meshindex = g_model.pData - (char*)newModel;

			// pointer to old meshes for this model (in .mdl)
			r2::mstudiomesh_t* oldMeshes = reinterpret_cast<r2::mstudiomesh_t*>((char*)oldModel + oldModel->meshindex);

			// pointer to new meshes for this model (in .rmdl)
			r5::v8::mstudiomesh_t* newMeshes = reinterpret_cast<r5::v8::mstudiomesh_t*>(g_model.pData);

			for (int k = 0; k < newModel->nummeshes; ++k)
			{
				r2::mstudiomesh_t* oldMesh = oldMeshes + k;
				r5::v8::mstudiomesh_t* newMesh = newMeshes + k;

				memcpy(newMesh, oldMesh, sizeof(r5::v8::mstudiomesh_t));

				newMesh->modelindex = (char*)newModel - (char*)newMesh;

				g_model.pData += sizeof(r5::v8::mstudiomesh_t);
			}
		}
	}

	ALIGN4(g_model.pData);
}

void ConvertIkChains_53(r2::mstudioikchain_t* pOldIkChains, int numIkChains, bool isRig)
{
	g_model.hdrV54()->ikchainindex = g_model.pData - g_model.pBase;

	if (!isRig)
		return;

	printf("converting %i ikchains...\n", numIkChains);

	int currentLinkCount = 0;
	std::vector<r5::v8::mstudioiklink_t> ikLinks;

	for (int i = 0; i < numIkChains; i++)
	{
		r2::mstudioikchain_t* oldChain = &pOldIkChains[i];
		r5::v8::mstudioikchain_t* newChain = reinterpret_cast<r5::v8::mstudioikchain_t*>(g_model.pData);

		AddToStringTable((char*)newChain, &newChain->sznameindex, STRING_FROM_IDX(oldChain, oldChain->sznameindex));

		newChain->linktype = oldChain->linktype;
		newChain->numlinks = oldChain->numlinks;
		newChain->linkindex = (sizeof(r5::v8::mstudioiklink_t) * currentLinkCount) + (sizeof(r5::v8::mstudioikchain_t) * (numIkChains - i));
		newChain->unk = oldChain->unk;

		g_model.pData += sizeof(r5::v8::mstudioikchain_t);

		currentLinkCount += oldChain->numlinks;
	}

	for (int i = 0; i < numIkChains; i++)
	{
		r2::mstudioikchain_t* oldChain = &pOldIkChains[i];

		for (int linkIdx = 0; linkIdx < oldChain->numlinks; linkIdx++)
		{
			mstudioiklink_t* oldLink = PTR_FROM_IDX(mstudioiklink_t, oldChain, oldChain->linkindex + (sizeof(mstudioiklink_t) * linkIdx));
			r5::v8::mstudioiklink_t* newLink = reinterpret_cast<r5::v8::mstudioiklink_t*>(g_model.pData);

			newLink->bone = oldLink->bone;
			newLink->kneeDir = oldLink->kneeDir;

			g_model.pData += sizeof(r5::v8::mstudioiklink_t);
		}
	}

	ALIGN4(g_model.pData);
}

void ConvertTextures_53(mstudiotexturedir_t* pCDTextures, int numCDTextures, r2::mstudiotexture_t* pOldTextures, int numTextures)
{
	// TODO[rexx]: maybe add old cdtexture parsing here if available, or give the user the option to manually set the material paths
	printf("converting %i textures...\n", numTextures);

	g_model.hdrV54()->textureindex = g_model.pData - g_model.pBase;
	for (int i = 0; i < numTextures; ++i)
	{
		r2::mstudiotexture_t* oldTexture = &pOldTextures[i];

		r5::v8::mstudiotexture_t* newTexture = reinterpret_cast<r5::v8::mstudiotexture_t*>(g_model.pData);

		const char* textureName = STRING_FROM_IDX(oldTexture, oldTexture->sznameindex);
		AddToStringTable((char*)newTexture, &newTexture->sznameindex, textureName);

		std::string texName = "material/" + std::string(textureName) + ".rpak";
		newTexture->textureGuid = HashString(texName.c_str());

		g_model.pData += sizeof(r5::v8::mstudiotexture_t);
	}

	ALIGN4(g_model.pData);

	// Material Shader Types
	// Used for the CMaterialSystem::FindMaterial call in CModelLoader::Studio_LoadModel
	// Must be set properly otherwise the materials will not be found
	g_model.hdrV54()->materialtypesindex = g_model.pData - g_model.pBase;

	MaterialShaderType_t materialType = MaterialShaderType_t::SKNP;
	if (g_model.hdrV54()->flags & STUDIOHDR_FLAGS_STATIC_PROP)
		materialType = MaterialShaderType_t::RGDP;

	memset(g_model.pData, materialType, numTextures);
	g_model.pData += numTextures;

	ALIGN4(g_model.pData); // align data to 4 bytes

	// Write static cdtexture data
	g_model.hdrV54()->cdtextureindex = g_model.pData - g_model.pBase;

	// i think cdtextures are mostly unused in r5 so use empty string
	AddToStringTable(g_model.pBase, (int*)g_model.pData, "");
	g_model.pData += sizeof(int);
}

void ConvertSkins_53(char* pOldSkinData, int numSkinRef, int numSkinFamilies)
{
	// TODO[rexx]: maybe add old cdtexture parsing here if available, or give the user the option to manually set the material paths
	printf("converting %i skins (%i skinrefs)...\n", numSkinFamilies, numSkinRef);

	g_model.hdrV54()->skinindex = g_model.pData - g_model.pBase;

	int skinIndexDataSize = sizeof(__int16) * numSkinRef * numSkinFamilies;
	memcpy(g_model.pData, pOldSkinData, skinIndexDataSize);
	g_model.pData += skinIndexDataSize;

	ALIGN4(g_model.pData);

	// write skin names
	// skin 0 is unnamed
	for (int i = 0; i < numSkinFamilies-1; ++i)
	{
		char* skinNameBuf = new char[32];
		sprintf_s(skinNameBuf, 32, "skin%i", i);
		AddToStringTable(g_model.pBase, (int*)g_model.pData, skinNameBuf);

		g_model.pData += 4;
	}

	ALIGN4(g_model.pData);
}

// i lied it doesnt convert anything it just creates a default ref anim
void ConvertAnims_53()
{
	r5::v8::mstudioseqdesc_t* seqdesc = reinterpret_cast<r5::v8::mstudioseqdesc_t*>(g_model.pData);

	g_model.hdrV54()->localseqindex = g_model.pData - g_model.pBase;
	g_model.hdrV54()->numlocalseq = 1;

	seqdesc->baseptr = 0;
	AddToStringTable((char*)seqdesc, &seqdesc->szlabelindex, "ref");
	AddToStringTable((char*)seqdesc, &seqdesc->szactivitynameindex, "");

	seqdesc->activity = -1;

	seqdesc->bbmin = g_model.hdrV54()->mins;
	seqdesc->bbmax = g_model.hdrV54()->maxs;
	seqdesc->groupsize[0] = 1;
	seqdesc->groupsize[1] = 1;
	seqdesc->paramindex[0] = -1;
	seqdesc->paramindex[1] = -1;
	seqdesc->fadeintime = 0.2;
	seqdesc->fadeouttime = 0.2;

	// needs to be adjusted if adding more than one anim
	seqdesc->eventindex = sizeof(*seqdesc);
	seqdesc->autolayerindex = sizeof(*seqdesc);
	seqdesc->weightlistindex = sizeof(*seqdesc);

	g_model.pData += sizeof(r5::v8::mstudioseqdesc_t);

	// weightlist
	for (int i = 0; i < g_model.hdrV54()->numbones; ++i)
	{
		*reinterpret_cast<float*>(g_model.pData) = 1.0f;
		g_model.pData += sizeof(int);
	}

	seqdesc->animindexindex = g_model.pData - (char*)seqdesc;

	// blend
	*reinterpret_cast<int*>(g_model.pData) = seqdesc->animindexindex + sizeof(int);
	g_model.pData += sizeof(int);

	// add animdesc
	r5::v8::mstudioanimdesc_t* animdesc = reinterpret_cast<r5::v8::mstudioanimdesc_t*>(g_model.pData);

	AddToStringTable((char*)animdesc, &animdesc->sznameindex, "@ref");
	animdesc->fps = 30;
	animdesc->flags = STUDIO_ALLZEROS; // no way!!!

	g_model.pData += sizeof(r5::v8::mstudioanimdesc_t);
	ALIGN4(g_model.pData);

}

#define FILEBUFSIZE (32 * 1024 * 1024)

//
// ConvertMDL53To54
// Purpose: converts mdl data from mdl v53 (Titanfall 2) to rmdl v9 (Apex Legends Season 2/3)
//
void ConvertMDL53To54(char* pMDL, const std::string& pathIn, const std::string& pathOut)
{
	std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();

	printf("Converting model '%s' from version 53 to version 54 (subversion 10)...\n", rawModelName.c_str());

	TIME_SCOPE(__FUNCTION__);

	rmem input(pMDL);

	r2::studiohdr_t* oldHeader = input.get<r2::studiohdr_t>();

	std::unique_ptr<char[]> vtxBuf;
	if (oldHeader->vtxSize > 0)
	{
		vtxBuf = std::unique_ptr<char[]>(new char[oldHeader->vtxSize]);

		input.seek(oldHeader->vtxOffset, rseekdir::beg);
		input.read(vtxBuf.get(), oldHeader->vtxSize);
	}
	else
	{
		printf("Skipping model '%s' as it has no vertex data (animation models are not supported)...\n\n", rawModelName.c_str());
		return;
	}

	std::unique_ptr<char[]> vvdBuf;
	if (oldHeader->vvdSize > 0)
	{
		vvdBuf = std::unique_ptr<char[]>(new char[oldHeader->vvdSize]);

		input.seek(oldHeader->vvdOffset, rseekdir::beg);
		input.read(vvdBuf.get(), oldHeader->vvdSize);
	}
	else
	{
		printf("Skipping model '%s' as it has no vertex data (animation models are not supported)...\n\n", rawModelName.c_str());
		return;
	}

	std::unique_ptr<char[]> vphyBuf;
	if (oldHeader->phySize > 0)
	{
		vphyBuf = std::unique_ptr<char[]>(new char[oldHeader->phySize]);

		input.seek(oldHeader->phyOffset, rseekdir::beg);
		input.read(vphyBuf.get(), oldHeader->phySize);
	}

	std::unique_ptr<char[]> vvcBuf;
	if (oldHeader->vvcSize > 0)
	{
		vvcBuf = std::unique_ptr<char[]>(new char[oldHeader->vvcSize]);

		input.seek(oldHeader->vvcOffset, rseekdir::beg);
		input.read(vvcBuf.get(), oldHeader->vvcSize);
	}

	std::string rmdlPath = ChangeExtension(pathOut, "rmdl");
	std::ofstream out(rmdlPath, std::ios::out | std::ios::binary);

	// allocate temp file buffer
	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	// convert mdl hdr
	r5::v8::studiohdr_t* pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	ConvertStudioHdr(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// init string table so we can use 
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
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, STRING_FROM_IDX(pMDL, oldHeader->unkStringOffset));

	// convert bones and jigglebones
	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_53((r2::mstudiobone_t*)input.getPtr(), oldHeader->numbones, false);

	// convert attachments
	input.seek(oldHeader->localattachmentindex, rseekdir::beg);
	g_model.hdrV54()->localattachmentindex = ConvertAttachmentTo54((mstudioattachment_t*)input.getPtr(), oldHeader->numlocalattachments);

	// convert hitboxsets and hitboxes
	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_53((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	// copy bonebyname table (bone ids sorted alphabetically by name)
	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	ConvertAnims_53();

	// convert bodyparts, models, and meshes
	input.seek(oldHeader->bodypartindex, rseekdir::beg);
	ConvertBodyParts_53((mstudiobodyparts_t*)input.getPtr(), oldHeader->numbodyparts);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, false);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_53((r2::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, false);

	// get cdtextures pointer for converting textures
	input.seek(oldHeader->cdtextureindex, rseekdir::beg);
	void* pOldCDTextures = input.getPtr();

	// convert textures
	input.seek(oldHeader->textureindex, rseekdir::beg);
	ConvertTextures_53((mstudiotexturedir_t*)pOldCDTextures, oldHeader->numcdtextures, (r2::mstudiotexture_t*)input.getPtr(), oldHeader->numtextures);

	// convert skin data
	input.seek(oldHeader->skinindex, rseekdir::beg);
	ConvertSkins_53((char*)input.getPtr(), oldHeader->numskinref, oldHeader->numskinfamilies);

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
		ConvertLinearBoneTableTo54((mstudiolinearbone_t*)input.getPtr(), (char*)input.getPtr() + sizeof(mstudiolinearbone_t));
	}

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN4(g_model.pData);

	// Generate BVH4 collision - prefer visual mesh (LOD0), fallback to PHY
	{
		printf("generating collision...\n");

		collision::GenerationResult collResult;
		bool collisionGenerated = false;

		// =========================================================================
		// PRIMARY: Generate collision from visual mesh (LOD0) - more reliable
		// =========================================================================
		if (vtxBuf && vvdBuf)
		{
			printf("  generating collision from LOD0 visual mesh...\n");

			// Read VVD header to get vertex data
			vvd::vertexFileHeader_t* vvdHeader = reinterpret_cast<vvd::vertexFileHeader_t*>(vvdBuf.get());

			// Get vertex positions from VVD
			std::vector<float> vertices;
			std::vector<uint32_t> indices;

			// Get vertices from VVD file
			vvd::mstudiovertex_t* vvdVerts = reinterpret_cast<vvd::mstudiovertex_t*>(vvdBuf.get() + vvdHeader->vertexDataStart);
			int numVerts = vvdHeader->numLODVertexes[0]; // Use LOD0

			vertices.reserve(numVerts * 3);
			for (int i = 0; i < numVerts; i++)
			{
				vertices.push_back(vvdVerts[i].m_vecPosition.x);
				vertices.push_back(vvdVerts[i].m_vecPosition.y);
				vertices.push_back(vvdVerts[i].m_vecPosition.z);
			}

			// Get indices from VTX file, using MDL mesh structure to get vertex offsets
			OptimizedModel::FileHeader_t* vtxHeader = reinterpret_cast<OptimizedModel::FileHeader_t*>(vtxBuf.get());
			OptimizedModel::BodyPartHeader_t* bodyParts = reinterpret_cast<OptimizedModel::BodyPartHeader_t*>(vtxBuf.get() + vtxHeader->bodyPartOffset);

			// Track cumulative model vertex base across all models
			int modelVertexBase = 0;

			// Get MDL bodyparts array for accessing mesh vertex offsets
			mstudiobodyparts_t* mdlBodyparts = reinterpret_cast<mstudiobodyparts_t*>((char*)oldHeader + oldHeader->bodypartindex);

			for (int bp = 0; bp < vtxHeader->numBodyParts; bp++)
			{
				OptimizedModel::BodyPartHeader_t* bodyPart = &bodyParts[bp];
				OptimizedModel::ModelHeader_t* vtxModels = reinterpret_cast<OptimizedModel::ModelHeader_t*>((char*)bodyPart + bodyPart->modelOffset);

				// Get corresponding MDL bodypart to access mesh vertex offsets
				mstudiobodyparts_t* mdlBodypart = &mdlBodyparts[bp];
				r2::mstudiomodel_t* mdlModels = reinterpret_cast<r2::mstudiomodel_t*>((char*)mdlBodypart + mdlBodypart->modelindex);

				for (int m = 0; m < bodyPart->numModels; m++)
				{
					OptimizedModel::ModelHeader_t* vtxModel = &vtxModels[m];
					OptimizedModel::ModelLODHeader_t* lods = reinterpret_cast<OptimizedModel::ModelLODHeader_t*>((char*)vtxModel + vtxModel->lodOffset);

					// Get corresponding MDL model
					r2::mstudiomodel_t* mdlModel = &mdlModels[m];

					// Only use LOD0
					OptimizedModel::ModelLODHeader_t* lod = &lods[0];
					OptimizedModel::MeshHeader_t* vtxMeshes = reinterpret_cast<OptimizedModel::MeshHeader_t*>((char*)lod + lod->meshOffset);

					// Get MDL meshes for this model (use r2 mesh type for TF2)
					r2::mstudiomesh_t* mdlMeshes = reinterpret_cast<r2::mstudiomesh_t*>((char*)mdlModel + mdlModel->meshindex);

					for (int me = 0; me < lod->numMeshes; me++)
					{
						OptimizedModel::MeshHeader_t* vtxMesh = &vtxMeshes[me];
						OptimizedModel::StripGroupHeader_t* stripGroups = reinterpret_cast<OptimizedModel::StripGroupHeader_t*>((char*)vtxMesh + vtxMesh->stripGroupHeaderOffset);

						// Get the mesh's vertex offset from MDL
						r2::mstudiomesh_t* mdlMesh = &mdlMeshes[me];
						int meshVertexBase = modelVertexBase + mdlMesh->vertexoffset;

						for (int sg = 0; sg < vtxMesh->numStripGroups; sg++)
						{
							OptimizedModel::StripGroupHeader_t* stripGroup = &stripGroups[sg];
							OptimizedModel::Vertex_t* vtxVerts = reinterpret_cast<OptimizedModel::Vertex_t*>((char*)stripGroup + stripGroup->vertOffset);
							uint16_t* vtxIndices = reinterpret_cast<uint16_t*>((char*)stripGroup + stripGroup->indexOffset);
							OptimizedModel::StripHeader_t* strips = reinterpret_cast<OptimizedModel::StripHeader_t*>((char*)stripGroup + stripGroup->stripOffset);

							for (int s = 0; s < stripGroup->numStrips; s++)
							{
								OptimizedModel::StripHeader_t* strip = &strips[s];

								// Check strip type - STRIP_IS_TRILIST = 0x01
								if (strip->flags & OptimizedModel::STRIP_IS_TRILIST)
								{
									// Triangle list: read triplets sequentially
									for (int i = 0; i < strip->numIndices; i += 3)
									{
										int idx0 = vtxIndices[strip->indexOffset + i];
										int idx1 = vtxIndices[strip->indexOffset + i + 1];
										int idx2 = vtxIndices[strip->indexOffset + i + 2];

										// Add mesh vertex base to get correct global VVD index
										indices.push_back(meshVertexBase + vtxVerts[idx0].origMeshVertID);
										indices.push_back(meshVertexBase + vtxVerts[idx1].origMeshVertID);
										indices.push_back(meshVertexBase + vtxVerts[idx2].origMeshVertID);
									}
								}
								else
								{
									// Triangle strip: each new index forms a triangle with previous two
									for (int i = 0; i < strip->numIndices - 2; i++)
									{
										int idx0 = vtxIndices[strip->indexOffset + i];
										int idx1 = vtxIndices[strip->indexOffset + i + 1];
										int idx2 = vtxIndices[strip->indexOffset + i + 2];

										// Alternate winding order for triangle strips
										if (i & 1)
										{
											std::swap(idx1, idx2);
										}

										// Skip degenerate triangles (used for strip stitching)
										if (idx0 == idx1 || idx1 == idx2 || idx0 == idx2)
											continue;

										// Add mesh vertex base to get correct global VVD index
										indices.push_back(meshVertexBase + vtxVerts[idx0].origMeshVertID);
										indices.push_back(meshVertexBase + vtxVerts[idx1].origMeshVertID);
										indices.push_back(meshVertexBase + vtxVerts[idx2].origMeshVertID);
									}
								}
							}
						}
					}

					// Move to next model's vertex range
					modelVertexBase += mdlModel->numvertices;
				}
			}

			if (!vertices.empty() && !indices.empty())
			{
				uint32_t numVerts = static_cast<uint32_t>(vertices.size() / 3);
				uint32_t numTris = static_cast<uint32_t>(indices.size() / 3);

				printf("  collision input: %u vertices, %u triangles\n", numVerts, numTris);

				// Validate indices
				uint32_t maxIdx = 0;
				bool hasInvalidIdx = false;
				for (size_t i = 0; i < indices.size(); i++)
				{
					if (indices[i] >= numVerts)
					{
						if (!hasInvalidIdx)
						{
							printf("  WARNING: triangle index %u at position %zu is out of bounds (max=%u)\n",
								indices[i], i, numVerts - 1);
						}
						hasInvalidIdx = true;
					}
					if (indices[i] > maxIdx) maxIdx = indices[i];
				}

				if (!hasInvalidIdx)
				{
					collResult = collision::QuickGenerate(
						vertices.data(),
						numVerts,
						indices.data(),
						numTris
					);
					collisionGenerated = collResult.success;

					if (collisionGenerated)
					{
						printf("  visual mesh collision generated: %u nodes, %u triangles\n",
							collResult.nodeCount, collResult.triangleCount);
					}
					else
					{
						printf("  failed to generate visual mesh collision: %s\n", collResult.errorMessage.c_str());
					}
				}
				else
				{
					printf("  skipping visual mesh collision due to invalid indices\n");
				}
			}
			else
			{
				printf("  no mesh data available for collision\n");
			}
		}

		// =========================================================================
		// FALLBACK: Generate collision from PHY data if visual mesh failed
		// =========================================================================
		if (!collisionGenerated && vphyBuf && oldHeader->phySize > 0)
		{
			printf("  falling back to PHY collision conversion...\n");

			// Parse PHY format
			collision::ParsedPHYData phyData =
				collision::PHYParser::Parse(vphyBuf.get(), oldHeader->phySize);

			if (phyData.valid && !phyData.solids.empty())
			{
				// Compute WORLD-space bone matrices
				r5::v8::mstudiobone_t* bones = reinterpret_cast<r5::v8::mstudiobone_t*>((char*)pHdr + pHdr->boneindex);

				std::vector<matrix3x4_t> localBones(pHdr->numbones);
				std::vector<matrix3x4_t> worldBones(pHdr->numbones);
				std::vector<float> bonePoses(pHdr->numbones * 12);

				// First pass: compute local matrices from bone quat/pos
				for (int i = 0; i < pHdr->numbones; i++)
				{
					r5::v8::mstudiobone_t* bone = &bones[i];
					QuaternionMatrix(bone->quat, bone->pos, localBones[i]);
				}

				// Second pass: chain parent transforms to get world matrices
				for (int i = 0; i < pHdr->numbones; i++)
				{
					r5::v8::mstudiobone_t* bone = &bones[i];

					if (bone->parent < 0)
					{
						worldBones[i] = localBones[i];
					}
					else
					{
						ConcatTransforms(worldBones[bone->parent], localBones[i], worldBones[i]);
					}
				}

				// Copy world matrices to flat array
				for (int i = 0; i < pHdr->numbones; i++)
				{
					float* dst = &bonePoses[i * 12];
					for (int row = 0; row < 3; row++)
					{
						for (int col = 0; col < 4; col++)
						{
							dst[row * 4 + col] = worldBones[i][row][col];
						}
					}
				}

				// Configure conversion
				collision::PHYConversionConfig convConfig;
				convConfig.bvhConfig.maxTrianglesPerLeaf = 4;
				convConfig.bvhConfig.defaultSurfaceProp = "default";
				convConfig.bvhConfig.contentsMask = oldHeader->contents;
				convConfig.debugOutput = false;
				convConfig.validateTransforms = true;

				// Convert PHY → BVH4
				collResult = collision::PHYToBVH4Converter::Convert(phyData, bonePoses.data(), pHdr->numbones, convConfig);
				collisionGenerated = collResult.success;

				if (collisionGenerated)
				{
					printf("  PHY -> BVH4 complete: %u solids -> %u triangles, %u nodes\n",
						(uint32_t)phyData.solids.size(),
						collResult.triangleCount,
						collResult.nodeCount);
				}
				else
				{
					printf("  ERROR: PHY BVH4 generation failed: %s\n", collResult.errorMessage.c_str());
				}
			}
			else
			{
				printf("  ERROR: PHY parsing failed: %s\n", phyData.errorMessage.c_str());
			}
		}

		// =========================================================================
		// Write collision data if generated successfully
		// =========================================================================
		if (collisionGenerated && !collResult.collisionData.empty())
		{
			ALIGN16(g_model.pData);
			pHdr->bvhOffset = g_model.pData - g_model.pBase;
			memcpy(g_model.pData, collResult.collisionData.data(), collResult.collisionData.size());
			g_model.pData += collResult.collisionData.size();

			// Set collision bounds
			pHdr->mins = Vector(
				collResult.boundsMin[0],
				collResult.boundsMin[1],
				collResult.boundsMin[2]
			);
			pHdr->maxs = Vector(
				collResult.boundsMax[0],
				collResult.boundsMax[1],
				collResult.boundsMax[2]
			);

			printf("  collision written: %zu bytes\n", collResult.collisionData.size());
		}
		else
		{
			// No collision generated - use hull bounds
			pHdr->mins = pHdr->hull_min;
			pHdr->maxs = pHdr->hull_max;
			pHdr->bvhOffset = 0;

			printf("  WARNING: no collision generated, using hull bounds\n");
		}
	}

	pHdr->length = g_model.pData - g_model.pBase;

	out.write(g_model.pBase, pHdr->length);

	// now that rmdl is fully converted, convert vtx/vvd/vvc to VG
	CreateVGFile(ChangeExtension(pathOut, "vg"), pHdr, vtxBuf.get(), vvdBuf.get(), vvcBuf.get(), nullptr);

	// Write external .phy file for ragdoll physics
	if (vphyBuf && oldHeader->phySize > 0)
	{
		printf("writing external .phy file for ragdoll physics...\n");

		std::string phyPath = ChangeExtension(pathOut, "phy");
		std::ofstream phyOut(phyPath, std::ios::out | std::ios::binary);

		if (phyOut.is_open())
		{
			phyOut.write(vphyBuf.get(), oldHeader->phySize);
			phyOut.close();

			printf("  wrote external .phy file (%d bytes)\n", oldHeader->phySize);

			// Header already configured:
			// - pHdr->phyOffset = -123456 (external file sentinel)
			// - pHdr->phySize > 0 (tells Apex to load .phy)
		}
		else
		{
			printf("  ERROR: failed to create .phy file at '%s'\n", phyPath.c_str());

			// Disable PHY to avoid crash
			pHdr->phySize = 0;
		}
	}
	else
	{
		printf("  No PHY data for ragdoll physics\n");
		pHdr->phySize = 0; // Ensure PHY is disabled
	}

	// now delete rmdl buffer so we can write the rig
	FreeModelBuf(g_model.pBase);

	///////////////
	// ANIM RIGS //
	///////////////
	// TODO[rexx]: this ought to be moved to a separate function when possible

	std::string rigName = originalModelName;
	if (rigName.rfind("animrig/", 0) != 0)
		rigName = "animrig/" + rigName;
	if (EndsWith(rigName, ".mdl"))
	{
		rigName = rigName.substr(0, rigName.length() - 4);
		rigName += ".rrig";
	}

	printf("Creating rig from model...\n");

	std::string rrigPath = ChangeExtension(pathOut, "rrig");
	std::ofstream rigOut(rrigPath, std::ios::out | std::ios::binary);

	g_model.pBase = AllocModelBuf(FILEBUFSIZE);
	g_model.pData = g_model.pBase;

	// generate rig
	pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	GenerateRigHdr(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// reset string table for rig
	BeginStringTable();

	memcpy_s(&pHdr->name, 64, rigName.c_str(), min(rigName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, rigName.c_str());
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, STRING_FROM_IDX(pMDL, oldHeader->surfacepropindex));
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, STRING_FROM_IDX(pMDL, oldHeader->unkStringOffset));

	// convert bones and jigglebones
	input.seek(oldHeader->boneindex, rseekdir::beg);
	ConvertBones_53((r2::mstudiobone_t*)input.getPtr(), oldHeader->numbones, true);

	// convert attachments
	input.seek(oldHeader->localattachmentindex, rseekdir::beg);
	g_model.hdrV54()->localattachmentindex = ConvertAttachmentTo54((mstudioattachment_t*)input.getPtr(), oldHeader->numlocalattachments);

	// convert hitboxsets and hitboxes
	input.seek(oldHeader->hitboxsetindex, rseekdir::beg);
	ConvertHitboxes_53((mstudiohitboxset_t*)input.getPtr(), oldHeader->numhitboxsets);

	// copy bonebyname table (bone ids sorted alphabetically by name)
	input.seek(oldHeader->bonetablebynameindex, rseekdir::beg);
	input.read(g_model.pData, g_model.hdrV54()->numbones);

	g_model.hdrV54()->bonetablebynameindex = g_model.pData - g_model.pBase;
	g_model.pData += g_model.hdrV54()->numbones;

	ALIGN4(g_model.pData);

	input.seek(oldHeader->localposeparamindex, rseekdir::beg);
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams((mstudioposeparamdesc_t*)input.getPtr(), oldHeader->numlocalposeparameters, true);

	input.seek(oldHeader->ikchainindex, rseekdir::beg);
	ConvertIkChains_53((r2::mstudioikchain_t*)input.getPtr(), oldHeader->numikchains, true);
	ALIGN4(g_model.pData);

	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN4(g_model.pData);

	pHdr->length = g_model.pData - g_model.pBase;

	rigOut.write(g_model.pBase, pHdr->length);

	FreeModelBuf(g_model.pBase);
	//printf("Done!\n");


	g_model.stringTable.clear(); // cleanup string table

	printf("Finished converting model '%s', proceeding...\n\n", rawModelName.c_str());
}
