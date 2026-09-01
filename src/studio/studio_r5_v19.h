// Copyright (c) 2024, rexx
// Copyright (c) 2026, CafeFPS
// See LICENSE.txt for licensing information (GPL v3)
//
// v19.1 (Season 19+) model structures for Apex Legends
// Based on structures from RSX
//
// Key changes from v12.1:
// - 2-byte packing (#pragma pack(push, 2))
// - 16-bit offsets are DIRECT byte offsets (no multiplication needed)
// - Bone structure split into header + data + linear arrays
// - Animation descriptors have animDataAsset GUID for external data
//
// NOTE: Binary analysis of actual v19.1 files shows that offsets are stored
// as actual byte values, NOT halved. FIX_OFFSET is kept as identity for
// compatibility but does NOT multiply by 2.
//

#pragma once
#include <studio/studio.h>

// Macro for offset conversion - v19.1 uses direct byte offsets
// (Previously multiplied by 2, but binary analysis proved this incorrect)
#ifndef FIX_OFFSET
#define FIX_OFFSET(x) (x)
#endif

#pragma pack(push, 2)
namespace r5
{
	namespace v191
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
		// Bone Data (16 bytes) - v19 format
		// Contains parent, flags, proc info
		// Pose data is in linear bone arrays
		//
		struct mstudiobonedata_t
		{
			short parent;              // parent bone

			uint16_t unk_76;           // unknown, possibly padding

			int flags;

			uint8_t collisionIndex;    // index into collision sections, 0xFF if none

			uint8_t proctype;
			uint16_t procindex;        // procedural rule offset

			int unk_C;                 // possibly alignment for 16 bytes
		};
		static_assert(sizeof(mstudiobonedata_t) == 16, "mstudiobonedata_t size mismatch");

		//
		// Linear Bone Data (18 bytes header + arrays)
		// Contains arrays of bone transform data indexed by bone
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
			inline int* pParent(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<int*>((char*)this + FIX_OFFSET(parentindex)) + i;
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

			uint16_t qalignmentindex;
			inline const Quaternion* pQAlignment(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<Quaternion*>((char*)this + FIX_OFFSET(qalignmentindex)) + i;
			}

			uint16_t scaleindex;
			inline const Vector* pScale(int i) const {
				assert(i >= 0 && i < numbones);
				return reinterpret_cast<Vector*>((char*)this + FIX_OFFSET(scaleindex)) + i;
			}
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
			inline const mstudioiklink_t* const pLink(const int i) const {
				return reinterpret_cast<const mstudioiklink_t* const>((const char*)this + FIX_OFFSET(linkindex)) + i;
			}

			float unk_10;              // likely cos value for IK constraints
		};

		//
		// Animation Structures
		//
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

			uint16_t szattachmentindex; // name of world attachment

			float start;               // beginning of influence
			float peak;                // start of full influence
			float tail;                // end of full influence
			float end;                 // end of all influence

			float contact;             // frame footstep makes ground contact
			float drop;                // how far down the foot should drop
			float top;                 // top of the foot box

			float height;
			float endHeight;
			float radius;
			float floor;

