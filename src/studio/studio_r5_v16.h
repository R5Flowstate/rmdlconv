// Copyright (c) 2024, rexx
// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)
//
// v16 (Season 13-18) model structures for Apex Legends
// Based on structures from binary templates and rmdl_v16.bt
//
// Key differences from v19.1:
// - studiohdr_t is 224 bytes (0xE0) vs v19.1's 228 bytes (0xE4)
// - mstudiolinearbone_t has 7 indices (14 bytes) - missing qalignmentindex/scaleindex
// - Bone structure uses FEATURE_FORCELINEARBONE (16-byte mstudiobonedata_t)
// - Animation uses rseq v11 (vs v19.1's rseq v12.1)
//
// NOTE: Like v19.1, v16 uses direct byte offsets. FIX_OFFSET is identity.
//

#pragma once
#include <studio/studio.h>

// Macro for offset conversion - v16 uses direct byte offsets
#ifndef FIX_OFFSET
#define FIX_OFFSET(x) (x)
#endif

#pragma pack(push, 2)
namespace r5
{
	namespace v160
	{
		//
		// Hardware Data (VG groups)
		//
		struct studio_hw_groupdata_t
		{
			int dataOffset;           // offset to this section in compressed vg
			int dataSizeCompressed;   // compressed size of data in vg
			int dataSizeDecompressed; // decompressed size of data in vg
			int dataCompression;      // compressionType_t

			uint8_t lodIndex;
			uint8_t lodCount;         // number of lods contained within this group
			uint8_t lodMap;           // lods in this group, each bit is a lod
		};

		//
		// Bone Header (12 bytes)
		// Contains name, surface prop, physics bone info
		//
		struct mstudiobonehdr_t
		{
			int contents;              // See BSPFlags.h for the contents flags

			uint8_t unk_4;

			uint8_t surfacepropLookup; // written on compile in v54
			uint16_t surfacepropidx;   // index into string table for property name

			uint16_t physicsbone;      // index into physically simulated bone

			uint16_t sznameindex;
			inline char* const pszName() const { return ((char*)this + FIX_OFFSET(sznameindex)); }
		};
		static_assert(sizeof(mstudiobonehdr_t) == 12, "mstudiobonehdr_t size mismatch");

		//
		// Bone Data (128 bytes) - v16 format WITHOUT FEATURE_FORCELINEARBONE
		// Contains inline pose data + parent, flags, proc info
		// NOTE: Pose data is ALSO in linear bone arrays for quick access
		//
		struct mstudiobonedata_t
		{
			matrix3x4_t poseToBone;    // 48 bytes (offset 0)
			Quaternion qAlignment;     // 16 bytes (offset 48)

			// default values
			Vector pos;                // 12 bytes (offset 64)
			Quaternion quat;           // 16 bytes (offset 76)
			RadianEuler rot;           // 12 bytes (offset 92)
			Vector scale;              // 12 bytes (offset 104)

			short parent;              // 2 bytes (offset 116) - parent bone

			uint16_t unk_76;           // 2 bytes (offset 118)

			int flags;                 // 4 bytes (offset 120)

			uint8_t collisionIndex;    // 1 byte (offset 124) - index into collision sections, 0xFF if none

			uint8_t proctype;          // 1 byte (offset 125)
			uint16_t procindex;        // 2 bytes (offset 126) - procedural rule offset
		};
		static_assert(sizeof(mstudiobonedata_t) == 128, "mstudiobonedata_t size mismatch");

		//
		// Linear Bone Data (14 bytes header + arrays)
		// Contains arrays of bone transform data indexed by bone
		// NOTE: v16 does NOT have qalignmentindex or scaleindex
		//
		struct mstudiolinearbone_t
		{
			uint16_t numbones;

			uint16_t flagsindex;
			inline int* pFlags(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<int*>((char*)this + FIX_OFFSET(flagsindex)) + i;
			}
			inline int flags(int i) const { return *pFlags(i); }

