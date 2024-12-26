// Strike
// Bad Ass Hackers 2021-2024

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <assert.h>
#include "CMP.h"

using namespace std;

#define DUMPALL

struct tMAT_Header
{
	char verification[4];		// 'MAT '
	long version;				// version of material.   should be 0x32
	long type;					// type of material.  0 = color       1 = ?         2 = texture
	long numMaterials;			// number of materials contained in material file
	long numTextures;			// number of textures. in color materials, this is 0. in texture materials, this is equal to numMaterials
	long unknown0;				// JKSpecs says this is 0.  BoBo Fett's mat-16 spec says this is 1
	long colorBits;				// number of bits per pixel
	long blueBits;				// number of blue bits per pixel (could this be red?)
	long greenBits;				// number of green bits per pixel
	long redBits;				// number of red bits per pixel (could this be blue?)
	long blueShiftL;			// blue left shift
	long greenShiftL;			// green left shift
	long redShiftL;				// red left shift
	long blueShiftR;			// blue right shift
	long greenShiftR;			// green right shift
	long redShiftR;				// red right shift
	//char unknown1[12];			// unknown/unused bytes
	long alphaBits;
	long alphaShiftL;
	long alphaShiftR;
};

// Texture material header
struct tMAT_TextureHeader
{
	long unknown0;				// unknown	( BoBo Fett reports this as being 0 )
	long unknown1;				// unknown  ( BoBo Fett reports this as being 4 )
	long unknown2;				// unknown  ( JKSpecs reports this as being 0xBFF78482, BoBo Fett reports this as being 4 )
	long textureId;				// current texture ID, starting at 0 up to numTextures - 1
};

// Material entry header
struct tMAT_MaterialHeader
{
	long type;					// type of material entry (0 = color    8 = texture)
	long colorIndex;			// for color materials, this is the palette entry in CMP for the material color. for textures, this is RGB of transparency value
	long unknown[4];			// unknown/unused bytes   ( JKSpecs reports these are all 0x3F800000 )
	//struct tMAT_TextureHeader *textureHeader;		// pointer to texture header (NULL for color materials)
};

// Texture material data
struct tMAT_TextureData
{
	long width;					// width of texture
	long height;				// height of texture
	long transparent;			// bool to indictate using transparency
	long unknown[2];			// unknown/unused bytes
	long numMipmaps;			// number of mipmaps for texture
	//void **pData;				// actual bitmap data (array of pointers to bytes, numMipmaps large)
};

// MAT data format
struct tMAT_Data
{
	struct tMAT_Header header;							// MAT file header
	struct tMAT_MaterialHeader* materialHeaders;		// material headers			(array of size numMaterials)
	struct tMAT_TextureData* textureData;				// texture datas			(array of size numTextures)
};

struct mipimage
{
	int width;
	int height;
	bool trans;
	unsigned char* buf;
};

char* subfile(const char* szroot, const char* szfile)
{
	static char blah[MAX_PATH];

	sprintf(blah, "%s\\%s", szroot, szfile);

	return blah;
}


struct MODELNODEHEADER
{
	char name[32];
	int someIndex1;
	int geoMode;
	int lightMode;
	int texMode;
	int ndunno[6];
	int numVerts;
	int numTexVerts;
	int numFaces;
	float fdunno[7];
};

struct MODELFACEHEADER
{
	int matIndex;
	int flags;
	int geoMode;
	int lightMode;
	int texMode;
	int numVerts;

	int dunno5; // pointer placeholder
	int dunno6; // pointer placeholder
	int dunno7; // pointer placeholder

	int dunno8; // usually -1
	int dunno9; // usually 0
	int dunno10; // usually 0
	int dunno11; // usually 0
	int dunno12; // usually 0

	float nx, ny, nz;

	int dunno13;
	int dunno14;
};

struct MODELHIERARCHYNODEHEADER
{
	char name[8];
	int hdunno[17];
	int meshIndex;
	int parentIndex;
	int numChildren;
	int firstChildIndex;
	int siblingIndex;
	float px, py, pz;
	float x, y, z;
	float pitch, yaw, roll;
	int hdunno2[12];
};

struct MODELFACE
{
	MODELFACEHEADER hdr;

	int* pnFaceIndices; // all vert indices, then all texvert indices
};

struct MODELNODE
{
	MODELNODEHEADER hdr;

	float* pVerts; // 3d
	float* pVertNorms; // 3d
	float* pVertUnknowns;  // 2d
	float* pTexVerts; // 2d

	MODELFACE* pFaces;
};