			Vector pos;
			Quaternion q;
		};

		struct mstudioanimsections_t
		{
			int animindex;             // negative bit set if external
			inline const bool isExternal() const { return animindex < 0; }
			inline const int Index() const { return isExternal() ? ~animindex : animindex; }
		};

		//
		// Animation Descriptor v19.1 (with animDataAsset GUID)
		//
		struct mstudioanimdesc_t
		{
			float fps;                 // frames per second
			int flags;                 // looping/non-looping flags

			int numframes;

			uint16_t sznameindex;
			inline const char* pszName() const { return ((char*)this + FIX_OFFSET(sznameindex)); }

			uint16_t framemovementindex;

			uint16_t numikrules;

			uint8_t unused_12[4];      // padding/unused

			uint16_t ikruleindex;
			inline const mstudioikrule_t* const pIKRule(const int i) const {
				return reinterpret_cast<mstudioikrule_t*>((char*)this + FIX_OFFSET(ikruleindex)) + i;
			}

			uint64_t animDataAsset;    // GUID in pak, pointer to asset on load

			char* sectionDataExternal; // set on pak asset load
			uint16_t unk1;             // thread/mutex for external data

			uint16_t sectionindex;
			uint16_t sectionstallframes;
			uint16_t sectionframes;
			inline const mstudioanimsections_t* pSection(int i) const {
				return reinterpret_cast<mstudioanimsections_t*>((char*)this + FIX_OFFSET(sectionindex)) + i;
			}
		};

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

		//
		// No-interp frames (v18+)
		//
		struct mstudio_nointerpframes_t
		{
			int firstFrame;
			int lastFrame;
		};

		//
		// Sequence Descriptor v18/v19
		//
		struct mstudioseqdesc_t
		{
			uint16_t szlabelindex;
			inline const char* pszLabel() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(szlabelindex); }

			uint16_t szactivitynameindex;
			inline const char* pszActivity() const { return reinterpret_cast<const char* const>(this) + FIX_OFFSET(szactivitynameindex); }

			int flags;

			uint16_t activity;
			uint16_t actweight;

			uint16_t numevents;
			uint16_t eventindex;

			Vector bbmin;
			Vector bbmax;

			uint16_t numblends;

			uint16_t animindexindex;
			const int AnimIndex(const uint16_t i) const {
				return FIX_OFFSET(reinterpret_cast<const uint16_t* const>((const char* const)this + FIX_OFFSET(animindexindex))[i]);
			}
			const int AnimCount() const { return groupsize[0] * groupsize[1]; }
			mstudioanimdesc_t* pAnimDesc(const uint16_t i) const {
				return reinterpret_cast<mstudioanimdesc_t*>((char*)this + AnimIndex(i));
			}

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

			// v18+ additions
			uint16_t noInterpFrameOffset;
			uint16_t noInterpFrameCount;
		};

		//
		// Pose Parameter
		//
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
		// Studio Header v19.1 (2-byte packed)
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

			uint16_t unk_7E[2];        // added in v13 -> v14

			char activitylistversion;

			uint8_t numlocalattachments;
			uint16_t localattachmentindex;

			uint16_t numlocalnodes;
			uint16_t localnodenameindex;
			uint16_t localNodeDataOffset;

			uint16_t numikchains;
			uint16_t ikchainindex;

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

			// v17 addition
			int unk_E0;
		};

		//
		// Helper macros for accessing bone data in v19.1
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
		// V19.1 Collision Structures (40-byte headers vs V10's 32-byte)
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

		// V19.1 collision header - 40 bytes (V10 is 32 bytes)
		// Extra 8 bytes: skinInfosOfs(4) + skinCount(1) + meshGroupCount(1) + pad(2)
		struct mstudiocollheader_t
		{
			uint32_t bvhFlags;           // 0x00: BVH flags
			uint32_t nodesOfs;           // 0x04: Offset to BVH nodes
			uint32_t vertsOfs;           // 0x08: Offset to vertices
			uint32_t leafDataOfs;        // 0x0C: Offset to leaf data
			uint32_t skinInfosOfs;       // 0x10: Offset to skin info (V19.1 ONLY)
			uint8_t  skinCount;          // 0x14: Number of skins (V19.1 ONLY)
			uint8_t  meshGroupCount;     // 0x15: Number of mesh groups (V19.1 ONLY)
			uint16_t pad;                // 0x16: Padding (V19.1 ONLY)
			float    origin[3];          // 0x18: Decode origin for int16 vertices
			float    decodeScale;        // 0x24: Decode scale for int16 vertices
		};
		static_assert(sizeof(mstudiocollheader_t) == 40, "mstudiocollheader_t size mismatch");

	} // namespace v191
} // namespace r5
#pragma pack(pop)