			uint16_t parentindex;
			// NOTE: v16 stores parent indices as int16_t, not int32_t like v19.1
			inline int16_t* pParent(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<int16_t*>((char*)this + FIX_OFFSET(parentindex)) + i;
			}

			uint16_t posindex;
			inline const Vector* pPos(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<Vector*>((char*)this + FIX_OFFSET(posindex)) + i;
			}

			uint16_t quatindex;
			inline const Quaternion* pQuat(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<Quaternion*>((char*)this + FIX_OFFSET(quatindex)) + i;
			}

			uint16_t rotindex;
			inline const RadianEuler* pRot(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<RadianEuler*>((char*)this + FIX_OFFSET(rotindex)) + i;
			}

			uint16_t posetoboneindex;
			inline const matrix3x4_t* pPoseToBone(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<matrix3x4_t*>((char*)this + FIX_OFFSET(posetoboneindex)) + i;
			}

			// NOTE: v16 does NOT have qalignmentindex or scaleindex
			// These are only present in v19.1+
		};

		//
		// Attachment (56 bytes)
		//
		struct mstudioattachment_t
		{
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }
			uint16_t localbone;        // parent bone
			int flags;

			matrix3x4_t local;         // attachment point
		};
		static_assert(sizeof(mstudioattachment_t) == 56, "mstudioattachment_t size mismatch");

		//
		// Hitbox (32 bytes with 2-byte packing)
		//
		struct mstudiobbox_t
		{
			uint16_t bone;
			uint16_t group;            // intersection group

			Vector bbmin;              // bounding box
			Vector bbmax;

			uint16_t szhitboxnameindex;
			inline const char* const pszHitboxName() const {
				if (szhitboxnameindex == 0)
					return "";
				return reinterpret_cast<const char* const>(this) + FIX_OFFSET(szhitboxnameindex);
			}

			uint16_t hitdataGroupOffset; // hit_data group in keyvalues
		};
		static_assert(sizeof(mstudiobbox_t) == 32, "mstudiobbox_t size mismatch");

		//
		// Hitbox Set (6 bytes)
		//
		struct mstudiohitboxset_t
		{
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }

			uint16_t numhitboxes;
			uint16_t hitboxindex;
			const mstudiobbox_t* const pHitbox(const int i) const {
				return reinterpret_cast<const mstudiobbox_t* const>((char*)this + FIX_OFFSET(hitboxindex)) + i;
			}
		};
		static_assert(sizeof(mstudiohitboxset_t) == 6, "mstudiohitboxset_t size mismatch");

		//
		// IK Structures
		//
		struct mstudioiklock_t
		{
			uint16_t chain;
			uint16_t flags;
			float flPosWeight;
			float flLocalQWeight;
		};

		struct mstudioiklink_t
		{
			uint16_t bone;
			uint16_t pad;
			Vector kneeDir;
		};

		struct mstudioikchain_t
		{
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }

			uint16_t linktype;
			uint16_t numlinks;
			uint16_t linkindex;
			inline const mstudioiklink_t* pLink(int i) const {
				return reinterpret_cast<const mstudioiklink_t*>((const char*)this + FIX_OFFSET(linkindex)) + i;
			}

			float unk_10;  // default value: 0.707f
		};

		struct mstudioposeparamdesc_t
		{
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }

			uint16_t flags;
			float start;
			float end;
			float loop;
		};

		//
		// Sequence/Animation Structures (rseq v11)
		//
		struct mstudioseqdesc_t
		{
			uint16_t szlabelindex;
			inline const char* pszLabel() const { return reinterpret_cast<const char*>(this) + FIX_OFFSET(szlabelindex); }

			uint16_t szactivitynameindex;
			inline const char* pszActivity() const { return reinterpret_cast<const char*>(this) + FIX_OFFSET(szactivitynameindex); }

			int flags;

			uint16_t activity;
			uint16_t actweight;

			uint16_t numevents;
			uint16_t eventindex;

			Vector bbmin;
			Vector bbmax;

			uint16_t numblends;

			uint16_t animindexindex;

			short paramindex[2];
			float paramstart[2];
			float paramend[2];

			float fadeintime;
			float fadeouttime;

			uint16_t localentrynode;
			uint16_t localexitnode;

			uint16_t numikrules;

			uint16_t numautolayers;
			uint16_t autolayerindex;

			uint16_t weightlistindex;

			uint8_t groupsize[2];

			uint16_t posekeyindex;

			uint16_t numiklocks;
			uint16_t iklockindex;

			uint16_t unk_5C;

			uint16_t cycleposeindex;

			uint16_t activitymodifierindex;
			uint16_t numactivitymodifiers;

			int ikResetMask;
			int unk_68;

			uint16_t weightFixupOffset;
			uint16_t weightFixupCount;
		};
		static_assert(sizeof(mstudioseqdesc_t) == 112, "v160 mstudioseqdesc_t size mismatch - expected 112 bytes (rseq v11)");

		struct mstudioevent_t
		{
			float cycle;
			int event;
			int type;

			int unk_C;

			uint16_t optionsindex;
			uint16_t szeventindex;
		};

		struct mstudioautolayer_t
		{
			uint64_t assetSequence;  // hashed aseq guid asset
			short iSequence;
			short iPose;

			int flags;
			float start;
			float peak;
			float tail;
			float end;
		};

		struct mstudioactivitymodifier_t
		{
			uint16_t sznameindex;
			bool negate;
		};

		struct mstudiocompressedikerror_t
		{
			uint16_t sectionframes;
			float scale[6];
		};

		struct mstudioikrule_t
		{
			short chain;
			short bone;
			char type;
			char slot;

			mstudiocompressedikerror_t compressedikerror;
			uint16_t compressedikerrorindex;

			short iStart;
			uint16_t ikerrorindex;

			uint16_t szattachmentindex;

			float start;
			float peak;
			float tail;
			float end;

			float contact;
			float drop;
			float top;

			float height;
			float endHeight;
			float radius;
			float floor;

			Vector pos;
			Quaternion q;
		};

		struct mstudioanimdesc_t
		{
			float fps;
			int flags;

			int numframes;

			uint16_t sznameindex;
			inline const char* pszName() const { return reinterpret_cast<const char*>(this) + FIX_OFFSET(sznameindex); }

			uint16_t framemovementindex;

			int animindex;

			uint16_t numikrules;
			uint16_t ikruleindex;

			int64_t sectionDataExternal;
			uint16_t unk1;

			uint16_t sectionindex;
			uint16_t sectionstallframes;
			uint16_t sectionframes;
		};

		struct mstudioanimsections_t
		{
			int animindex;
		};

		//
		// Mesh/Model/Bodypart Structures
		//
		struct mstudiomesh_t
		{
			uint16_t material;
			uint16_t meshid;

			char unk_4[4];

			Vector center;
		};

		struct mstudiomodel_t
		{
			uint16_t unkStringOffset;
			inline char* const pszString() const { return ((char*)this + FIX_OFFSET(unkStringOffset)); }

			uint16_t meshCountTotal;
			uint16_t meshCountBase;
			uint16_t meshCountBlend;
			uint16_t meshOffset;
			inline mstudiomesh_t* const pMesh(const uint16_t meshIdx) const {
				return reinterpret_cast<mstudiomesh_t*>((char*)this + FIX_OFFSET(meshOffset)) + meshIdx;
			}
		};

		struct mstudiobodyparts_t
		{
			uint16_t sznameindex;
			inline char* const pszName() const { return ((char*)this + FIX_OFFSET(sznameindex)); }

			uint16_t modelindex;
			inline mstudiomodel_t* const pModel(const uint16_t modelIdx) const {
				return reinterpret_cast<mstudiomodel_t*>((char*)this + FIX_OFFSET(modelindex)) + modelIdx;
			}

			int base;
			int nummodels;
			int meshOffset;
		};

		//
		// Texture (material reference)
		//
		struct mstudiotexture_t
		{
			uint64_t texture;          // hash of material path
		};

		//
		// Studio Header v16 (2-byte packed, 224 bytes / 0xE0)
		// NOTE: This is 4 bytes SMALLER than v19.1 (no unk_E0 field at end)
		//
		struct studiohdr_t
		{
			int flags;
			int checksum;
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }
			char name[33];             // internal name, null padded

			uint8_t surfacepropLookup;

			float mass;

			int contents;

			uint16_t hitboxsetindex;
			uint8_t numhitboxsets;

			uint8_t illumpositionattachmentindex;

			Vector illumposition;

			Vector hull_min;
			Vector hull_max;

			Vector view_bbmin;
			Vector view_bbmax;

			uint16_t boneCount;
			uint16_t boneHdrOffset;
			uint16_t boneDataOffset;

			uint16_t numlocalseq;
			uint16_t localseqindex;
			inline const mstudioseqdesc_t* pLocalSeq(int i) const {
				return reinterpret_cast<const mstudioseqdesc_t*>((const char*)this + FIX_OFFSET(localseqindex)) + i;
			}

			uint16_t unk_7E[2];        // added in v13 -> v14 (disabled sequence indices in v19.1)

			char activitylistversion;

			uint8_t numlocalattachments;
			uint16_t localattachmentindex;

			uint16_t numlocalnodes;
			uint16_t localnodenameindex;
			uint16_t localNodeDataOffset;

			uint16_t numikchains;
			uint16_t ikchainindex;
			inline const mstudioikchain_t* pIKChain(int i) const {
				return reinterpret_cast<const mstudioikchain_t*>((const char*)this + FIX_OFFSET(ikchainindex)) + i;
			}

			uint16_t numtextures;
			uint16_t textureindex;

			uint16_t numskinref;
			uint16_t numskinfamilies;
			uint16_t skinindex;

			uint16_t numbodyparts;
			uint16_t bodypartindex;
			inline const mstudiobodyparts_t* const pBodypart(const uint16_t i) const {
				assert(i >= 0 && i < numbodyparts);
				return reinterpret_cast<mstudiobodyparts_t*>((char*)this + FIX_OFFSET(bodypartindex)) + i;
			}

			uint16_t uiPanelCount;
			uint16_t uiPanelOffset;

			uint16_t numlocalposeparameters;
			uint16_t localposeparamindex;
			inline const mstudioposeparamdesc_t* pPoseParameter(int i) const {
				return reinterpret_cast<const mstudioposeparamdesc_t*>((const char*)this + FIX_OFFSET(localposeparamindex)) + i;
			}

			uint16_t surfacepropindex;

			uint16_t keyvalueindex;

			uint16_t virtualModel;

			uint16_t meshCount;

			uint16_t bonetablebynameindex;

			uint16_t boneStateOffset;
			uint16_t boneStateCount;
			inline const uint8_t* pBoneStates() const {
				return boneStateCount > 0 ? reinterpret_cast<uint8_t*>((char*)this + offsetof(studiohdr_t, boneStateOffset) + FIX_OFFSET(boneStateOffset)) : nullptr;
			}

			uint16_t groupHeaderOffset;
			uint16_t groupHeaderCount;
			const studio_hw_groupdata_t* const pLODGroup(const uint16_t i) const {
				return reinterpret_cast<const studio_hw_groupdata_t* const>((char*)this + offsetof(studiohdr_t, groupHeaderOffset) + FIX_OFFSET(groupHeaderOffset)) + i;
			}

			uint16_t lodOffset;
			uint16_t lodCount;
			const float* const pLODThreshold(const uint16_t i) const {
				return reinterpret_cast<const float* const>((char*)this + offsetof(studiohdr_t, lodOffset) + FIX_OFFSET(lodOffset)) + i;
			}
			const float LODThreshold(const uint16_t i) const { return *pLODThreshold(i); }

			float fadeDistance;
			float gatherSize;          // repurposed as nearFadeDist in v19.1

			uint16_t numsrcbonetransform;
			uint16_t srcbonetransformindex;

			uint16_t sourceFilenameOffset;

			uint16_t linearboneindex;

			uint16_t procBoneCount;
			uint16_t procBoneOffset;
			uint16_t linearProcBoneOffset;

			uint16_t boneFollowerCount;
			uint16_t boneFollowerOffset;

			uint16_t bvhOffset;

			char bvhUnk[2];            // collision detail for bvh

			uint16_t unkDataCount;     // GPU bones count in v19.1
			uint16_t unkDataOffset;    // GPU bones offset in v19.1
			uint16_t unkStrcOffset;    // Wind params offset in v19.1

			// NOTE: v16 does NOT have unk_E0 field (setDressLevel in v19.1)
			// This makes v16 studiohdr_t 224 bytes (0xE0) instead of v19.1's 228 bytes (0xE4)
		};
		static_assert(sizeof(studiohdr_t) == 224, "studiohdr_t size mismatch - expected 224 bytes (0xE0)");

		//
		// Helper macros for accessing bone data in v16
		//
		inline const mstudiobonehdr_t* GetBoneHdr(const studiohdr_t* hdr, int i) {
			return reinterpret_cast<const mstudiobonehdr_t*>((const char*)hdr + FIX_OFFSET(hdr->boneHdrOffset)) + i;
		}

		inline const mstudiobonedata_t* GetBoneData(const studiohdr_t* hdr, int i) {
			return reinterpret_cast<const mstudiobonedata_t*>((const char*)hdr + FIX_OFFSET(hdr->boneDataOffset)) + i;
		}

		inline const mstudiolinearbone_t* GetLinearBone(const studiohdr_t* hdr) {
			return reinterpret_cast<const mstudiolinearbone_t*>((const char*)hdr + FIX_OFFSET(hdr->linearboneindex));
		}

		//
		// V16 Collision Structures (same as v19.1 - 40-byte headers)
		//

		// Same as V10 - 16 bytes
		struct mstudiocollmodel_t
		{
			int32_t contentMasksIndex;   // Offset to contents mask array
			int32_t surfacePropsIndex;   // Offset to surface properties
			int32_t surfaceNamesIndex;   // Offset to surface prop names
			int32_t headerCount;         // Number of collision parts
		};
		static_assert(sizeof(mstudiocollmodel_t) == 16, "mstudiocollmodel_t size mismatch");

		// V16 collision header - 40 bytes (same as v19.1)
		struct mstudiocollheader_t
		{
			uint32_t bvhFlags;           // 0x00: BVH flags
			uint32_t nodesOfs;           // 0x04: Offset to BVH nodes
			uint32_t vertsOfs;           // 0x08: Offset to vertices
			uint32_t leafDataOfs;        // 0x0C: Offset to leaf data
			uint32_t skinInfosOfs;       // 0x10: Offset to skin info
			uint8_t  skinCount;          // 0x14: Number of skins
			uint8_t  meshGroupCount;     // 0x15: Number of mesh groups
			uint16_t pad;                // 0x16: Padding
			float    origin[3];          // 0x18: Decode origin for int16 vertices
			float    decodeScale;        // 0x24: Decode scale for int16 vertices
		};
		static_assert(sizeof(mstudiocollheader_t) == 40, "mstudiocollheader_t size mismatch");

	} // namespace v160

	//
	// v17 (Season 19) - Identical to v16 except studiohdr_t has an extra 4-byte field
	// Total header size: 228 bytes (0xE4) vs v16's 224 bytes (0xE0)
	//
	namespace v170
	{
		// Inherit all types from v160 except studiohdr_t
		using namespace v160;

		// Undef the static_assert that would conflict
		// We redefine studiohdr_t with the extra field

		//
		// Studio Header v17 (2-byte packed, 228 bytes / 0xE4)
		// Same as v16 but with additional unk_E0 field at end
		//
		struct studiohdr_t
		{
			int flags;
			int checksum;
			uint16_t sznameindex;
			inline const char* const pszName() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(sznameindex); }
			char name[33];

			uint8_t surfacepropLookup;

			float mass;

			int contents;

			uint16_t hitboxsetindex;
			uint8_t numhitboxsets;

			uint8_t illumpositionattachmentindex;

			Vector illumposition;

			Vector hull_min;
			Vector hull_max;

			Vector view_bbmin;
			Vector view_bbmax;

			uint16_t boneCount;
			uint16_t boneHdrOffset;
			uint16_t boneDataOffset;

			uint16_t numlocalseq;
			uint16_t localseqindex;
			inline const v160::mstudioseqdesc_t* pLocalSeq(int i) const {
				return reinterpret_cast<const v160::mstudioseqdesc_t*>((const char*)this + FIX_OFFSET(localseqindex)) + i;
			}

			uint16_t unk_7E[2];

			char activitylistversion;

			uint8_t numlocalattachments;
			uint16_t localattachmentindex;

			uint16_t numlocalnodes;
			uint16_t localnodenameindex;
			uint16_t localNodeDataOffset;

			uint16_t numikchains;
			uint16_t ikchainindex;
			inline const v160::mstudioikchain_t* pIKChain(int i) const {
				return reinterpret_cast<const v160::mstudioikchain_t*>((const char*)this + FIX_OFFSET(ikchainindex)) + i;
			}

			uint16_t numtextures;
			uint16_t textureindex;

			uint16_t numskinref;
			uint16_t numskinfamilies;
			uint16_t skinindex;

			uint16_t numbodyparts;
			uint16_t bodypartindex;
			inline const v160::mstudiobodyparts_t* const pBodypart(const uint16_t i) const {
				assert(i >= 0 && i < numbodyparts);
				return reinterpret_cast<v160::mstudiobodyparts_t*>((char*)this + FIX_OFFSET(bodypartindex)) + i;
			}

			uint16_t uiPanelCount;
			uint16_t uiPanelOffset;

			uint16_t numlocalposeparameters;
			uint16_t localposeparamindex;
			inline const v160::mstudioposeparamdesc_t* pPoseParameter(int i) const {
				return reinterpret_cast<const v160::mstudioposeparamdesc_t*>((const char*)this + FIX_OFFSET(localposeparamindex)) + i;
			}

			uint16_t surfacepropindex;

			uint16_t keyvalueindex;

			uint16_t virtualModel;

			uint16_t meshCount;

			uint16_t bonetablebynameindex;

			uint16_t boneStateOffset;
			uint16_t boneStateCount;
			inline const uint8_t* pBoneStates() const {
				return boneStateCount > 0 ? reinterpret_cast<uint8_t*>((char*)this + offsetof(studiohdr_t, boneStateOffset) + FIX_OFFSET(boneStateOffset)) : nullptr;
			}

			uint16_t groupHeaderOffset;
			uint16_t groupHeaderCount;
			const v160::studio_hw_groupdata_t* const pLODGroup(const uint16_t i) const {
				return reinterpret_cast<const v160::studio_hw_groupdata_t* const>((char*)this + offsetof(studiohdr_t, groupHeaderOffset) + FIX_OFFSET(groupHeaderOffset)) + i;
			}

			uint16_t lodOffset;
			uint16_t lodCount;
			const float* const pLODThreshold(const uint16_t i) const {
				return reinterpret_cast<const float* const>((char*)this + offsetof(studiohdr_t, lodOffset) + FIX_OFFSET(lodOffset)) + i;
			}
			const float LODThreshold(const uint16_t i) const { return *pLODThreshold(i); }

			float fadeDistance;
			float gatherSize;

			uint16_t numsrcbonetransform;
			uint16_t srcbonetransformindex;

			uint16_t sourceFilenameOffset;

			uint16_t linearboneindex;

			uint16_t procBoneCount;
			uint16_t procBoneOffset;
			uint16_t linearProcBoneOffset;

			uint16_t boneFollowerCount;
			uint16_t boneFollowerOffset;

			uint16_t bvhOffset;

			char bvhUnk[2];

			uint16_t unkDataCount;
			uint16_t unkDataOffset;
			uint16_t unkStrcOffset;

			// NEW in v17: Additional field at end (called setDressLevel in v19.1)
			int unk_E0;
		};
		static_assert(sizeof(studiohdr_t) == 228, "v170 studiohdr_t size mismatch - expected 228 bytes (0xE4)");

	} // namespace v170

	//
	// v18 (Season 18) - Same header as v17, but uses rseq v12 for sequences
	// mstudioseqdesc_t gains noInterpFrameOffset/Count fields (86 bytes vs 82 bytes)
	//
	namespace v180
	{
		// Use v170 studiohdr_t (same 228-byte header with unk_E0)
		using v170::studiohdr_t;

		// All other types from v160
		using v160::mstudiobonehdr_t;
		using v160::mstudiobonedata_t;
		using v160::mstudiolinearbone_t;
		using v160::mstudioposeparamdesc_t;
		using v160::mstudiohitboxset_t;
		using v160::mstudiobbox_t;
		using v160::mstudioattachment_t;
		using v160::mstudioikchain_t;
		using v160::mstudioiklink_t;
		using v160::mstudioikrule_t;
		using v160::mstudiocompressedikerror_t;
		using v160::mstudioevent_t;
		using v160::mstudioautolayer_t;
		using v160::mstudioactivitymodifier_t;
		using v160::mstudioanimdesc_t;
		using v160::mstudioanimsections_t;
		using v160::mstudiomesh_t;
		using v160::mstudiomodel_t;
		using v160::mstudiobodyparts_t;
		using v160::mstudiotexture_t;
		using v160::mstudiocollmodel_t;
		using v160::mstudiocollheader_t;
		using v160::studio_hw_groupdata_t;
		using v160::GetBoneHdr;
		using v160::GetBoneData;
		using v160::GetLinearBone;

		//
		// mstudioseqdesc_t for rseq v12 (86 bytes)
		// Same as v11 but with noInterpFrame fields at end
		//
		struct mstudioseqdesc_t
		{
			uint16_t szlabelindex;
			inline const char* pszLabel() const { return reinterpret_cast<const char*>(this) + FIX_OFFSET(szlabelindex); }

			uint16_t szactivitynameindex;
			inline const char* pszActivity() const { return reinterpret_cast<const char*>(this) + FIX_OFFSET(szactivitynameindex); }

			int flags;

			uint16_t activity;
			uint16_t actweight;

			uint16_t numevents;
			uint16_t eventindex;

			Vector bbmin;
			Vector bbmax;

			uint16_t numblends;

			uint16_t animindexindex;

			short paramindex[2];
			float paramstart[2];
			float paramend[2];

			float fadeintime;
			float fadeouttime;

			uint16_t localentrynode;
			uint16_t localexitnode;

			uint16_t numikrules;

			uint16_t numautolayers;
			uint16_t autolayerindex;

			uint16_t weightlistindex;

			uint8_t groupsize[2];

			uint16_t posekeyindex;

			uint16_t numiklocks;
			uint16_t iklockindex;

			uint16_t unk_5C;

			uint16_t cycleposeindex;

			uint16_t activitymodifierindex;
			uint16_t numactivitymodifiers;

			int ikResetMask;
			int unk_68;

			uint16_t weightFixupOffset;
			uint16_t weightFixupCount;

			// NEW in v18 (rseq v12): noInterpFrame fields
			uint16_t noInterpFrameOffset;
			uint16_t noInterpFrameCount;
		};
		static_assert(sizeof(mstudioseqdesc_t) == 116, "v180 mstudioseqdesc_t size mismatch - expected 116 bytes (rseq v12)");

		// New struct for no-interp frame ranges
		struct mstudio_nointerpframes_t
		{
			int firstFrame;
			int lastFrame;
		};

	} // namespace v180
} // namespace r5
#pragma pack(pop)