void loadmodel(const char* szdump, int nNumMats, const char* const* pszMatNames, float worldScale, FILE* f)
{
	char modelName[32];
	fread(modelName, 1, 32, f);

	char* modelfile = subfile(szdump, modelName);
	char modelfile2[256];
	sprintf(modelfile2, "%s.txt", modelfile);
	FILE* mf = fopen(modelfile2, "w");


	fprintf(mf, "\n---------------------------------------------------------\n");
	fprintf(mf, "-------------------- HEADER ------------------------\n");
	fprintf(mf, "---------------------------------------------------------\n");



	int mdunno1;
	fread(&mdunno1, 4, 1, f);
	fprintf(mf, "mdunno1=%d\n", mdunno1);

	int numNodes;
	fread(&numNodes, 4, 1, f);
	fprintf(mf, "numNodes=%d\n", numNodes);

	int mdunno[23];
	fread(mdunno, 4, 23, f);
	for (int ii = 0; ii < 23; ii++)
	{
		fprintf(mf, "mdunno[%d]=%d\n", ii, mdunno[ii]);
	}

	int numHierarchyNodes = mdunno[11];




	MODELNODE* pNodes = nullptr;
	if (numNodes > 0)
	{
		fprintf(mf, "\n---------------------------------------------------------\n");
		fprintf(mf, "-------------------- NODE DEFINITIONS ------------------------\n");
		fprintf(mf, "---------------------------------------------------------\n");


		pNodes = new MODELNODE[numNodes];

		for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
		{
			fprintf(mf, "\n----------------\n");

			MODELNODE* node = &pNodes[nodeIndex];

			fread(&node->hdr, sizeof(MODELNODEHEADER), 1, f);

			fprintf(mf, "nodeName=%s\n", node->hdr.name);
			fprintf(mf, "someIndex1=%d\n", node->hdr.someIndex1);
			fprintf(mf, "geoMode=%d\n", node->hdr.geoMode);
			fprintf(mf, "lightMode=%d\n", node->hdr.lightMode);
			fprintf(mf, "texMode=%d\n", node->hdr.texMode);

			for (int ii = 0; ii < 6; ii++)
			{
				fprintf(mf, "ndunno[%d]=%d\n", ii, node->hdr.ndunno[ii]);
			}

			fprintf(mf, "numVerts=%d\n", node->hdr.numVerts);
			fprintf(mf, "numTexVerts=%d\n", node->hdr.numTexVerts);
			fprintf(mf, "numFaces=%d\n", node->hdr.numFaces);

			for (int ii = 0; ii < 7; ii++)
			{
				fprintf(mf, "fdunno[%d]=%f\n", ii, node->hdr.fdunno[ii]);
			}

		}
	}


	fprintf(mf, "\n\n\n---------------------------------------------------------\n");
	fprintf(mf, "-------------------- NODES!!!!!!! ------------------------\n");
	fprintf(mf, "---------------------------------------------------------\n");

#if 1
	for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
	{
		MODELNODE* node = &pNodes[nodeIndex];

		fprintf(mf, "\nnode: %s\n---------------------------\n", node->hdr.name);

		if (node->hdr.numVerts == 0)
		{
			node->pVerts = nullptr;
			node->pVertNorms = nullptr;
			node->pVertUnknowns = nullptr;
		}
		else
		{
			node->pVerts = new float[node->hdr.numVerts * 3];
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx, fy, fz;
				fread(&fx, 4, 1, f);
				fread(&fy, 4, 1, f);
				fread(&fz, 4, 1, f);

				node->pVerts[ii * 3 + 0] = fx;
				node->pVerts[ii * 3 + 1] = fy;
				node->pVerts[ii * 3 + 2] = fz;

				fprintf(mf, "vert[%d]: %f/%f/%f\n", ii, fx, fy, fz);
			}

			node->pVertNorms = new float[node->hdr.numVerts * 3];
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx, fy, fz;
				fread(&fx, 4, 1, f);
				fread(&fy, 4, 1, f);
				fread(&fz, 4, 1, f);

				node->pVertNorms[ii * 3 + 0] = fx;
				node->pVertNorms[ii * 3 + 1] = fy;
				node->pVertNorms[ii * 3 + 2] = fz;

				float mag = sqrtf(fx * fx + fy * fy + fz * fz);

				fprintf(mf, "norm[%d]: mag=%f\t%f/%f/%f\n", ii, mag, fx, fy, fz);
			}

			node->pVertUnknowns = new float[node->hdr.numVerts * 2];
			bool hadNonZero = false;
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx, fy;
				fread(&fx, 4, 1, f);
				fread(&fy, 4, 1, f);

				node->pVertUnknowns[ii * 2 + 0] = fx;
				node->pVertUnknowns[ii * 2 + 1] = fy;

				hadNonZero |= (fx != 0) || (fy != 0);

				fprintf(mf, "uvscroll?[%d]: %f/%f\n", ii, fx, fy);
			}
		}

		if (node->hdr.numTexVerts == 0)
			node->pTexVerts = nullptr;
		else
		{
			node->pTexVerts = new float[node->hdr.numTexVerts * 2];
			for (int ii = 0; ii < node->hdr.numTexVerts; ii++)
			{
				float fx, fy;
				fread(&fx, 4, 1, f);
				fread(&fy, 4, 1, f);

				node->pTexVerts[ii * 2 + 0] = fx;
				node->pTexVerts[ii * 2 + 1] = fy;

				fprintf(mf, "texvert[%d]: %f/%f\n", ii, fx, fy);
			}
		}

		if (node->hdr.numFaces == 0)
			node->pFaces = nullptr;
		else
		{
			node->pFaces = new MODELFACE[node->hdr.numFaces];


			for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
			{
				MODELFACE* face = &node->pFaces[faceIndex];

				fread(&face->hdr, sizeof(MODELFACEHEADER), 1, f);


				fprintf(mf, "face[%d]: matIndex=%d (%s), flags=%d, geo=%d, light=%d, tex=%d, numVerts=%d, d5=0x%08X, d6=0x%08X, d7=0x%08X, d8=%d, d9=%d, d10=%d, d11=%d, d12=%d, norm=(%f/%f/%f), d13=%d, d14=%d\n", faceIndex,
					face->hdr.matIndex, pszMatNames[face->hdr.matIndex], face->hdr.flags, face->hdr.geoMode, face->hdr.lightMode, face->hdr.texMode, face->hdr.numVerts,
					face->hdr.dunno5, face->hdr.dunno6, face->hdr.dunno7,
					face->hdr.dunno8, face->hdr.dunno9, face->hdr.dunno10, face->hdr.dunno11, face->hdr.dunno12,
					face->hdr.nx, face->hdr.ny, face->hdr.nz,
					face->hdr.dunno13, face->hdr.dunno14
				);
			}



			for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
			{
				MODELFACE* face = &node->pFaces[faceIndex];

				face->pnFaceIndices = new int[face->hdr.numVerts * 2];

				fprintf(mf, "faceindices[%d]=", faceIndex);

				for (int ii = 0; ii < face->hdr.numVerts; ii++)
				{
					int vi;
					fread(&vi, 4, 1, f);

					face->pnFaceIndices[ii] = vi;

					fprintf(mf, "%d,", vi);
				}
				fprintf(mf, " | ");
				for (int ii = 0; ii < face->hdr.numVerts; ii++)
				{
					int vi;
					fread(&vi, 4, 1, f);

					face->pnFaceIndices[face->hdr.numVerts + ii] = vi;

					fprintf(mf, "%d,", vi);
				}

				fprintf(mf, "\n");
			}
		}



		/*for (int ii = 0; ii < 300; ii++)
		{
			int fx;
			fread(&fx, 4, 1, f);

			fprintf(mf, "dunno[%d]: %d\n", ii, fx);
		}*/

	}
#else

	// verts?
	for (int vertIndex = 0; vertIndex < 100; vertIndex++)
	{
		float fx, fy, fz;
		fread(&fx, 4, 1, f);
		fread(&fy, 4, 1, f);
		fread(&fz, 4, 1, f);

		fprintf(mf, "vertIndex=%d: %f/%f/%f\n", vertIndex, fx, fy, fz);
	}
#endif

	fprintf(mf, "\n\n\n---------------------------------------------------------\n");
	fprintf(mf, "-------------------- NODE HIERARCHY ------------------------\n");
	fprintf(mf, "---------------------------------------------------------\n");

	MODELHIERARCHYNODEHEADER* pHNodes = nullptr;
	if (numHierarchyNodes > 0)
	{
		pHNodes = new MODELHIERARCHYNODEHEADER[numHierarchyNodes];

		for (int hierNodeIndex = 0; hierNodeIndex < numHierarchyNodes; hierNodeIndex++)
		{
			MODELHIERARCHYNODEHEADER* hnode = &pHNodes[hierNodeIndex];

			fread(hnode, sizeof(MODELHIERARCHYNODEHEADER), 1, f);

			fprintf(mf, "%d: hierarchy node=%s\n", hierNodeIndex, hnode->name);

			for (int ii = 0; ii < 17; ii++)
			{
				fprintf(mf, "hdunno[%d]=%d\t%f\n", ii, hnode->hdunno[ii], *(float*)&hnode->hdunno[ii]);
			}

			fprintf(mf, "meshIndex=%d\n", hnode->meshIndex);
			fprintf(mf, "parentIndex=%d\n", hnode->parentIndex);
			fprintf(mf, "numChildren=%d\n", hnode->numChildren);
			fprintf(mf, "firstChildIndex=%d\n", hnode->firstChildIndex);
			fprintf(mf, "siblingIndex=%d\n", hnode->siblingIndex);
			fprintf(mf, "pivot=%f/%f/%f\n", hnode->px, hnode->py, hnode->pz);
			fprintf(mf, "pos=%f/%f/%f\n", hnode->x, hnode->y, hnode->z);
			fprintf(mf, "pyr=%f/%f/%f\n", hnode->pitch, hnode->yaw, hnode->roll);

			for (int ii = 0; ii < 12; ii++)
			{
				fprintf(mf, "hdunno2[%d]=%d\t%f\n", ii, hnode->hdunno2[ii], *(float*)&hnode->hdunno2[ii]);
			}

			fprintf(mf, "\n");
		}
	}


	fprintf(mf, "\n\n\n---------------------------------------------------------\n");
	fprintf(mf, "-------------------- TAIL 64bytes ------------------------\n");
	fprintf(mf, "---------------------------------------------------------\n");
	{
		int tail[16];
		fread(tail, 4, 16, f);

		for (int ii = 0; ii < 16; ii++)
		{
			fprintf(mf, "tail[%d]=%d\t%f\n", ii, tail[ii], *(float*)&tail[ii]);
		}
	}


	fclose(mf);



	mf = fopen(modelfile, "w");
	fprintf(mf, "# MODEL '%s' created with TPMExtractor\n", modelName);
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: HEADER\n");
	fprintf(mf, "\n");
	fprintf(mf, "3DO 2.1\n");
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: MODELRESOURCE\n");
	fprintf(mf, "\n");
	fprintf(mf, "# Materials list\n");
	fprintf(mf, "MATERIALS %d\n", nNumMats);
	fprintf(mf, "\n");
	for (int x = 0; x < nNumMats; x++)
	{
		char matname[32];
		strcpy(matname, pszMatNames[x]);
		int sl = strlen(matname);
		matname[sl - 4] = 0;
		strcat(matname, "16.mat");

		fprintf(mf, "%d: %s\n", x, matname);//pszMatNames[x]);
	}
	fprintf(mf, "\n");
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: GEOMETRYDEF\n");
	fprintf(mf, "\n");
	fprintf(mf, "# Object radius\n");
	fprintf(mf, "RADIUS %f # dunno\n", 0.1f);
	fprintf(mf, "\n");
	fprintf(mf, "# Insertion offset\n");
	fprintf(mf, "INSERT OFFSET 0 0 %f #dunno\n", 0.1f);
	fprintf(mf, "\n");
	fprintf(mf, "# Number of Geometry Sets\n");
	fprintf(mf, "GEOSETS 1\n");
	fprintf(mf, "\n");
	fprintf(mf, "# Geometry Set definition\n");
	fprintf(mf, "GEOSET 0\n");
	fprintf(mf, "\n");
	fprintf(mf, "# Number of Meshes\n");
	fprintf(mf, "MESHES %d\n", numNodes);
	fprintf(mf, "\n");
	for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
	{
		MODELNODE* node = &pNodes[nodeIndex];

		fprintf(mf, "\n");
		fprintf(mf, "# Mesh definition\n");
		fprintf(mf, "MESH %d\n", nodeIndex);
		fprintf(mf, "\n");
		fprintf(mf, "NAME %s\n", node->hdr.name);
		fprintf(mf, "\n");
		fprintf(mf, "RADIUS %f # dunno\n", 0.1f);
		fprintf(mf, "\n");
		fprintf(mf, "GEOMETRYMODE %d\n", node->hdr.geoMode);
		fprintf(mf, "LIGHTINGMODE %d\n", node->hdr.lightMode);
		fprintf(mf, "TEXTUREMODE %d\n", node->hdr.texMode);
		fprintf(mf, "\n");
		fprintf(mf, "\n");
		fprintf(mf, "VERTICES %d\n", node->hdr.numVerts);
		fprintf(mf, "\n");
		fprintf(mf, "# num:     x:         y:         z:         i: \n");
		for (int vi = 0; vi < node->hdr.numVerts; vi++)
		{
			fprintf(mf, "%d: %f %f %f %f\n", vi, node->pVerts[vi * 3 + 0] * worldScale, node->pVerts[vi * 3 + 1] * worldScale, node->pVerts[vi * 3 + 2] * worldScale, 0.0f);
		}
		fprintf(mf, "\n");
		fprintf(mf, "\n");
		fprintf(mf, "TEXTURE VERTICES %d\n", node->hdr.numTexVerts);
		fprintf(mf, "\n");
		for (int vi = 0; vi < node->hdr.numTexVerts; vi++)
		{
			fprintf(mf, "%d: %f %f\n", vi, node->pTexVerts[vi * 2 + 0], node->pTexVerts[vi * 2 + 1]);
		}
		fprintf(mf, "\n");
		fprintf(mf, "\n");
		fprintf(mf, "VERTEX NORMALS\n");
		fprintf(mf, "\n");
		fprintf(mf, "# num:     x:         y:         z:\n");
		for (int vi = 0; vi < node->hdr.numVerts; vi++)
		{
			fprintf(mf, "%d: %f %f %f\n", vi, node->pVertNorms[vi * 3 + 0], node->pVertNorms[vi * 3 + 1], node->pVertNorms[vi * 3 + 2]);
		}
		fprintf(mf, "\n");
		fprintf(mf, "\n");
		fprintf(mf, "FACES %d\n", node->hdr.numFaces);
		fprintf(mf, "\n");
		fprintf(mf, "#  num:  material:   type:  geo:  light:   tex:  extralight:  verts:\n");
		for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
		{
			MODELFACE* face = &node->pFaces[faceIndex];

			fprintf(mf, "%d: %d 0x%X %d %d %d %f %d", faceIndex, face->hdr.matIndex, face->hdr.flags, face->hdr.geoMode, face->hdr.lightMode, face->hdr.texMode, 0.0f, face->hdr.numVerts);
			for (int vi = 0; vi < face->hdr.numVerts; vi++)
			{
				fprintf(mf, " %d, %d", face->pnFaceIndices[vi], face->pnFaceIndices[face->hdr.numVerts + vi]);
			}
			fprintf(mf, "\n");
		}
		fprintf(mf, "\n");
		fprintf(mf, "\n");
		fprintf(mf, "FACE NORMALS\n");
		fprintf(mf, "\n");
		fprintf(mf, "# num:     x:         y:         z:\n");
		for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
		{
			MODELFACE* face = &node->pFaces[faceIndex];

			fprintf(mf, "%d: %f %f %f\n", faceIndex, face->hdr.nx, face->hdr.ny, face->hdr.nz);
		}
	}
	fprintf(mf, "\n");
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: HIERARCHYDEF\n");
	fprintf(mf, "\n");
	fprintf(mf, "# Hierarchy node list\n");
	fprintf(mf, "HIERARCHY NODES %d\n", numHierarchyNodes);
	fprintf(mf, "\n");
	fprintf(mf, "#  num:   flags:   type:    mesh:  parent:  child:  sibling:  numChildren:        x:         y:         z:     pitch:       yaw:      roll:    pivotx:    pivoty:    pivotz:  hnodename:\n");
	for (int hnodeIndex = 0; hnodeIndex < numHierarchyNodes; hnodeIndex++)
	{
		MODELHIERARCHYNODEHEADER* hnode = &pHNodes[hnodeIndex];

		fprintf(mf, "%d: 0x%X 0x%X %d %d %d %d %d %f %f %f %f %f %f %f %f %f %s\n", hnodeIndex,
			0x0, 0x1, hnode->meshIndex,
			hnode->parentIndex, hnode->firstChildIndex, hnode->siblingIndex, hnode->numChildren,
#if 1
			hnode->x * worldScale, hnode->y * worldScale, hnode->z * worldScale,
			hnode->pitch, hnode->yaw, hnode->roll,
			hnode->px * worldScale, hnode->py * worldScale, hnode->pz * worldScale,
#else
			hnode->x, hnode->y, hnode->z,
			hnode->pitch * 180.0f / M_PI, hnode->yaw * 180.0f / M_PI, hnode->roll * 180.0f / M_PI,
			hnode->px, hnode->py, hnode->pz,
#endif
			hnode->name);
	}
	fprintf(mf, "\n");
	fclose(mf);




	if (pHNodes != nullptr)
		delete[] pHNodes;

	if (pNodes != nullptr)
	{
		for (int ii = 0; ii < numNodes; ii++)
		{
			MODELNODE* node = &pNodes[ii];

			for (int ii2 = 0; ii2 < node->hdr.numFaces; ii2++)
			{
				MODELFACE* face = &node->pFaces[ii2];

				delete[] face->pnFaceIndices;
			}

			if (node->pVerts != nullptr)
				delete[] node->pVerts;

			if (node->pVertNorms != nullptr)
				delete[] node->pVertNorms;

			if (node->pVertUnknowns != nullptr)
				delete[] node->pVertUnknowns;

			if (node->pTexVerts != nullptr)
				delete[] node->pTexVerts;

			if (node->pFaces != nullptr)
				delete[] node->pFaces;
		}
		delete[] pNodes;
	}
}

struct KEYHEADER
{
	char name[32];
	int dunno1;
	int dunno2;
	int dunno3;
	int type;
	float fps;
	int numFrames;
	int numNodes;
	int bleh[18];
};

struct KEYFRAME
{
	float frame;
	int flags;
	float x, y, z;
	float pitch, yaw, roll;
	float dx, dy, dz;
	float dpitch, dyaw, droll;
};

struct KEYNODEHEADER
{
	char name[32];
	int someIndex;
	int numFrames;
	KEYFRAME* pFrames;
};

void loadkey(const char* szdump, float worldScale, FILE* f)
{
	KEYHEADER* key = new KEYHEADER;
	fread(key, sizeof(KEYHEADER), 1, f);

	char* keyfile = subfile(szdump, key->name);
	char keyfile2[256];
	sprintf(keyfile2, "%s.txt", keyfile);
	FILE* mf = fopen(keyfile2, "w");


	fprintf(mf, "dunno1: %d\n", key->dunno1);
	fprintf(mf, "dunno2: %d\n", key->dunno2);
	fprintf(mf, "dunno3: %d\n", key->dunno3);
	fprintf(mf, "type: %d\n", key->type);
	fprintf(mf, "fps: %f\n", key->fps);
	fprintf(mf, "numFrames: %d\n", key->numFrames);
	fprintf(mf, "numNodes: %d\n", key->numNodes);

	for (int x = 0; x < 18; x++)
		fprintf(mf, "bleh[%d]=%d\t%f\n", x, key->bleh[x], *(float*)&key->bleh[x]);


	KEYNODEHEADER* pNodes = nullptr;
	if (key->numNodes > 0)//!! could be dunno3 or dunno6 !!
	{
		pNodes = new KEYNODEHEADER[key->numNodes];

		for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
		{
			KEYNODEHEADER* node = &pNodes[nodeIndex];

			fread(node, sizeof(KEYNODEHEADER), 1, f);

			fprintf(mf, "node=%s\t%d\t%d\n", node->name, node->someIndex, node->numFrames);
			//fprintf(mf, "dunno1=%d\t%f\n", node->dunno1, *(float*)&node->dunno1);
			//fprintf(mf, "dunno2=%d\t%f\n", node->dunno2, *(float*)&node->dunno2);
			//fprintf(mf, "dunno3=%d\t%f\n", node->dunno3, *(float*)&node->dunno3);
		}


		for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
		{
			KEYNODEHEADER* node = &pNodes[nodeIndex];

			node->pFrames = new KEYFRAME[node->numFrames];
			for (int frameIndex = 0; frameIndex < node->numFrames; frameIndex++)
			{
				KEYFRAME* frame = &node->pFrames[frameIndex];

				fread(frame, sizeof(KEYFRAME), 1, f);

				fprintf(mf, "frame[%d]: %f\t%d\tpos=%f/%f/%f\tpyr=%f/%f/%f\tdeltapos=%f/%f/%f\tdeltapyr=%f/%f/%f\n", frameIndex, frame->frame, frame->flags,
					frame->x, frame->y, frame->z,
					frame->pitch, frame->yaw, frame->roll,
					frame->dx, frame->dy, frame->dz,
					frame->dpitch, frame->dyaw, frame->droll
				);

			}
		}
	}


	fprintf(mf, "\n\n\n---------------------------------------------------------\n");
	fprintf(mf, "-------------------- TAIL 64bytes ------------------------\n");
	fprintf(mf, "---------------------------------------------------------\n");
	{
		int tail[16];
		fread(tail, 4, 16, f);

		for (int ii = 0; ii < 16; ii++)
		{
			fprintf(mf, "tail[%d]=%d\t%f\n", ii, tail[ii], *(float*)&tail[ii]);
		}
	}

	/*for (int x = 0; x < 1000; x++)
	{
		int v;
		fread(&v, 4, 1, f);
		fprintf(mf, "[%d]=%d\t%f\n", x, v, *(float*)&v);
	}*/


	fclose(mf);







	mf = fopen(keyfile, "w");

	fprintf(mf, "# KEYFRAME '%s' created with TPMExtractor\n", key->name);
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: HEADER\n");
	fprintf(mf, "\n");
	fprintf(mf, "FLAGS 0x0\n");
	fprintf(mf, "TYPE 0x%04X\n", key->type);
	fprintf(mf, "FRAMES %d\n", key->numFrames);
	fprintf(mf, "FPS %f\n", key->fps);
	fprintf(mf, "JOINTS %d\n", key->numNodes);
	fprintf(mf, "\n");
	fprintf(mf, "\n");
	fprintf(mf, "###############\n");
	fprintf(mf, "SECTION: KEYFRAME NODES\n");
	fprintf(mf, "\n");
	fprintf(mf, "NODES %d\n", key->numNodes);
	fprintf(mf, "\n");
	for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
	{
		KEYNODEHEADER* node = &pNodes[nodeIndex];

		fprintf(mf, "NODE %d\n", nodeIndex);
		fprintf(mf, "MESH NAME %s\n", node->name);
		fprintf(mf, "ENTRIES %d\n", node->numFrames);
		fprintf(mf, "\n");
		fprintf(mf, "# num:   frame:   flags:           x:           y:           z:           p:           y:           r:\n");
		fprintf(mf, "#                                 dx:          dy:          dz:          dp:          dy:          dr:\n");

		for (int x = 0; x < node->numFrames; x++)
		{
			KEYFRAME* frame = &node->pFrames[x];

			fprintf(mf, "%d:\t%d\t0x%04X\t%f\t%f\t%f\t%f\t%f\t%f\n", x, (int)frame->frame, frame->flags, frame->x * worldScale, frame->y * worldScale, frame->z * worldScale, frame->pitch, frame->yaw, frame->roll);
			fprintf(mf, "\t\t\t%f\t%f\t%f\t%f\t%f\t%f\n", frame->dx * worldScale, frame->dy * worldScale, frame->dz * worldScale, frame->dpitch, frame->dyaw, frame->droll);
		}
		fprintf(mf, "\n");
	}


	fclose(mf);







	for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
	{
		KEYNODEHEADER* node = &pNodes[nodeIndex];

		if (node->pFrames != nullptr)
			delete[] node->pFrames;
	}
	if (pNodes != nullptr)
	{

		delete[] pNodes;
	}

	delete key;
}


void dumpbaf(const char* szfile, const char* szdump)
{
	printf("Processing: %s\n", szfile);

	assert(_CrtCheckMemory());

	/*int lastslash = -1;
	int dot = -1;
	for (int i = strlen(szfile) - 1; ; i--)
	{
		if (szfile[i] == '.')
			dot = i;

		if (szfile[i] == '\\' || szfile[i] == '/')
		{
			lastslash = i;
			break;
		}
	}

	char bafname[32]; char* pbafname = bafname;
	for (int i = lastslash + 1; i < dot; i++)
		*(pbafname++) = szfile[i];
	*pbafname = 0;


	CreateDirectory(bafname, NULL);*/



	FILE* f = fopen(szfile, "rb");

	int header1, header2;
	fread(&header1, 4, 1, f);
	fread(&header2, 4, 1, f);

	char name[32];
	fread(name, 1, 32, f);

	char sourcename[128];
	fread(sourcename, 1, 128, f);

	char dunno[28];
	fread(dunno, 1, 28, f);

	int numMats;
	fread(&numMats, 4, 1, f);

	int numKeys;
	fread(&numKeys, 4, 1, f);

	int numModels;
	fread(&numModels, 4, 1, f);

	bool weirdModelFlag = (numModels & 0x1000000) != 0;
	numModels &= (0x1000000 - 1);


	//char dunno2[80];
	//fread(dunno2, 1, 80, f);
	int dunno2[7];
	fread(dunno2, 4, 7, f);

	float dunno3[3];
	fread(dunno3, 4, 3, f);

	int dunno4[10];
	fread(dunno4, 4, 10, f);


	// load CMP
	char cmpName[32];
	fread(cmpName, 1, 32, f);


	CCMP cmp;
	cmp.newColormap();

	// if "colormap"  file ends with  .mat   then this baf does not have a colormap..  create a fake grayscale one.
	// theres probably something in baf header  (or maybe  numModels is actually numCmps)   but whatever we will haxx it
	bool nocolormap = false;
	if (cmpName[strlen(cmpName) - 1] == 't')
	{
		for (int x = 0; x < 256; x++)
			cmp.setColor(x, RGB(x, x, x));

		nocolormap = true;
		goto processmats;
	}

#if 0
	char* cmpdata = new char[812];
	fread(cmpdata, 1, 812, f);

	FILE* fc = fopen("colormap.hex", "wb");
	fwrite(cmpdata, 1, 812, fc);
	fclose(fc);

	delete[] cmpdata;
#else
	// generate valid CMP file
	//{


	// skip first 16 bytes?
	char cmpdumb[16];
	fread(cmpdumb, 1, 16, f);

	for (int x = 0; x < 256; x++)
	{
		char r, g, b;
		fread(&r, 1, 1, f);
		fread(&g, 1, 1, f);
		fread(&b, 1, 1, f);


		cmp.setColor(x, RGB(r, g, b));

	}


	// skip last 28 bytes?
	char cmpdumb2[28];
	fread(cmpdumb2, 1, 28, f);

#ifdef DUMPALL
	cmp.saveColormap(subfile(szdump, cmpName));
#endif
	//}
#endif


processmats:

	char** pszMatNames = new char* [numMats];

	// now time for mats!
	for (int matIndex = 0; matIndex < numMats; matIndex++)
	{
		char matName[32];

		if (nocolormap && matIndex == 0)
			strcpy(matName, cmpName);// if there was no colormap, then we already read the mat name
		else
			fread(matName, 1, 32, f);


		pszMatNames[matIndex] = new char[strlen(matName) + 1];
		strcpy(pszMatNames[matIndex], matName);


#ifdef DUMPALL
		FILE* fo = fopen(subfile(szdump, matName), "wb");
#endif

		tMAT_Header hdr;
		fread(&hdr, sizeof(tMAT_Header), 1, f);
#ifdef DUMPALL
		fwrite(&hdr, sizeof(tMAT_Header), 1, fo);
#endif

		for (int matI = 0; matI < hdr.numMaterials; matI++)
		{
			tMAT_MaterialHeader mathdr;
			fread(&mathdr, sizeof(tMAT_MaterialHeader), 1, f);
#ifdef DUMPALL
			fwrite(&mathdr, sizeof(tMAT_MaterialHeader), 1, fo);
#endif

			if (mathdr.type == 8)
			{
				tMAT_TextureHeader texhdr;
				fread(&texhdr, sizeof(tMAT_TextureHeader), 1, f);
#ifdef DUMPALL
				fwrite(&texhdr, sizeof(tMAT_TextureHeader), 1, fo);
#endif
			}
		}

		vector<mipimage> exportmips;

		for (int texI = 0; texI < hdr.numTextures; texI++)
		{
			tMAT_TextureData texdat;
			fread(&texdat, sizeof(tMAT_TextureData), 1, f);
#ifdef DUMPALL
			fwrite(&texdat, sizeof(tMAT_TextureData), 1, fo);
#endif

			int mipSize = (texdat.width * texdat.height * hdr.colorBits) / 8;

			for (int mipI = 0; mipI < texdat.numMipmaps; mipI++)
			{
				unsigned char* mip = new unsigned char[mipSize];

				fread(mip, 1, mipSize, f);
#ifdef DUMPALL
				fwrite(mip, 1, mipSize, fo);
#endif


				if (mipI == 0)
				{
					// save this mip for later
					mipimage mi;
					mi.width = texdat.width;
					mi.height = texdat.height;
					mi.trans = texdat.transparent != 0;
					mi.buf = mip;

					exportmips.push_back(mi);
				}
				else
					delete[] mip;// discard this submip


				mipSize /= 4;
			}
		}

		CCMP* pcmp = &cmp;
		CCMP matcmp;

		// NEW TYPE;   seems to specify that *.mat  includes its own palette at the end
		if (hdr.type == 3)
		{
			matcmp.newColormap();

			for (int x = 0; x < 256; x++)
			{
				char r, g, b;
				fread(&r, 1, 1, f);
				fread(&g, 1, 1, f);
				fread(&b, 1, 1, f);


				matcmp.setColor(x, RGB(r, g, b));
			}

			pcmp = &matcmp;
		}



		for (vector<mipimage>::iterator mipIter = exportmips.begin(); mipIter != exportmips.end(); mipIter++)
		{
			mipimage mi = (*mipIter);

			// only export first?
			if (mipIter == exportmips.begin())
			{
				BITMAPFILEHEADER bmFH;
				BITMAPINFOHEADER bmIH;

				ZeroMemory(&bmFH, sizeof(BITMAPFILEHEADER));
				ZeroMemory(&bmIH, sizeof(BITMAPINFOHEADER));

				// fill info header
				bmIH.biBitCount = 24;
				bmIH.biWidth = mi.width;
				bmIH.biHeight = -mi.height;
				bmIH.biPlanes = 1;
				bmIH.biSize = sizeof(BITMAPINFOHEADER);
				bmIH.biSizeImage = mi.width * mi.height * 3;

				// fill file header last
				bmFH.bfType = ('M' << 8) | 'B';
				bmFH.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
				bmFH.bfSize = bmFH.bfOffBits + bmIH.biSizeImage;

				char bmpfile[32];
				strcpy(bmpfile, matName);
				int sl = strlen(bmpfile);
				bmpfile[sl - 1] = 'p';
				bmpfile[sl - 2] = 'm';
				bmpfile[sl - 3] = 'b';

				assert(_CrtCheckMemory());

				char* outbmpfile = subfile(szdump, bmpfile);
				errno = 0;
				FILE* fb = fopen(outbmpfile, "wb");
				int fileerr = errno;

				fwrite(&bmFH, sizeof(BITMAPFILEHEADER), 1, fb);
				fwrite(&bmIH, sizeof(BITMAPINFOHEADER), 1, fb);

				for (int y = 0; y < mi.height; y++)
					for (int x = 0; x < mi.width; x++)
					{
						COLORREF clr;

						unsigned char pi = mi.buf[x + y * mi.width];
						if (mi.trans && pi == 0)
							clr = RGB(255, 0, 255);
						else
							clr = pcmp->getColor(pi);

						char r = GetRValue(clr);
						char g = GetGValue(clr);
						char b = GetBValue(clr);

						fwrite(&b, 1, 1, fb);
						fwrite(&g, 1, 1, fb);
						fwrite(&r, 1, 1, fb);

					}

				fclose(fb);
			}



			// export 16-bit mats?
			// only export first?
			if (mipIter == exportmips.begin())
			{
				bool hasTransparency = false;
				for (int y = 0; !hasTransparency && y < mi.height; y++)
					for (int x = 0; !hasTransparency && x < mi.width; x++)
					{
						COLORREF clr;

						unsigned char pi = mi.buf[x + y * mi.width];
						if (mi.trans && pi == 0)
							hasTransparency = true;
					}


				char mat16file[32];
				strcpy(mat16file, matName);
				int sl = strlen(mat16file);
				mat16file[sl - 4] = 0;
				strcat(mat16file, "16.mat");

				char* outmat16file = subfile(szdump, mat16file);
				FILE* fb = fopen(outmat16file, "wb");


				tMAT_Header mh;
				mh.verification[0] = 'M';
				mh.verification[1] = 'A';
				mh.verification[2] = 'T';
				mh.verification[3] = ' ';
				mh.version = 0x32;
				mh.type = 2;
				mh.numMaterials = 1;
				mh.numTextures = 1;
				if (hasTransparency)
				{
#if 1
					mh.unknown0 = 0;
					mh.colorBits = 16;
					mh.redBits = 5;
					mh.greenBits = 5;
					mh.blueBits = 5;
					mh.redShiftL = 10;
					mh.greenShiftL = 5;
					mh.blueShiftL = 0;
					mh.redShiftR = 3;
					mh.greenShiftR = 3;
					mh.blueShiftR = 3;
					mh.alphaBits = 1;
					mh.alphaShiftL = 15;
					mh.alphaShiftR = 1;
#else
					mh.unknown0 = 3;//RGBA
					mh.colorBits = 16;
					mh.redBits = 5;
					mh.greenBits = 5;
					mh.blueBits = 5;
					mh.redShiftL = 11;
					mh.greenShiftL = 6;
					mh.blueShiftL = 1;
					mh.redShiftR = 3;
					mh.greenShiftR = 3;
					mh.blueShiftR = 3;
					mh.alphaBits = 1;
					mh.alphaShiftL = 0;
					mh.alphaShiftR = 7;
#endif
				}
				else
				{
					mh.unknown0 = 1;
					mh.colorBits = 16;
					mh.redBits = 5;
					mh.greenBits = 6;
					mh.blueBits = 5;
					mh.redShiftL = 11;
					mh.greenShiftL = 5;
					mh.blueShiftL = 0;
					mh.redShiftR = 3;
					mh.greenShiftR = 2;
					mh.blueShiftR = 3;
					mh.alphaBits = 0;
					mh.alphaShiftL = 0;
					mh.alphaShiftR = 0;
				}

				fwrite(&mh, sizeof(tMAT_Header), 1, fb);


				int dw;


				// material header
				dw = 8;
				fwrite(&dw, 4, 1, fb);

				dw = 0;
				fwrite(&dw, 4, 1, fb);

				for (int x = 0; x < 4; x++)
				{
					dw = 0x3F800000;
					fwrite(&dw, 4, 1, fb);
				}

				dw = 0;
				fwrite(&dw, 4, 1, fb);

				dw = 0;
				fwrite(&dw, 4, 1, fb);

				dw = 0xBFF78482;
				fwrite(&dw, 4, 1, fb);

				dw = 0;// texture index
				fwrite(&dw, 4, 1, fb);



				// texture
				fwrite(&mi.width, 4, 1, fb);
				fwrite(&mi.height, 4, 1, fb);

				dw = mi.trans ? 1 : 0;
				fwrite(&dw, 4, 1, fb);

				dw = 0;
				fwrite(&dw, 4, 1, fb);

				dw = 0;
				fwrite(&dw, 4, 1, fb);

				dw = 1;// num mipmaps
				fwrite(&dw, 4, 1, fb);


				if (hasTransparency)
				{
					for (int y = 0; y < mi.height; y++)
						for (int x = 0; x < mi.width; x++)
						{
							unsigned char pi = mi.buf[x + y * mi.width];
							if (mi.trans && pi == 0)
							{
								short t = 0;
								fwrite(&t, 2, 1, fb);
							}
							else
							{
								COLORREF clr = pcmp->getColor(pi);

								unsigned short r = (char)((float)GetRValue(clr) / 255.0f * 31.0f);
								unsigned short g = (char)((float)GetGValue(clr) / 255.0f * 31.0f);
								unsigned short b = (char)((float)GetBValue(clr) / 255.0f * 31.0f);

								unsigned short t = 0x8000;//alpha

								t |= r << 10;
								t |= g << 5;
								t |= b;

								fwrite(&t, 2, 1, fb);
							}

						}
				}
				else
				{
					for (int y = 0; y < mi.height; y++)
						for (int x = 0; x < mi.width; x++)
						{
							unsigned char pi = mi.buf[x + y * mi.width];

							COLORREF clr = pcmp->getColor(pi);

							unsigned short r = (char)((float)GetRValue(clr) / 255.0f * 31.0f);
							unsigned short g = (char)((float)GetGValue(clr) / 255.0f * 63.0f);
							unsigned short b = (char)((float)GetBValue(clr) / 255.0f * 31.0f);

							unsigned short t = 0;

							t |= r << 11;
							t |= g << 5;
							t |= b;

							fwrite(&t, 2, 1, fb);

						}
				}


				fclose(fb);
			}





			delete[] mi.buf;
		}

#ifdef DUMPALL
		fclose(fo);
#endif
	}


	float worldScale = 0.2f;



processmodels:

	// time for models!
	for (int modelIndex = 0; modelIndex < numModels; modelIndex++)
	{
		loadmodel(szdump, numMats, pszMatNames, worldScale, f);
	}


processkeys:

	// time for keys!
	for (int keyIndex = 0; keyIndex < numKeys; keyIndex++)
	{
		loadkey(szdump, worldScale, f);
	}


#ifdef DUMPALL
	if (true)
	{
		long fcur = ftell(f);
		fseek(f, 0, SEEK_END);
		long fend = ftell(f);
		fseek(f, fcur, SEEK_SET);

		int dumplen = fend - fcur;
		char* sigh = new char[dumplen];

		fread(sigh, 1, dumplen, f);


		char* funame = subfile(szdump, "REMAIN.HEX");
		FILE* fu = fopen(funame, "wb");
		fwrite(sigh, 1, dumplen, fu);
		fclose(fu);

		delete[] sigh;
	}
#endif



	//delete[] cmpdata;

	fclose(f);


	for (int x = 0; x < numMats; x++)
		delete[] pszMatNames[x];
	delete[] pszMatNames;


	assert(_CrtCheckMemory());
}