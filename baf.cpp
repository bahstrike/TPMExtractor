// Strike
// Bad Ass Hackers 2021-2024
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): fixed-width on-disk structs (the
// tool now runs on 64-bit hosts with the same output), text files through TextWriter (CRLF
// and %f identical on every platform), BMPs through the bmp module, Options threaded through,
// names from data checked with platform::safe_output_path, zeroed debug "tail" when the read
// hits end of file, errors reported instead of crashing.
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): default mode parses the real BAF
// layout (bounds-checked, header fields validated) and writes JK-ready 3DO/KEY values plus
// retail CMPs and 8-bit MATs; --legacy keeps the original sequential read, now from memory
// with every count bounded, and still writes the original's output byte for byte.
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): legacy texture sizes checked before
// any product that could overflow; default mode keeps own palettes in the raw MAT copies, gives
// a colormap-less BAF the colormap its 8-bit MATs index, declares every model node in KEY
// JOINTS, names scaled 3DOs and KEYs apart and warns when 8-bit MATs lose their colours.
//
// The BAF layout (little-endian; the game loads it as a series of in-memory struct dumps, so
// pointer fields hold stale editor values):
//   header, 0x120 bytes: magic 0x00010001, header size 0x120, name[32], source path[128],
//     +0xA8 runtime allocation size, +0xAC draw scale (float), +0xC0 model size,
//     +0xC4 material count, +0xC8 key count, +0xCC u16 has_colormap, +0xCE and +0xCF flag
//     bytes, +0xD0 materials section size, +0xD4 key section size, +0xD8..+0xE7 payload offsets
//     (relative to 0x120) of the colormap, the material table, the model and the keys
//   colormap block, 0x34C bytes, only when has_colormap: name[32], flags, tint[3], palette[768],
//     7 runtime pointers
//   materials: per material name[32] + a MAT stream: header 0x4C, cels 0x18 each (+0x10 when the
//     cel type has bit 3), textures 0x18 + mips of w*h*bpp/8 bytes (sides halving), and a
//     768-byte palette when the MAT type has bit 0
//   exactly one model: rdModel3 header 0x84, geoset-0 meshes 0x70 each; per mesh vertices[nV],
//     normals[nV], vertices_i[nV], vertices_unk[nV] (floats), UVs[nUV], faces 0x4C each, then
//     per face its vertex indices and, only when the face's material has type bit 1, its UV
//     indices; then the hierarchy nodes, 0xB4 each
//   keys: per key a 0x40-byte wrapper (+0x00 block size, +0x1C sound-event count), rdKeyframe
//     0x84, joints 0x2C each, then each joint's entries, 0x38 each
//   the keys' sound-event records, 0x60 bytes each in key order, then the end of the file.
// Angles are degrees with JK's pitch/yaw/roll convention, axes are JK's, and the hierarchy
// composes as in JK, so positions only need a scale.
//
// The original tool took numModels = int@0xCC & 0xFFFFFF (really has_colormap plus two flag
// bytes), recognised a missing colormap by a name ending in 't', always read per-face UV indices
// and printed the 64 bytes after each model and key as a "tail" (the next key's wrapper).
// --legacy keeps all of that. Default mode reads the layout above.

#include "baf.h"

#include "binreader.h"
#include "bmp.h"
#include "jkout.h"
#include "log.h"
#include "platform.h"
#include "textout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace tpmx {

namespace {

// =========================================================================================
// Shared by both modes
// =========================================================================================

const double kPi = 3.14159265358979323846;  // M_PI (MSVC lacks it without _USE_MATH_DEFINES)

inline uint32_t ld_u32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int32_t ld_i32(const uint8_t* p) { return (int32_t)ld_u32(p); }
inline float ld_f32(const uint8_t* p)
{
	const uint32_t u = ld_u32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}

// The original printed int bits reinterpreted as float with *(float*)&i.
inline float as_float(int32_t i)
{
	float f;
	memcpy(&f, &i, sizeof f);
	return f;
}

// A NUL-terminated name read from a fixed-size field: at most `n` bytes.
std::string fixed_str(const void* p, size_t n)
{
	const char* s = static_cast<const char*>(p);
	size_t len = 0;
	while (len < n && s[len])
		++len;
	return std::string(s, len);
}

// The original printed struct name fields with %s, which could run past the declared array
// into the following fields of the same struct; this keeps that output, bounded by the struct.
template <class T>
std::string struct_str(const T& s)
{
	return fixed_str(&s, sizeof(T));
}

// (int)f the way the original's x86 code converted it: a value outside int's range, or NaN,
// gives INT_MIN (the "integer indefinite"); a plain cast would be undefined behaviour there.
int float_to_int(float f)
{
	if (f >= -2147483648.0f && f < 2147483648.0f)
		return (int)f;
	return std::numeric_limits<int>::min();
}

// "<name minus its last 4 characters>16.mat" (the original's matname[sl - 4] = 0; strcat "16.mat")
std::string mat16_name(const std::string& matName)
{
	return (matName.size() >= 4 ? matName.substr(0, matName.size() - 4) : matName) + "16.mat";
}

// the last three characters replaced by "bmp"
std::string bmp_name(const std::string& matName)
{
	return matName.size() >= 3 ? matName.substr(0, matName.size() - 3) + "bmp" : matName + ".bmp";
}

// Output path for a name from the BAF; logs and returns false for unsafe names.
bool out_path(const std::string& szdump, const std::string& name, const std::string& baf, std::string& out)
{
	std::string why;
	if (platform::safe_output_path(szdump, name, out, &why))
		return true;
	log::error("%s: %s\n", baf.c_str(), why.c_str());
	return false;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
	std::string why;
	if (platform::write_file(path, bytes, &why))
		return true;
	log::error("%s\n", why.c_str());
	return false;
}

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
	out.push_back((uint8_t)v);
	out.push_back((uint8_t)(v >> 8));
	out.push_back((uint8_t)(v >> 16));
	out.push_back((uint8_t)(v >> 24));
}

// The original's 16-bit MAT for one texture (mip 0 only): ARGB1555 when the texture is
// transparent and uses index 0, RGB565 otherwise; header values and texel rounding as the
// original wrote them. MotS/OpenJKDF2 only (retail JK draws 8-bit MATs).
std::vector<uint8_t> encode_mat16(int width, int height, bool trans, const uint8_t* pix, const Palette& pal)
{
	const size_t count = (size_t)width * (size_t)height;
	bool hasTransparency = false;
	for (size_t i = 0; trans && !hasTransparency && i < count; i++)
		hasTransparency = pix[i] == 0;

	std::vector<uint8_t> out;
	out.reserve(76 + 64 + count * 2);
	out.insert(out.end(), {'M', 'A', 'T', ' '});
	put_u32(out, 0x32);  // version
	put_u32(out, 2);     // type
	put_u32(out, 1);     // numMaterials
	put_u32(out, 1);     // numTextures
	if (hasTransparency)
	{
		// unknown0, colorBits, blue/green/red bits, blue/green/red shift left, ... shift right,
		// alpha bits, alpha shift left, alpha shift right
		// (the original's disabled alternative: unknown0 = 3 "RGBA", shifts 11/6/1, alpha shifts 0/7)
		for (uint32_t v : {0u, 16u, 5u, 5u, 5u, 0u, 5u, 10u, 3u, 3u, 3u, 1u, 15u, 1u})
			put_u32(out, v);
	}
	else
	{
		for (uint32_t v : {1u, 16u, 5u, 6u, 5u, 0u, 5u, 11u, 3u, 2u, 3u, 0u, 0u, 0u})
			put_u32(out, v);
	}
	// material header {8, 0, 1.0f x4}, texture header {0, 0, 0xBFF78482, texture index 0}
	for (uint32_t v : {8u, 0u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0u, 0u, 0xBFF78482u, 0u})
		put_u32(out, v);
	// texture: width, height, transparent, 0, 0, one mip
	put_u32(out, (uint32_t)width);
	put_u32(out, (uint32_t)height);
	put_u32(out, trans ? 1u : 0u);
	put_u32(out, 0);
	put_u32(out, 0);
	put_u32(out, 1);

	for (size_t i = 0; i < count; i++)
	{
		const uint8_t pi = pix[i];
		unsigned short t = 0;
		if (hasTransparency)
		{
			if (pi != 0)  // (trans && pi == 0) stays 0
			{
				const Rgb c = pal.c[pi];
				const unsigned short r = (unsigned short)(int)((float)c.r / 255.0f * 31.0f);
				const unsigned short g = (unsigned short)(int)((float)c.g / 255.0f * 31.0f);
				const unsigned short b = (unsigned short)(int)((float)c.b / 255.0f * 31.0f);
				t = (unsigned short)(0x8000 | (r << 10) | (g << 5) | b);
			}
		}
		else
		{
			const Rgb c = pal.c[pi];
			const unsigned short r = (unsigned short)(int)((float)c.r / 255.0f * 31.0f);
			const unsigned short g = (unsigned short)(int)((float)c.g / 255.0f * 63.0f);
			const unsigned short b = (unsigned short)(int)((float)c.b / 255.0f * 31.0f);
			t = (unsigned short)((r << 11) | (g << 5) | b);
		}
		out.push_back((uint8_t)t);
		out.push_back((uint8_t)(t >> 8));
	}
	return out;
}

// The BMP preview and the 16-bit MAT of a material's first texture (mip 0), as the original
// wrote them. `rows` is the BMP row layout (unpadded for --legacy).
bool write_preview_and_mat16(const std::string& szdump, const std::string& baf, const std::string& matName,
                             int width, int height, bool trans, const std::vector<uint8_t>& pix, const Palette& pal,
                             const Options& opts, BmpRows rows)
{
	if (width < 1 || height < 1 || pix.size() < (size_t)width * (size_t)height)
	{
		log::error("%s: material %s: texture %dx%d has %u bytes of pixels; not exported\n", baf.c_str(),
			matName.c_str(), width, height, (unsigned)pix.size());
		return false;
	}
	bool ok = true;
	std::string path, why;
	if (opts.bmp)
	{
		if (out_path(szdump, bmp_name(matName), baf, path))
		{
			if (!bmp24_write_indexed(path, width, height, pix.data(), pal, trans, rows, &why))
			{
				log::error("%s\n", why.c_str());
				ok = false;
			}
		}
		else
			ok = false;
	}
	if (opts.mat16)
	{
		if (out_path(szdump, mat16_name(matName), baf, path))
			ok = write_bytes(path, encode_mat16(width, height, trans, pix.data(), pal)) && ok;
		else
			ok = false;
	}
	return ok;
}

// =========================================================================================
// --legacy: the original's sequential read (dumpbaf/loadmodel/loadkey)
// =========================================================================================

struct tMAT_Header
{
	char verification[4];		// 'MAT '
	int32_t version;				// version of material.   should be 0x32
	int32_t type;					// type of material.  0 = color       1 = ?         2 = texture
	int32_t numMaterials;			// number of materials contained in material file
	int32_t numTextures;			// number of textures. in color materials, this is 0. in texture materials, this is equal to numMaterials
	int32_t unknown0;				// JKSpecs says this is 0.  BoBo Fett's mat-16 spec says this is 1
	int32_t colorBits;				// number of bits per pixel
	int32_t blueBits;				// number of blue bits per pixel (could this be red?)
	int32_t greenBits;				// number of green bits per pixel
	int32_t redBits;				// number of red bits per pixel (could this be blue?)
	int32_t blueShiftL;			// blue left shift
	int32_t greenShiftL;			// green left shift
	int32_t redShiftL;				// red left shift
	int32_t blueShiftR;			// blue right shift
	int32_t greenShiftR;			// green right shift
	int32_t redShiftR;				// red right shift
	//char unknown1[12];			// unknown/unused bytes
	int32_t alphaBits;
	int32_t alphaShiftL;
	int32_t alphaShiftR;
};
static_assert(sizeof(tMAT_Header) == 76, "tMAT_Header is 76 bytes on disk");

// Texture material header
struct tMAT_TextureHeader
{
	int32_t unknown0;				// unknown	( BoBo Fett reports this as being 0 )
	int32_t unknown1;				// unknown  ( BoBo Fett reports this as being 4 )
	int32_t unknown2;				// unknown  ( JKSpecs reports this as being 0xBFF78482, BoBo Fett reports this as being 4 )
	int32_t textureId;				// current texture ID, starting at 0 up to numTextures - 1
};
static_assert(sizeof(tMAT_TextureHeader) == 16, "tMAT_TextureHeader is 16 bytes on disk");

// Material entry header
struct tMAT_MaterialHeader
{
	int32_t type;					// type of material entry (0 = color    8 = texture)
	int32_t colorIndex;			// for color materials, this is the palette entry in CMP for the material color. for textures, this is RGB of transparency value
	int32_t unknown[4];			// unknown/unused bytes   ( JKSpecs reports these are all 0x3F800000 )
	//struct tMAT_TextureHeader *textureHeader;		// pointer to texture header (NULL for color materials)
};
static_assert(sizeof(tMAT_MaterialHeader) == 24, "tMAT_MaterialHeader is 24 bytes on disk");

// Texture material data
struct tMAT_TextureData
{
	int32_t width;					// width of texture
	int32_t height;				// height of texture
	int32_t transparent;			// bool to indictate using transparency
	int32_t unknown[2];			// unknown/unused bytes
	int32_t numMipmaps;			// number of mipmaps for texture
	//void **pData;				// actual bitmap data (array of pointers to bytes, numMipmaps large)
};
static_assert(sizeof(tMAT_TextureData) == 24, "tMAT_TextureData is 24 bytes on disk");

struct mipimage
{
	int width;
	int height;
	bool trans;
	std::vector<unsigned char> buf;
};


struct MODELNODEHEADER
{
	char name[32];
	int32_t someIndex1;
	int32_t geoMode;
	int32_t lightMode;
	int32_t texMode;
	int32_t ndunno[6];
	int32_t numVerts;
	int32_t numTexVerts;
	int32_t numFaces;
	float fdunno[7];
};
static_assert(sizeof(MODELNODEHEADER) == 112, "MODELNODEHEADER is 112 bytes on disk");

struct MODELFACEHEADER
{
	int32_t matIndex;
	int32_t flags;
	int32_t geoMode;
	int32_t lightMode;
	int32_t texMode;
	int32_t numVerts;

	int32_t dunno5; // pointer placeholder
	int32_t dunno6; // pointer placeholder
	int32_t dunno7; // pointer placeholder

	int32_t dunno8; // usually -1
	int32_t dunno9; // usually 0
	int32_t dunno10; // usually 0
	int32_t dunno11; // usually 0
	int32_t dunno12; // usually 0

	float nx, ny, nz;

	int32_t dunno13;
	int32_t dunno14;
};
static_assert(sizeof(MODELFACEHEADER) == 76, "MODELFACEHEADER is 76 bytes on disk");

// name[8] + hdunno[0..5] are really one name[32]: names longer than 7 characters
// continue into hdunno, and the original printed them with %s across that boundary.
struct MODELHIERARCHYNODEHEADER
{
	char name[8];
	int32_t hdunno[17];
	int32_t meshIndex;
	int32_t parentIndex;
	int32_t numChildren;
	int32_t firstChildIndex;
	int32_t siblingIndex;
	float px, py, pz;
	float x, y, z;
	float pitch, yaw, roll;
	int32_t hdunno2[12];
};
static_assert(sizeof(MODELHIERARCHYNODEHEADER) == 180, "MODELHIERARCHYNODEHEADER is 180 bytes on disk");

struct MODELFACE
{
	MODELFACEHEADER hdr;

	std::vector<int32_t> pnFaceIndices; // all vert indices, then all texvert indices
};

struct MODELNODE
{
	MODELNODEHEADER hdr;

	std::vector<float> pVerts; // 3d
	std::vector<float> pVertNorms; // 3d
	std::vector<float> pVertUnknowns;  // 2d
	std::vector<float> pTexVerts; // 2d

	std::vector<MODELFACE> pFaces;
};

struct KEYHEADER
{
	char name[32];
	int32_t dunno1;
	int32_t dunno2;
	int32_t dunno3;
	int32_t type;
	float fps;
	int32_t numFrames;
	int32_t numNodes;
	int32_t bleh[18];
};
static_assert(sizeof(KEYHEADER) == 132, "KEYHEADER is 132 bytes on disk");

struct KEYFRAME
{
	float frame;
	int32_t flags;
	float x, y, z;
	float pitch, yaw, roll;
	float dx, dy, dz;
	float dpitch, dyaw, droll;
};
static_assert(sizeof(KEYFRAME) == 56, "KEYFRAME is 56 bytes on disk");

// On disk the original's KEYNODEHEADER ends with a 32-bit pointer field (44 bytes on Win32);
// the in-memory frame array is kept separately so the layout no longer depends on the host.
struct KEYNODEHEADER
{
	char name[32];
	int32_t someIndex;
	int32_t numFrames;
	uint32_t pFrames_placeholder;
};
static_assert(sizeof(KEYNODEHEADER) == 44, "KEYNODEHEADER is 44 bytes on disk");

struct KEYNODE
{
	KEYNODEHEADER hdr;
	std::vector<KEYFRAME> pFrames;
};

// The original's fread() calls, over the file image in memory. read() copies what is left and
// zero-fills the rest of the destination (the original left it uninitialised) and counts the
// short read; the original ignored them. On the retail disc the only short reads are the 153
// "TAIL 64bytes" reads at the end of the file.
// check_count() guards every count the original used to allocate or loop: a negative count
// (where the original's new[] failed) or one the rest of the file cannot hold is an error with
// the file and offset, instead of the original's crash or near-endless loop. Counts of valid
// files always fit, so the output for them is unchanged.
class LegacyReader
{
public:
	LegacyReader(const std::vector<uint8_t>& data, const std::string& name) : data_(data), name_(name) {}

	void read(void* dst, size_t n)
	{
		const size_t avail = data_.size() - pos_;
		const size_t k = n < avail ? n : avail;
		if (k)
			memcpy(dst, data_.data() + pos_, k);
		if (k < n)
		{
			memset(static_cast<char*>(dst) + k, 0, n - k);
			++short_reads_;
		}
		pos_ += k;
	}

	void check_count(int64_t count, uint64_t min_element_size, const char* what) const
	{
		if (count < 0)
			throw ParseError(name_, pos_, format("negative %s count %lld", what, (long long)count));
		if ((uint64_t)count * min_element_size > remaining())
			throw ParseError(name_, pos_, format("%s count %lld needs at least %llu bytes, %llu left", what,
				(long long)count, (unsigned long long)((uint64_t)count * min_element_size),
				(unsigned long long)remaining()));
	}

	[[noreturn]] void fail_at(size_t at, const std::string& message) const { throw ParseError(name_, at, message); }

	size_t pos() const { return pos_; }
	size_t remaining() const { return data_.size() - pos_; }
	const uint8_t* here() const { return data_.data() + pos_; }
	unsigned short_reads() const { return short_reads_; }

private:
	const std::vector<uint8_t>& data_;
	const std::string& name_;
	size_t pos_ = 0;
	unsigned short_reads_ = 0;
};

// Fewest bytes the original read per item before allocating or looping over a count.
const uint64_t kLegacyVertexBytes = 12 + 12 + 8;       // position, normal, "uvscroll?" pair
const uint64_t kLegacyFaceIndexBytes = 8;              // vertex index + texture vertex index
const uint64_t kLegacyModelBytes = 32 + 4 + 4 + 92;    // name, mdunno1, numNodes, mdunno[23]

// Bytes of a texture's first mip, (width * height * colorBits) / 8 as the original computed it
// in int arithmetic: the same value for every mip that fits in a file. `at` is the file offset
// of the texture header. A negative field (where the original's new[] failed) or a mip larger
// than the rest of the file is an error there, found before any product that could overflow.
int64_t legacy_mip_bytes(const LegacyReader& f, size_t at, const tMAT_TextureData& t, int32_t colorBits)
{
	if (t.width < 0 || t.height < 0 || colorBits < 0)
		f.fail_at(at, format("texture %dx%d at %d bits per pixel: a negative size", t.width, t.height, colorBits));
	const uint64_t texels = (uint64_t)t.width * (uint64_t)t.height;  // below 2^62
	const uint64_t bits = (uint64_t)colorBits;
	// texels * bits / 8 <= remaining exactly when texels <= (remaining * 8 + 7) / bits
	if (bits != 0 && texels > ((uint64_t)f.remaining() * 8 + 7) / bits)
		f.fail_at(at, format("texture %dx%d at %d bits per pixel needs more than the %llu bytes left", t.width,
			t.height, colorBits, (unsigned long long)f.remaining()));
	return (int64_t)(texels * bits / 8);
}

void legacy_loadmodel(LegacyReader& f, const std::string& szdump, const std::string& baf, int nNumMats,
                      const std::vector<std::string>& pszMatNames, float worldScale, const Options& opts)
{
	char modelName[32] = {0};
	f.read(modelName, 32);
	const std::string modelNameStr = fixed_str(modelName, sizeof modelName);

	std::string modelfile;
	if (!out_path(szdump, modelNameStr, baf, modelfile))
		throw std::runtime_error("unsafe model name");
	std::string modelfile2 = modelfile + ".txt";
	TextWriter mf;
	if (opts.debug_dumps)
		mf.open(modelfile2);
	else
		mf.discard();


	mf.printf("\n---------------------------------------------------------\n");
	mf.printf("-------------------- HEADER ------------------------\n");
	mf.printf("---------------------------------------------------------\n");



	int32_t mdunno1 = 0;
	f.read(&mdunno1, 4);
	mf.printf("mdunno1=%d\n", mdunno1);

	int32_t numNodes = 0;
	f.read(&numNodes, 4);
	mf.printf("numNodes=%d\n", numNodes);

	int32_t mdunno[23] = {0};
	f.read(mdunno, 4 * 23);
	for (int ii = 0; ii < 23; ii++)
	{
		mf.printf("mdunno[%d]=%d\n", ii, mdunno[ii]);
	}

	int numHierarchyNodes = mdunno[11];




	std::vector<MODELNODE> pNodes;
	if (numNodes > 0)
	{
		mf.printf("\n---------------------------------------------------------\n");
		mf.printf("-------------------- NODE DEFINITIONS ------------------------\n");
		mf.printf("---------------------------------------------------------\n");


		f.check_count(numNodes, sizeof(MODELNODEHEADER), "mesh");
		pNodes.resize((size_t)numNodes);

		for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
		{
			mf.printf("\n----------------\n");

			MODELNODE* node = &pNodes[nodeIndex];

			memset(&node->hdr, 0, sizeof(MODELNODEHEADER));
			f.read(&node->hdr, sizeof(MODELNODEHEADER));

			mf.printf("nodeName=%s\n", struct_str(node->hdr).c_str());
			mf.printf("someIndex1=%d\n", node->hdr.someIndex1);
			mf.printf("geoMode=%d\n", node->hdr.geoMode);
			mf.printf("lightMode=%d\n", node->hdr.lightMode);
			mf.printf("texMode=%d\n", node->hdr.texMode);

			for (int ii = 0; ii < 6; ii++)
			{
				mf.printf("ndunno[%d]=%d\n", ii, node->hdr.ndunno[ii]);
			}

			mf.printf("numVerts=%d\n", node->hdr.numVerts);
			mf.printf("numTexVerts=%d\n", node->hdr.numTexVerts);
			mf.printf("numFaces=%d\n", node->hdr.numFaces);

			for (int ii = 0; ii < 7; ii++)
			{
				mf.printf("fdunno[%d]=%f\n", ii, node->hdr.fdunno[ii]);
			}

		}
	}


	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- NODES!!!!!!! ------------------------\n");
	mf.printf("---------------------------------------------------------\n");

	for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
	{
		MODELNODE* node = &pNodes[nodeIndex];

		mf.printf("\nnode: %s\n---------------------------\n", struct_str(node->hdr).c_str());

		if (node->hdr.numVerts != 0)
		{
			f.check_count(node->hdr.numVerts, kLegacyVertexBytes, "vertex");
			node->pVerts.resize((size_t)node->hdr.numVerts * 3);
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx = 0, fy = 0, fz = 0;
				f.read(&fx, 4);
				f.read(&fy, 4);
				f.read(&fz, 4);

				node->pVerts[ii * 3 + 0] = fx;
				node->pVerts[ii * 3 + 1] = fy;
				node->pVerts[ii * 3 + 2] = fz;

				mf.printf("vert[%d]: %f/%f/%f\n", ii, fx, fy, fz);
			}

			node->pVertNorms.resize((size_t)node->hdr.numVerts * 3);
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx = 0, fy = 0, fz = 0;
				f.read(&fx, 4);
				f.read(&fy, 4);
				f.read(&fz, 4);

				node->pVertNorms[ii * 3 + 0] = fx;
				node->pVertNorms[ii * 3 + 1] = fy;
				node->pVertNorms[ii * 3 + 2] = fz;

				float mag = sqrtf(fx * fx + fy * fy + fz * fz);

				mf.printf("norm[%d]: mag=%f\t%f/%f/%f\n", ii, mag, fx, fy, fz);
			}

			// These are really two arrays (vertices_i[nV], then vertices_unk[nV]); the original
			// read them as interleaved pairs (the byte count is the same).
			node->pVertUnknowns.resize((size_t)node->hdr.numVerts * 2);
			for (int ii = 0; ii < node->hdr.numVerts; ii++)
			{
				float fx = 0, fy = 0;
				f.read(&fx, 4);
				f.read(&fy, 4);

				node->pVertUnknowns[ii * 2 + 0] = fx;
				node->pVertUnknowns[ii * 2 + 1] = fy;

				mf.printf("uvscroll?[%d]: %f/%f\n", ii, fx, fy);
			}
		}

		if (node->hdr.numTexVerts != 0)
		{
			f.check_count(node->hdr.numTexVerts, 8, "texture vertex");
			node->pTexVerts.resize((size_t)node->hdr.numTexVerts * 2);
			for (int ii = 0; ii < node->hdr.numTexVerts; ii++)
			{
				float fx = 0, fy = 0;
				f.read(&fx, 4);
				f.read(&fy, 4);

				node->pTexVerts[ii * 2 + 0] = fx;
				node->pTexVerts[ii * 2 + 1] = fy;

				mf.printf("texvert[%d]: %f/%f\n", ii, fx, fy);
			}
		}

		if (node->hdr.numFaces != 0)
		{
			f.check_count(node->hdr.numFaces, sizeof(MODELFACEHEADER), "face");
			node->pFaces.resize((size_t)node->hdr.numFaces);


			for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
			{
				MODELFACE* face = &node->pFaces[faceIndex];

				memset(&face->hdr, 0, sizeof(MODELFACEHEADER));
				f.read(&face->hdr, sizeof(MODELFACEHEADER));

				const char* matName = (face->hdr.matIndex >= 0 && face->hdr.matIndex < (int)pszMatNames.size())
					? pszMatNames[face->hdr.matIndex].c_str() : "(invalid material index)";

				mf.printf("face[%d]: matIndex=%d (%s), flags=%d, geo=%d, light=%d, tex=%d, numVerts=%d, d5=0x%08X, d6=0x%08X, d7=0x%08X, d8=%d, d9=%d, d10=%d, d11=%d, d12=%d, norm=(%f/%f/%f), d13=%d, d14=%d\n", faceIndex,
					face->hdr.matIndex, matName, face->hdr.flags, face->hdr.geoMode, face->hdr.lightMode, face->hdr.texMode, face->hdr.numVerts,
					(unsigned)face->hdr.dunno5, (unsigned)face->hdr.dunno6, (unsigned)face->hdr.dunno7,
					face->hdr.dunno8, face->hdr.dunno9, face->hdr.dunno10, face->hdr.dunno11, face->hdr.dunno12,
					face->hdr.nx, face->hdr.ny, face->hdr.nz,
					face->hdr.dunno13, face->hdr.dunno14
				);
			}



			for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
			{
				MODELFACE* face = &node->pFaces[faceIndex];

				f.check_count(face->hdr.numVerts, kLegacyFaceIndexBytes, "face vertex");
				face->pnFaceIndices.resize((size_t)face->hdr.numVerts * 2);

				mf.printf("faceindices[%d]=", faceIndex);

				for (int ii = 0; ii < face->hdr.numVerts; ii++)
				{
					int32_t vi = 0;
					f.read(&vi, 4);

					face->pnFaceIndices[ii] = vi;

					mf.printf("%d,", vi);
				}
				mf.printf(" | ");
				for (int ii = 0; ii < face->hdr.numVerts; ii++)
				{
					int32_t vi = 0;
					f.read(&vi, 4);

					face->pnFaceIndices[face->hdr.numVerts + ii] = vi;

					mf.printf("%d,", vi);
				}

				mf.printf("\n");
			}
		}

	}

	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- NODE HIERARCHY ------------------------\n");
	mf.printf("---------------------------------------------------------\n");

	std::vector<MODELHIERARCHYNODEHEADER> pHNodes;
	if (numHierarchyNodes > 0)
	{
		f.check_count(numHierarchyNodes, sizeof(MODELHIERARCHYNODEHEADER), "hierarchy node");
		pHNodes.resize((size_t)numHierarchyNodes);

		for (int hierNodeIndex = 0; hierNodeIndex < numHierarchyNodes; hierNodeIndex++)
		{
			MODELHIERARCHYNODEHEADER* hnode = &pHNodes[hierNodeIndex];

			memset(hnode, 0, sizeof(MODELHIERARCHYNODEHEADER));
			f.read(hnode, sizeof(MODELHIERARCHYNODEHEADER));

			mf.printf("%d: hierarchy node=%s\n", hierNodeIndex, struct_str(*hnode).c_str());

			for (int ii = 0; ii < 17; ii++)
			{
				mf.printf("hdunno[%d]=%d\t%f\n", ii, hnode->hdunno[ii], as_float(hnode->hdunno[ii]));
			}

			mf.printf("meshIndex=%d\n", hnode->meshIndex);
			mf.printf("parentIndex=%d\n", hnode->parentIndex);
			mf.printf("numChildren=%d\n", hnode->numChildren);
			mf.printf("firstChildIndex=%d\n", hnode->firstChildIndex);
			mf.printf("siblingIndex=%d\n", hnode->siblingIndex);
			mf.printf("pivot=%f/%f/%f\n", hnode->px, hnode->py, hnode->pz);
			mf.printf("pos=%f/%f/%f\n", hnode->x, hnode->y, hnode->z);
			mf.printf("pyr=%f/%f/%f\n", hnode->pitch, hnode->yaw, hnode->roll);

			for (int ii = 0; ii < 12; ii++)
			{
				mf.printf("hdunno2[%d]=%d\t%f\n", ii, hnode->hdunno2[ii], as_float(hnode->hdunno2[ii]));
			}

			mf.printf("\n");
		}
	}


	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- TAIL 64bytes ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	{
		// At the end of the file this read returns nothing; the original then printed
		// uninitialised stack memory, different on every run. The fork prints zeros there.
		int32_t tail[16] = {0};
		f.read(tail, 4 * 16);

		for (int ii = 0; ii < 16; ii++)
		{
			mf.printf("tail[%d]=%d\t%f\n", ii, tail[ii], as_float(tail[ii]));
		}
	}


	std::string err;
	if (!mf.close(&err))
		log::error("%s\n", err.c_str());



	if (!mf.open(modelfile, &err))
	{
		log::error("%s\n", err.c_str());
		return;
	}
	mf.printf("# MODEL '%s' created with TPMExtractor\n", modelNameStr.c_str());
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HEADER\n");
	mf.printf("\n");
	mf.printf("3DO 2.1\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: MODELRESOURCE\n");
	mf.printf("\n");
	mf.printf("# Materials list\n");
	mf.printf("MATERIALS %d\n", nNumMats);
	mf.printf("\n");
	for (int x = 0; x < nNumMats; x++)
	{
		mf.printf("%d: %s\n", x, mat16_name(pszMatNames[x]).c_str());//pszMatNames[x]);
	}
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: GEOMETRYDEF\n");
	mf.printf("\n");
	mf.printf("# Object radius\n");
	mf.printf("RADIUS %f # dunno\n", 0.1f);
	mf.printf("\n");
	mf.printf("# Insertion offset\n");
	mf.printf("INSERT OFFSET 0 0 %f #dunno\n", 0.1f);
	mf.printf("\n");
	mf.printf("# Number of Geometry Sets\n");
	mf.printf("GEOSETS 1\n");
	mf.printf("\n");
	mf.printf("# Geometry Set definition\n");
	mf.printf("GEOSET 0\n");
	mf.printf("\n");
	mf.printf("# Number of Meshes\n");
	mf.printf("MESHES %d\n", numNodes);
	mf.printf("\n");
	for (int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++)
	{
		MODELNODE* node = &pNodes[nodeIndex];

		mf.printf("\n");
		mf.printf("# Mesh definition\n");
		mf.printf("MESH %d\n", nodeIndex);
		mf.printf("\n");
		mf.printf("NAME %s\n", struct_str(node->hdr).c_str());
		mf.printf("\n");
		mf.printf("RADIUS %f # dunno\n", 0.1f);
		mf.printf("\n");
		mf.printf("GEOMETRYMODE %d\n", node->hdr.geoMode);
		mf.printf("LIGHTINGMODE %d\n", node->hdr.lightMode);
		mf.printf("TEXTUREMODE %d\n", node->hdr.texMode);
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("VERTICES %d\n", node->hdr.numVerts);
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:         i: \n");
		for (int vi = 0; vi < node->hdr.numVerts; vi++)
		{
			mf.printf("%d: %f %f %f %f\n", vi, node->pVerts[vi * 3 + 0] * worldScale, node->pVerts[vi * 3 + 1] * worldScale, node->pVerts[vi * 3 + 2] * worldScale, 0.0f);
		}
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("TEXTURE VERTICES %d\n", node->hdr.numTexVerts);
		mf.printf("\n");
		for (int vi = 0; vi < node->hdr.numTexVerts; vi++)
		{
			mf.printf("%d: %f %f\n", vi, node->pTexVerts[vi * 2 + 0], node->pTexVerts[vi * 2 + 1]);
		}
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("VERTEX NORMALS\n");
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:\n");
		for (int vi = 0; vi < node->hdr.numVerts; vi++)
		{
			mf.printf("%d: %f %f %f\n", vi, node->pVertNorms[vi * 3 + 0], node->pVertNorms[vi * 3 + 1], node->pVertNorms[vi * 3 + 2]);
		}
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("FACES %d\n", node->hdr.numFaces);
		mf.printf("\n");
		mf.printf("#  num:  material:   type:  geo:  light:   tex:  extralight:  verts:\n");
		for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
		{
			MODELFACE* face = &node->pFaces[faceIndex];

			mf.printf("%d: %d 0x%X %d %d %d %f %d", faceIndex, face->hdr.matIndex, (unsigned)face->hdr.flags, face->hdr.geoMode, face->hdr.lightMode, face->hdr.texMode, 0.0f, face->hdr.numVerts);
			for (int vi = 0; vi < face->hdr.numVerts; vi++)
			{
				mf.printf(" %d, %d", face->pnFaceIndices[vi], face->pnFaceIndices[face->hdr.numVerts + vi]);
			}
			mf.printf("\n");
		}
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("FACE NORMALS\n");
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:\n");
		for (int faceIndex = 0; faceIndex < node->hdr.numFaces; faceIndex++)
		{
			MODELFACE* face = &node->pFaces[faceIndex];

			mf.printf("%d: %f %f %f\n", faceIndex, face->hdr.nx, face->hdr.ny, face->hdr.nz);
		}
	}
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HIERARCHYDEF\n");
	mf.printf("\n");
	mf.printf("# Hierarchy node list\n");
	mf.printf("HIERARCHY NODES %d\n", numHierarchyNodes);
	mf.printf("\n");
	mf.printf("#  num:   flags:   type:    mesh:  parent:  child:  sibling:  numChildren:        x:         y:         z:     pitch:       yaw:      roll:    pivotx:    pivoty:    pivotz:  hnodename:\n");
	for (int hnodeIndex = 0; hnodeIndex < numHierarchyNodes; hnodeIndex++)
	{
		MODELHIERARCHYNODEHEADER* hnode = &pHNodes[hnodeIndex];

		mf.printf("%d: 0x%X 0x%X %d %d %d %d %d %f %f %f %f %f %f %f %f %f %s\n", hnodeIndex,
			0x0, 0x1, hnode->meshIndex,
			hnode->parentIndex, hnode->firstChildIndex, hnode->siblingIndex, hnode->numChildren,
			hnode->x * worldScale, hnode->y * worldScale, hnode->z * worldScale,
			hnode->pitch, hnode->yaw, hnode->roll,
			hnode->px * worldScale, hnode->py * worldScale, hnode->pz * worldScale,
			struct_str(*hnode).c_str());
	}
	mf.printf("\n");
	if (!mf.close(&err))
		log::error("%s\n", err.c_str());
}

void legacy_loadkey(LegacyReader& f, const std::string& szdump, const std::string& baf, float worldScale, const Options& opts)
{
	KEYHEADER keyhdr;
	memset(&keyhdr, 0, sizeof keyhdr);
	KEYHEADER* key = &keyhdr;
	f.read(key, sizeof(KEYHEADER));
	const std::string keyName = struct_str(*key);

	std::string keyfile;
	if (!out_path(szdump, keyName, baf, keyfile))
		throw std::runtime_error("unsafe key name");
	std::string keyfile2 = keyfile + ".txt";
	TextWriter mf;
	if (opts.debug_dumps)
		mf.open(keyfile2);
	else
		mf.discard();


	mf.printf("dunno1: %d\n", key->dunno1);
	mf.printf("dunno2: %d\n", key->dunno2);
	mf.printf("dunno3: %d\n", key->dunno3);
	mf.printf("type: %d\n", key->type);
	mf.printf("fps: %f\n", key->fps);
	mf.printf("numFrames: %d\n", key->numFrames);
	mf.printf("numNodes: %d\n", key->numNodes);

	for (int x = 0; x < 18; x++)
		mf.printf("bleh[%d]=%d\t%f\n", x, key->bleh[x], as_float(key->bleh[x]));


	std::vector<KEYNODE> pNodes;
	if (key->numNodes > 0)//!! could be dunno3 or dunno6 !!
	{
		f.check_count(key->numNodes, sizeof(KEYNODEHEADER), "key node");
		pNodes.resize((size_t)key->numNodes);

		for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
		{
			KEYNODEHEADER* node = &pNodes[nodeIndex].hdr;

			memset(node, 0, sizeof(KEYNODEHEADER));
			f.read(node, sizeof(KEYNODEHEADER));

			mf.printf("node=%s\t%d\t%d\n", struct_str(*node).c_str(), node->someIndex, node->numFrames);
			//fprintf(mf, "dunno1=%d\t%f\n", node->dunno1, *(float*)&node->dunno1);
			//fprintf(mf, "dunno2=%d\t%f\n", node->dunno2, *(float*)&node->dunno2);
			//fprintf(mf, "dunno3=%d\t%f\n", node->dunno3, *(float*)&node->dunno3);
		}


		for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
		{
			KEYNODE* node = &pNodes[nodeIndex];

			f.check_count(node->hdr.numFrames, sizeof(KEYFRAME), "key frame");
			node->pFrames.resize((size_t)node->hdr.numFrames);
			for (int frameIndex = 0; frameIndex < node->hdr.numFrames; frameIndex++)
			{
				KEYFRAME* frame = &node->pFrames[frameIndex];

				memset(frame, 0, sizeof(KEYFRAME));
				f.read(frame, sizeof(KEYFRAME));

				mf.printf("frame[%d]: %f\t%d\tpos=%f/%f/%f\tpyr=%f/%f/%f\tdeltapos=%f/%f/%f\tdeltapyr=%f/%f/%f\n", frameIndex, frame->frame, frame->flags,
					frame->x, frame->y, frame->z,
					frame->pitch, frame->yaw, frame->roll,
					frame->dx, frame->dy, frame->dz,
					frame->dpitch, frame->dyaw, frame->droll
				);

			}
		}
	}


	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- TAIL 64bytes ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	{
		// See legacy_loadmodel(): zeros instead of uninitialised memory when the read hits end of file.
		int32_t tail[16] = {0};
		f.read(tail, 4 * 16);

		for (int ii = 0; ii < 16; ii++)
		{
			mf.printf("tail[%d]=%d\t%f\n", ii, tail[ii], as_float(tail[ii]));
		}
	}

	/*for (int x = 0; x < 1000; x++)
	{
		int v;
		fread(&v, 4, 1, f);
		fprintf(mf, "[%d]=%d\t%f\n", x, v, *(float*)&v);
	}*/


	std::string err;
	if (!mf.close(&err))
		log::error("%s\n", err.c_str());







	if (!mf.open(keyfile, &err))
	{
		log::error("%s\n", err.c_str());
		return;
	}

	mf.printf("# KEYFRAME '%s' created with TPMExtractor\n", keyName.c_str());
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HEADER\n");
	mf.printf("\n");
	mf.printf("FLAGS 0x0\n");
	mf.printf("TYPE 0x%04X\n", (unsigned)key->type);
	mf.printf("FRAMES %d\n", key->numFrames);
	mf.printf("FPS %f\n", key->fps);
	mf.printf("JOINTS %d\n", key->numNodes);
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: KEYFRAME NODES\n");
	mf.printf("\n");
	mf.printf("NODES %d\n", key->numNodes);
	mf.printf("\n");
	for (int nodeIndex = 0; nodeIndex < key->numNodes; nodeIndex++)
	{
		KEYNODE* node = &pNodes[nodeIndex];

		mf.printf("NODE %d\n", nodeIndex);
		mf.printf("MESH NAME %s\n", struct_str(node->hdr).c_str());
		mf.printf("ENTRIES %d\n", node->hdr.numFrames);
		mf.printf("\n");
		mf.printf("# num:   frame:   flags:           x:           y:           z:           p:           y:           r:\n");
		mf.printf("#                                 dx:          dy:          dz:          dp:          dy:          dr:\n");

		for (int x = 0; x < node->hdr.numFrames; x++)
		{
			KEYFRAME* frame = &node->pFrames[x];

			mf.printf("%d:\t%d\t0x%04X\t%f\t%f\t%f\t%f\t%f\t%f\n", x, float_to_int(frame->frame), (unsigned)frame->flags, frame->x * worldScale, frame->y * worldScale, frame->z * worldScale, frame->pitch, frame->yaw, frame->roll);
			mf.printf("\t\t\t%f\t%f\t%f\t%f\t%f\t%f\n", frame->dx * worldScale, frame->dy * worldScale, frame->dz * worldScale, frame->dpitch, frame->dyaw, frame->droll);
		}
		mf.printf("\n");
	}


	if (!mf.close(&err))
		log::error("%s\n", err.c_str());
}

bool legacy_dumpbaf(const std::string& szfile, const std::vector<uint8_t>& data, const std::string& szdump,
                    const Options& opts, const Palette* fallback_palette)
{
	LegacyReader f(data, szfile);

	bool ok = true;

	int32_t header1 = 0, header2 = 0;
	f.read(&header1, 4);
	f.read(&header2, 4);

	char name[32] = {0};
	f.read(name, 32);

	char sourcename[128] = {0};
	f.read(sourcename, 128);

	char dunno[28] = {0};
	f.read(dunno, 28);

	int32_t numMats = 0;
	f.read(&numMats, 4);

	int32_t numKeys = 0;
	f.read(&numKeys, 4);

	int32_t numModels = 0;
	f.read(&numModels, 4);

	// Really u16 has_colormap + two flag bytes; the original's mask keeps has_colormap.
	numModels &= (0x1000000 - 1);


	//char dunno2[80];
	//fread(dunno2, 1, 80, f);
	int32_t dunno2[7] = {0};
	f.read(dunno2, 4 * 7);

	float dunno3[3] = {0, 0, 0};
	f.read(dunno3, 4 * 3);

	int32_t dunno4[10] = {0};
	f.read(dunno4, 4 * 10);

	f.check_count(numMats, sizeof(tMAT_Header), "material");

	// load CMP
	char cmpNameBuf[32] = {0};
	f.read(cmpNameBuf, 32);
	const std::string cmpName = fixed_str(cmpNameBuf, sizeof cmpNameBuf);


	Palette cmp = Palette::grayscale();

	// if "colormap"  file ends with  .mat   then this baf does not have a colormap..  create a fake grayscale one.
	// theres probably something in baf header  (or maybe  numModels is actually numCmps)   but whatever we will haxx it
	bool nocolormap = false;
	if (!cmpName.empty() && cmpName[cmpName.size() - 1] == 't')
	{
		if (fallback_palette)
			cmp = *fallback_palette;

		nocolormap = true;
	}
	else
	{
		// generate valid CMP file

		// skip first 16 bytes?
		char cmpdumb[16];
		f.read(cmpdumb, 16);

		f.read(cmp.c, sizeof cmp.c);


		// skip last 28 bytes?
		char cmpdumb2[28];
		f.read(cmpdumb2, 28);

		// The original's CCMP::saveColormap: version 20, zero light table, no transparency table.
		std::string cmpfile, why;
		if (out_path(szdump, cmpName, szfile, cmpfile))
		{
			if (!jk::write_cmp(cmpfile, jk::legacy_cmp(cmp), &why))
			{
				log::error("%s\n", why.c_str());
				ok = false;
			}
		}
		else
			ok = false;
	}


	std::vector<std::string> pszMatNames;

	// now time for mats!
	for (int matIndex = 0; matIndex < numMats; matIndex++)
	{
		std::string matName;

		if (nocolormap && matIndex == 0)
			matName = cmpName;// if there was no colormap, then we already read the mat name
		else
		{
			char matNameBuf[32] = {0};
			f.read(matNameBuf, 32);
			matName = fixed_str(matNameBuf, sizeof matNameBuf);
		}


		pszMatNames.push_back(matName);


		// raw copy of the MAT record as stored in the BAF (written once the record is read)
		std::vector<uint8_t> raw;
		auto copy = [&raw](const void* p, size_t n) {
			raw.insert(raw.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
		};

		tMAT_Header hdr;
		memset(&hdr, 0, sizeof hdr);
		f.read(&hdr, sizeof(tMAT_Header));
		copy(&hdr, sizeof(tMAT_Header));

		if (hdr.numMaterials > 0)
			f.check_count(hdr.numMaterials, sizeof(tMAT_MaterialHeader), "cel");
		for (int matI = 0; matI < hdr.numMaterials; matI++)
		{
			tMAT_MaterialHeader mathdr;
			memset(&mathdr, 0, sizeof mathdr);
			f.read(&mathdr, sizeof(tMAT_MaterialHeader));
			copy(&mathdr, sizeof(tMAT_MaterialHeader));

			if (mathdr.type == 8)
			{
				tMAT_TextureHeader texhdr;
				memset(&texhdr, 0, sizeof texhdr);
				f.read(&texhdr, sizeof(tMAT_TextureHeader));
				copy(&texhdr, sizeof(tMAT_TextureHeader));
			}
		}

		std::vector<mipimage> exportmips;

		if (hdr.numTextures > 0)
			f.check_count(hdr.numTextures, sizeof(tMAT_TextureData), "texture");
		for (int texI = 0; texI < hdr.numTextures; texI++)
		{
			const size_t texAt = f.pos();
			tMAT_TextureData texdat;
			memset(&texdat, 0, sizeof texdat);
			f.read(&texdat, sizeof(tMAT_TextureData));
			copy(&texdat, sizeof(tMAT_TextureData));

			int64_t mipSize = 0;
			for (int mipI = 0; mipI < texdat.numMipmaps; mipI++)
			{
				// checked before the first level is read (the original computed it unchecked)
				if (mipI == 0)
					mipSize = legacy_mip_bytes(f, texAt, texdat, hdr.colorBits);
				// After the first level, an empty level reads and writes nothing: stop.
				if (mipI > 0 && mipSize == 0)
					break;
				f.check_count(mipSize, 1, "mip byte");
				std::vector<unsigned char> mip((size_t)mipSize);

				if (!mip.empty())
					f.read(mip.data(), mip.size());
				copy(mip.data(), mip.size());


				if (mipI == 0)
				{
					// save this mip for later
					mipimage mi;
					mi.width = texdat.width;
					mi.height = texdat.height;
					mi.trans = texdat.transparent != 0;
					mi.buf = std::move(mip);

					exportmips.push_back(std::move(mi));
				}
				// else: discard this submip


				mipSize /= 4;
			}
		}

		std::string rawfile;
		if (out_path(szdump, matName, szfile, rawfile))
			ok = write_bytes(rawfile, raw) && ok;
		else
			ok = false;

		Palette pal = cmp;

		// NEW TYPE;   seems to specify that *.mat  includes its own palette at the end
		if (hdr.type == 3)
		{
			pal = Palette();
			f.read(pal.c, sizeof pal.c);
		}

		// only the first texture's first mip is exported
		if (!exportmips.empty())
		{
			const mipimage& mi = exportmips.front();
			ok = write_preview_and_mat16(szdump, szfile, matName, mi.width, mi.height, mi.trans, mi.buf, pal, opts,
				BmpRows::Unpadded) && ok;
		}
	}


	float worldScale = opts.scale;



	// time for models!
	if (numModels > 0)
		f.check_count(numModels, kLegacyModelBytes, "model");
	for (int modelIndex = 0; modelIndex < numModels; modelIndex++)
	{
		legacy_loadmodel(f, szdump, szfile, numMats, pszMatNames, worldScale, opts);
	}


	// time for keys!
	if (numKeys > 0)
		f.check_count(numKeys, sizeof(KEYHEADER), "key");
	for (int keyIndex = 0; keyIndex < numKeys; keyIndex++)
	{
		legacy_loadkey(f, szdump, szfile, worldScale, opts);
	}


	if (opts.debug_dumps)
	{
		std::string funame;
		if (out_path(szdump, "REMAIN.HEX", szfile, funame))
			ok = write_bytes(funame, std::vector<uint8_t>(f.here(), f.here() + f.remaining())) && ok;
		else
			ok = false;
	}

	if (f.short_reads())
		log::verbose("%s: %u read(s) hit end of file\n", szfile.c_str(), f.short_reads());

	return ok;
}

// =========================================================================================
// Default mode: the real layout
// =========================================================================================

// Record sizes.
const size_t kHeaderSize = 0x120;
const size_t kColormapBlockSize = 0x34C;
const size_t kNameSize = 32;
const size_t kMatHeaderSize = 0x4C;
const size_t kCelSize = 0x18;
const size_t kCelExtSize = 0x10;
const size_t kTextureHeaderSize = 0x18;
const size_t kPaletteSize = 768;
const size_t kModelHeaderSize = 0x84;
const size_t kMeshHeaderSize = 0x70;
const size_t kFaceSize = 0x4C;
const size_t kNodeSize = 0xB4;
const size_t kKeyWrapperSize = 0x40;
const size_t kKeyHeaderSize = 0x84;
const size_t kJointSize = 0x2C;
const size_t kEntrySize = 0x38;
const size_t kSoundRecordSize = 0x60;
const uint32_t kBafMagic = 0x00010001;
const uint32_t kMatVersion = 0x32;

// Sanity limits for counts read from a file, far above the retail disc (at most 30-odd
// materials, 11 cels, 46 meshes, 512 vertices per mesh, 48 nodes and 440 frames per actor).
// Every count is also checked against the bytes left in the file.
const uint32_t kMaxMaterials = 4096;
const uint32_t kMaxCelsOrTextures = 1024;
const uint32_t kMaxMips = 16;
const int32_t kMaxTextureSide = 1 << 14;
const int32_t kMaxBitsPerPixel = 32;
const uint32_t kMaxMeshes = 4096;
const uint32_t kMaxMeshElements = 1u << 20;  // vertices, UVs or faces in one mesh
const uint32_t kMaxFaceVertices = 1024;
const uint32_t kMaxNodes = 4096;
const uint32_t kMaxKeys = 1u << 16;
const uint32_t kMaxJoints = 4096;
const uint32_t kMaxEntries = 1u << 20;
const uint32_t kMaxSoundRecords = 1u << 16;   // per key
const uint64_t kMaxBafFileSize = 256ull << 20;

// The draw scale (+0xAC) accepted as such; anything else (garbage, NaN, 0) is replaced by 1.0
// with a warning. The retail disc uses 0.5..3.0.
const float kMinDrawScale = 1.0f / 64.0f;
const float kMaxDrawScale = 64.0f;
// Default mode applies the draw scale in hundredths: the retail values are 0.5..3.0 in steps of
// 0.05, and about half of the BAFs store theirs about 1e-5 too high (1.00001 for 1.0), which
// would otherwise make keys and models that BAFs share differ in their last digits.
const int kDrawScaleSteps = 100;
// A 3DO or KEY written at a draw scale other than 1 gets "_s<hundredths, 3 digits>" before its
// extension ("_s110" for 1.1): JK keeps one model and one key per name, and a key or model that
// other BAFs share unscaled (or at another scale) must not get the same name.
const char* const kDrawScaleSuffix = "_s";

// JK values written in default mode.
const int kJkTextureModePerspective = 3;  // TEXTUREMODE: TPM stores 0, which JK draws affine
const uint32_t kJkFaceFlagMask = 0x3;     // the face flags retail JK honours: double-sided,
                                          // translucent (TPM's 0x8 and 0x10 markers do nothing)
const double kJkRadiusMargin = 0.1;       // JK's radius: max distance * 0.1 + max distance

// 8-bit MATs (retail JK).
const char* const kMat8Suffix = "8.mat";          // "<MAT name minus .mat>8.mat"
const int kMat8MipLevels = jk::kMat8MaxMips;      // generate up to JK's 4 mip levels ...
const int kMat8MinMipSide = 8;                    // ... while both sides stay >= 8 texels
const jk::ColorMetric kMat8Metric = jk::ColorMetric::Redmean;  // as build_cmp's light tables
// An 8-bit MAT whose texels moved further than this on average (RGB distance, 0..441) from their
// colours in the source palette is reported: a material with its own palette (or every material
// with --palette) can only use the colours of the one palette JK draws with.
const double kMat8RemapWarnError = 48.0;

// Model RADIUS covering the keys (BafExtras::animated_radius): frames sampled per key, and a
// budget of vertex transforms per BAF that bounds the work on malformed files.
const uint32_t kAnimatedRadiusMaxFramesPerKey = 4096;
const uint64_t kAnimatedRadiusBudget = 400000000ull;

// InsertOffsetMode::Auto re-roots a model by node 0's z rounded to 1/kAutoRerootSteps of a model
// unit: BAFs that share keys store node 0 heights that differ in their last bits, which would
// otherwise make their re-rooted keys differ in the last printed digit under the same name.
const double kAutoRerootSteps = 1000.0;

struct Vec3f
{
	float x = 0, y = 0, z = 0;
};

Vec3f read_vec3(BinReader& r)
{
	Vec3f v;
	v.x = r.f32();
	v.y = r.f32();
	v.z = r.f32();
	return v;
}

Vec3f vec3_at(const uint8_t* p) { Vec3f v; v.x = ld_f32(p); v.y = ld_f32(p + 4); v.z = ld_f32(p + 8); return v; }

struct BafHeader
{
	uint32_t magic = 0, header_size = 0, alloc_size = 0;
	float draw_scale = 1.0f;
	uint32_t model_size = 0, num_mats = 0, num_keys = 0;
	uint16_t has_colormap = 0;
	uint32_t mats_size = 0, key_size = 0, off_model = 0, off_keys = 0;
};

struct BafTexture
{
	int32_t width = 0, height = 0;
	int32_t transparent = 0;                  // any non-zero value (some are garbage): index 0 is see-through
	std::vector<std::vector<uint8_t>> mips;   // level k: (width >> k) * (height >> k) * bpp / 8 bytes
	uint64_t offset = 0;
};

struct BafMaterial
{
	std::string name;
	uint64_t offset = 0;                      // file offset of the name
	int32_t type = 0;                         // bit 1: textured; bit 0: own palette follows
	int32_t bpp = 0;
	uint32_t num_cels = 0;
	std::vector<BafTexture> textures;
	bool own_palette = false;
	Palette palette;
	const uint8_t* stream = nullptr;          // the MAT as stored after its name, with the palette
	size_t stream_size = 0;                   // of an own-palette material: the raw copy
};

struct BafFace
{
	int32_t material = 0;
	uint32_t flags = 0;
	int32_t geo = 0, light = 0;
	Vec3f normal;
	std::vector<int32_t> pos, uv;             // uv is empty for untextured materials
	const uint8_t* raw = nullptr;             // the 0x4C-byte record
	uint64_t offset = 0;
};

struct BafMesh
{
	std::string name;
	int32_t geo = 0, light = 0;
	std::vector<Vec3f> verts, norms;
	std::vector<float> uvs;                   // u, v pairs
	std::vector<BafFace> faces;
	const uint8_t* raw = nullptr;             // the 0x70-byte header
	const uint8_t* vi_raw = nullptr;          // vertices_i[nV], vertices_unk[nV]
	uint64_t offset = 0;
};

struct BafNode
{
	std::string name;
	uint32_t type = 0;                        // body-part bits matched by the KEY TYPE masks
	int32_t mesh = -1, parent = -1, num_children = 0, child = -1, sibling = -1;
	Vec3f pivot, pos, rot;
	const uint8_t* raw = nullptr;             // the 0xB4-byte record
	uint64_t offset = 0;
};

struct BafModel
{
	std::string name;
	float radius = 0;                         // +0x5C, model units
	std::vector<BafMesh> meshes;
	std::vector<BafNode> nodes;
	const uint8_t* raw = nullptr;             // the 0x84-byte header
	uint64_t offset = 0;
	size_t start = 0, end = 0;                // buffer positions
};

struct BafEntry
{
	float frame = 0;
	uint32_t flags = 0;                       // 0x1: position delta valid, 0x2: rotation delta valid
	Vec3f pos, rot, dpos, drot;
};

struct BafJoint
{
	std::string name;
	int32_t node = 0;
	std::vector<BafEntry> entries;
	const uint8_t* raw = nullptr;             // the 0x2C-byte record
};

struct BafKey
{
	std::string name;
	uint32_t type = 0, num_frames = 0, num_joints = 0;
	float fps = 0;
	uint32_t num_extra = 0;
	std::vector<BafJoint> joints;
	const uint8_t* raw = nullptr;             // the 0x84-byte rdKeyframe
	uint64_t offset = 0;                      // file offset of the wrapper
	size_t end = 0;                           // buffer position after the entries
};

struct Baf
{
	std::string file;
	const std::vector<uint8_t>* data = nullptr;
	BafHeader hdr;
	bool has_colormap = false;
	bool cmp_parsed = false;
	std::string cmp_name;
	Palette cmp_palette;
	std::vector<BafMaterial> mats;
	bool model_parsed = false;                // the model blob was read
	bool model_ok = false;                    // ... and its indices and hierarchy are valid
	BafModel model;
	std::vector<BafKey> keys;
	bool complete = false;                    // read to the end of the sound records
};

// ------------------------------------------------------------------------------- parse

std::string warn_name(const std::string& s) { return "\"" + s + "\""; }

// A name field without a NUL is used whole (32 bytes); JK names are at most 31 characters.
std::string read_name(BinReader& r, const char* what)
{
	const uint64_t at = r.file_offset();
	const uint8_t* p = r.span(kNameSize);
	if (!memchr(p, 0, kNameSize))
		log::warn_at(r.name(), at, "%s name has no terminating NUL in its 32 bytes\n", what);
	return fixed_str(p, kNameSize);
}

void parse_header(BinReader& r, Baf& b)
{
	BafHeader& h = b.hdr;
	if (r.size() < kHeaderSize)
		r.fail(format("file is %llu bytes, shorter than the 0x120-byte BAF header", (unsigned long long)r.size()));
	h.magic = r.u32();
	h.header_size = r.u32();
	r.skip(32 + 128);                    // name, editor source folder
	h.alloc_size = r.u32();              // 0xA8
	h.draw_scale = r.f32();              // 0xAC
	r.skip(4 + 12);                      // 0xB0 float, 0xB4 draw offset
	h.model_size = r.u32();              // 0xC0
	h.num_mats = r.u32();                // 0xC4
	h.num_keys = r.u32();                // 0xC8
	h.has_colormap = r.u16();            // 0xCC
	r.skip(2);                           // 0xCE last-material flag, 0xCF object flag
	h.mats_size = r.u32();               // 0xD0
	h.key_size = r.u32();                // 0xD4
	r.skip(8);                           // 0xD8 colormap, 0xDC material table payload offsets
	h.off_model = r.u32();               // 0xE0
	h.off_keys = r.u32();                // 0xE4
	r.seek(kHeaderSize);

	if (h.magic != kBafMagic)
		log::warn_at(b.file, 0x00, "magic is 0x%08X, expected 0x%08X\n", h.magic, kBafMagic);
	if (h.header_size != kHeaderSize)
		log::warn_at(b.file, 0x04, "header size field is 0x%X, the header is 0x%X bytes\n", h.header_size, (unsigned)kHeaderSize);
	if (!(h.draw_scale >= kMinDrawScale && h.draw_scale <= kMaxDrawScale))
	{
		log::warn_at(b.file, 0xAC, "draw scale %f is outside %f..%f; using 1.0\n", (double)h.draw_scale,
			(double)kMinDrawScale, (double)kMaxDrawScale);
		h.draw_scale = 1.0f;
	}
	if (h.num_mats > kMaxMaterials || (uint64_t)h.num_mats * (kNameSize + kMatHeaderSize) > r.remaining())
		r.fail_at(0xC4, format("material count %u is impossible for the %llu bytes after the header", h.num_mats,
			(unsigned long long)r.remaining()));
	if (h.num_keys > kMaxKeys || (uint64_t)h.num_keys * (kKeyWrapperSize + kKeyHeaderSize) > r.remaining())
		r.fail_at(0xC8, format("key count %u is impossible for the %llu bytes after the header", h.num_keys,
			(unsigned long long)r.remaining()));
	b.has_colormap = h.has_colormap != 0;
}

void parse_colormap(BinReader& r, Baf& b)
{
	b.cmp_name = read_name(r, "colormap");
	r.skip(4 + 12);                      // flags, tint
	r.bytes(b.cmp_palette.c, kPaletteSize);
	r.skip(7 * 4);                       // runtime pointers
}

void parse_material(BinReader& r, BafMaterial& m)
{
	m.offset = r.file_offset();
	m.name = read_name(r, "material");
	const size_t start = r.pos();
	const uint8_t* stream = r.span(0);
	if (!r.peek_tag("MAT "))
		r.fail(format("material %s has no \"MAT \" header", warn_name(m.name).c_str()));
	r.skip(4);
	const uint32_t version = r.u32();
	if (version != kMatVersion)
		log::warn_at(r.name(), r.file_offset() - 4, "material %s: version 0x%X, expected 0x%X\n", warn_name(m.name).c_str(),
			version, kMatVersion);
	m.type = r.i32();
	m.num_cels = r.count("cel", kMaxCelsOrTextures, kCelSize);
	const uint32_t num_textures = r.count("texture", kMaxCelsOrTextures, kTextureHeaderSize);
	r.skip(4);                           // colour mode
	const size_t bpp_at = r.pos();
	m.bpp = r.i32();
	r.skip(12 * 4);                      // the rest of the colour format
	if (m.bpp < 1 || m.bpp > kMaxBitsPerPixel)
		r.fail_at(bpp_at, format("material %s: %d bits per pixel", warn_name(m.name).c_str(), m.bpp));
	for (uint32_t c = 0; c < m.num_cels; c++)
	{
		const int32_t cel_type = r.i32();
		r.skip(kCelSize - 4);
		if (cel_type & 8)
			r.skip(kCelExtSize);
	}
	for (uint32_t t = 0; t < num_textures; t++)
	{
		BafTexture tex;
		tex.offset = r.file_offset();
		tex.width = r.i32();
		tex.height = r.i32();
		tex.transparent = r.i32();
		r.skip(8);
		const uint32_t mips = r.count("mip", kMaxMips, 0);
		if (tex.width < 1 || tex.height < 1 || tex.width > kMaxTextureSide || tex.height > kMaxTextureSide)
			r.fail_at(r.pos() - kTextureHeaderSize, format("material %s: texture %u is %dx%d",
				warn_name(m.name).c_str(), t, tex.width, tex.height));
		for (uint32_t k = 0; k < mips; k++)
		{
			const int32_t w = tex.width >> k, h = tex.height >> k;
			if (w < 1 || h < 1)
				r.fail(format("material %s: texture %u (%dx%d) cannot have %u mips", warn_name(m.name).c_str(), t,
					tex.width, tex.height, mips));
			tex.mips.push_back(r.bytes((size_t)((uint64_t)w * (uint64_t)h * (uint64_t)m.bpp / 8)));
		}
		m.textures.push_back(std::move(tex));
	}
	if (m.type & 1)
	{
		r.bytes(m.palette.c, kPaletteSize);
		m.own_palette = true;
	}
	// The raw copy keeps the palette that the type announces, so it loads in JK (whose loader
	// reads the 768 bytes after the last mip); the original's copy left it out.
	m.stream = stream;
	m.stream_size = r.pos() - start;
}

void parse_model(BinReader& r, const std::vector<BafMaterial>& mats, BafModel& m)
{
	m.start = r.pos();
	m.offset = r.file_offset();
	m.raw = r.span(kModelHeaderSize);
	m.name = fixed_str(m.raw, kNameSize);
	if (!memchr(m.raw, 0, kNameSize))
		log::warn_at(r.name(), m.offset, "model name has no terminating NUL in its 32 bytes\n");
	const uint32_t num_meshes = ld_u32(m.raw + 0x24);
	const uint32_t num_geosets = ld_u32(m.raw + 0x44);
	const uint32_t num_materials = ld_u32(m.raw + 0x4C);
	const uint32_t num_nodes = ld_u32(m.raw + 0x54);
	m.radius = ld_f32(m.raw + 0x5C);
	if (num_meshes > kMaxMeshes || (uint64_t)num_meshes * kMeshHeaderSize > r.remaining())
		r.fail_at(m.start + 0x24, format("mesh count %u is impossible for the %llu bytes left", num_meshes,
			(unsigned long long)r.remaining()));
	if (num_nodes > kMaxNodes)
		r.fail_at(m.start + 0x54, format("hierarchy node count %u exceeds the limit %u", num_nodes, kMaxNodes));
	if (num_geosets != 1)
		log::warn_at(r.name(), m.offset + 0x44, "the model has %u geosets; only geoset 0 is read\n", num_geosets);
	for (int g = 1; g < 4; g++)
		if (ld_u32(m.raw + 0x24 + 8 * g) != 0)
			log::warn_at(r.name(), m.offset + 0x24 + 8 * g, "geoset %d has %u meshes; only geoset 0 is read\n", g,
				ld_u32(m.raw + 0x24 + 8 * g));
	if (num_materials != mats.size())
		log::warn_at(r.name(), m.offset + 0x4C, "the model counts %u materials, the BAF has %u\n", num_materials,
			(unsigned)mats.size());

	m.meshes.resize(num_meshes);
	for (BafMesh& me : m.meshes)
	{
		me.offset = r.file_offset();
		me.raw = r.span(kMeshHeaderSize);
		me.name = fixed_str(me.raw, kNameSize);
		me.geo = ld_i32(me.raw + 0x24);
		me.light = ld_i32(me.raw + 0x28);
		for (size_t f = 0x48; f <= 0x50; f += 4)
			if (ld_u32(me.raw + f) > kMaxMeshElements)
				r.fail_at(r.pos() - kMeshHeaderSize + f, format("mesh %s: count %u exceeds the limit %u",
					warn_name(me.name).c_str(), ld_u32(me.raw + f), kMaxMeshElements));
	}
	for (BafMesh& me : m.meshes)
	{
		const uint32_t nv = ld_u32(me.raw + 0x48), nuv = ld_u32(me.raw + 0x4C), nf = ld_u32(me.raw + 0x50);
		if ((uint64_t)nv * 32 + (uint64_t)nuv * 8 + (uint64_t)nf * kFaceSize > r.remaining())
			r.fail(format("mesh %s: %u vertices, %u UVs and %u faces need more than the %llu bytes left",
				warn_name(me.name).c_str(), nv, nuv, nf, (unsigned long long)r.remaining()));
		me.verts.resize(nv);
		me.norms.resize(nv);
		for (Vec3f& v : me.verts)
			v = read_vec3(r);
		for (Vec3f& v : me.norms)
			v = read_vec3(r);
		me.vi_raw = r.span((size_t)nv * 8);   // vertices_i (alpha), vertices_unk
		me.uvs.resize((size_t)nuv * 2);
		for (float& u : me.uvs)
			u = r.f32();
		me.faces.resize(nf);
		for (BafFace& fc : me.faces)
		{
			fc.offset = r.file_offset();
			fc.raw = r.span(kFaceSize);
			fc.material = ld_i32(fc.raw + 0x00);
			fc.flags = ld_u32(fc.raw + 0x04);
			fc.geo = ld_i32(fc.raw + 0x08);
			fc.light = ld_i32(fc.raw + 0x0C);
			fc.normal = vec3_at(fc.raw + 0x38);
		}
		for (BafFace& fc : me.faces)
		{
			const uint32_t n = ld_u32(fc.raw + 0x14);
			if (n > kMaxFaceVertices)
				r.fail_at(r.pos(), format("mesh %s: a face has %u vertices (limit %u)", warn_name(me.name).c_str(), n,
					kMaxFaceVertices));
			fc.pos.resize(n);
			for (int32_t& i : fc.pos)
				i = r.i32();
			// UV indices follow only for textured materials (as the game reads them).
			const bool textured = fc.material >= 0 && (size_t)fc.material < mats.size() && (mats[fc.material].type & 2);
			if (textured)
			{
				fc.uv.resize(n);
				for (int32_t& i : fc.uv)
					i = r.i32();
			}
		}
	}
	if ((uint64_t)num_nodes * kNodeSize > r.remaining())
		r.fail(format("%u hierarchy nodes need %llu bytes, %llu left", num_nodes,
			(unsigned long long)((uint64_t)num_nodes * kNodeSize), (unsigned long long)r.remaining()));
	m.nodes.resize(num_nodes);
	for (BafNode& nd : m.nodes)
	{
		nd.offset = r.file_offset();
		nd.raw = r.span(kNodeSize);
		nd.name = fixed_str(nd.raw, kNameSize);
		nd.type = ld_u32(nd.raw + 0x48);
		nd.mesh = ld_i32(nd.raw + 0x4C);
		nd.parent = ld_i32(nd.raw + 0x50);
		nd.num_children = ld_i32(nd.raw + 0x54);
		nd.child = ld_i32(nd.raw + 0x58);
		nd.sibling = ld_i32(nd.raw + 0x5C);
		nd.pivot = vec3_at(nd.raw + 0x60);
		nd.pos = vec3_at(nd.raw + 0x6C);
		nd.rot = vec3_at(nd.raw + 0x78);
	}
	m.end = r.pos();
}

void parse_key(BinReader& r, BafKey& k)
{
	k.offset = r.file_offset();
	const size_t start = r.pos();
	const uint8_t* wrap = r.span(kKeyWrapperSize);
	const uint32_t block_size = ld_u32(wrap);
	k.num_extra = ld_u32(wrap + 0x1C);
	if (k.num_extra > kMaxSoundRecords)
		r.fail_at(start + 0x1C, format("sound-event count %u exceeds the limit %u", k.num_extra, kMaxSoundRecords));
	k.raw = r.span(kKeyHeaderSize);
	k.name = fixed_str(k.raw, kNameSize);
	if (!memchr(k.raw, 0, kNameSize))
		log::warn_at(r.name(), k.offset + kKeyWrapperSize, "key name has no terminating NUL in its 32 bytes\n");
	k.num_joints = ld_u32(k.raw + 0x28);
	k.type = ld_u32(k.raw + 0x2C);
	k.fps = ld_f32(k.raw + 0x30);
	k.num_frames = ld_u32(k.raw + 0x34);
	const uint32_t joints = ld_u32(k.raw + 0x38);
	if (joints > kMaxJoints || (uint64_t)joints * kJointSize > r.remaining())
		r.fail_at(r.pos() - kKeyHeaderSize + 0x38, format("key %s: joint count %u is impossible for the %llu bytes left",
			warn_name(k.name).c_str(), joints, (unsigned long long)r.remaining()));
	if (k.num_joints != joints)
		log::warn_at(r.name(), k.offset + kKeyWrapperSize + 0x28, "key %s: joint counts %u and %u differ; using %u\n",
			warn_name(k.name).c_str(), k.num_joints, joints, joints);
	k.joints.resize(joints);
	std::vector<uint32_t> counts(joints);
	uint64_t total = 0;
	for (size_t ji = 0; ji < k.joints.size(); ji++)
	{
		BafJoint& j = k.joints[ji];
		j.raw = r.span(kJointSize);
		j.name = fixed_str(j.raw, kNameSize);
		j.node = ld_i32(j.raw + 0x20);
		counts[ji] = ld_u32(j.raw + 0x24);
		if (counts[ji] > kMaxEntries)
			r.fail_at(r.pos() - kJointSize + 0x24, format("key %s: entry count %u exceeds the limit %u",
				warn_name(k.name).c_str(), counts[ji], kMaxEntries));
		total += counts[ji];
	}
	if (total * kEntrySize > r.remaining())
		r.fail(format("key %s: %llu entries need %llu bytes, %llu left", warn_name(k.name).c_str(),
			(unsigned long long)total, (unsigned long long)(total * kEntrySize), (unsigned long long)r.remaining()));
	for (size_t ji = 0; ji < k.joints.size(); ji++)
	{
		BafJoint& j = k.joints[ji];
		j.entries.resize(counts[ji]);
		for (BafEntry& e : j.entries)
		{
			e.frame = r.f32();
			e.flags = r.u32();
			e.pos = read_vec3(r);
			e.rot = read_vec3(r);
			e.dpos = read_vec3(r);
			e.drot = read_vec3(r);
		}
	}
	k.end = r.pos();
	if (block_size != k.end - start)
		log::warn_at(r.name(), k.offset, "key %s: block size field is %u, the key is %llu bytes\n",
			warn_name(k.name).c_str(), block_size, (unsigned long long)(k.end - start));
}

// Indices and hierarchy links of a parsed model; false (after logging) if JK could not load it.
bool validate_model(const Baf& b)
{
	const BafModel& m = b.model;
	bool ok = true;
	unsigned bad_materials = 0;
	for (const BafMesh& me : m.meshes)
	{
		for (const BafFace& fc : me.faces)
		{
			if (fc.material < 0 || (size_t)fc.material >= b.mats.size())
			{
				if (bad_materials++ == 0)
					log::warn_at(b.file, fc.offset, "mesh %s: face material index %d is not one of the %u materials; "
						"written as -1 (no material)\n", warn_name(me.name).c_str(), fc.material, (unsigned)b.mats.size());
			}
			if (fc.pos.size() < 3)
				log::warn_at(b.file, fc.offset, "mesh %s: a face has %u vertices\n", warn_name(me.name).c_str(),
					(unsigned)fc.pos.size());
			for (int32_t i : fc.pos)
				if (i < 0 || (size_t)i >= me.verts.size())
				{
					log::error_at(b.file, fc.offset, "mesh %s: face vertex index %d is outside 0..%d\n",
						warn_name(me.name).c_str(), i, (int)me.verts.size() - 1);
					ok = false;
					break;
				}
			for (int32_t i : fc.uv)
				if (i < 0 || (size_t)i * 2 >= me.uvs.size())
				{
					log::error_at(b.file, fc.offset, "mesh %s: face texture vertex index %d is outside 0..%d\n",
						warn_name(me.name).c_str(), i, (int)(me.uvs.size() / 2) - 1);
					ok = false;
					break;
				}
		}
	}
	if (bad_materials > 1)
		log::warn("%s: %u faces in all have an invalid material index\n", b.file.c_str(), bad_materials);

	const int32_t n = (int32_t)m.nodes.size();
	auto in_range = [n](int32_t i) { return i == -1 || (i >= 0 && i < n); };
	for (int32_t i = 0; i < n; i++)
	{
		const BafNode& nd = m.nodes[(size_t)i];
		const char* what = nullptr;
		if (!(nd.mesh == -1 || (nd.mesh >= 0 && (size_t)nd.mesh < m.meshes.size())))
			what = "mesh index";
		else if (!in_range(nd.parent))
			what = "parent index";
		else if (!in_range(nd.child))
			what = "child index";
		else if (!in_range(nd.sibling))
			what = "sibling index";
		else if (nd.num_children < 0 || nd.num_children > n)
			what = "child count";
		else if (nd.num_children > 0 && nd.child == -1)
			what = "child index (-1 with children)";
		if (what)
		{
			log::error_at(b.file, nd.offset, "hierarchy node %d %s: bad %s\n", i, warn_name(nd.name).c_str(), what);
			ok = false;
		}
	}
	if (!ok || n == 0)
		return ok;
	if (m.nodes[0].parent != -1)
		log::warn_at(b.file, m.nodes[0].offset, "hierarchy node 0 has parent %d\n", m.nodes[0].parent);
	// JK walks the hierarchy from node 0 through the child and sibling links.
	std::vector<char> seen((size_t)n, 0);
	std::vector<int32_t> stack(1, 0);
	seen[0] = 1;
	unsigned bad_parent = 0;
	while (!stack.empty())
	{
		const int32_t i = stack.back();
		stack.pop_back();
		int32_t c = m.nodes[(size_t)i].child;
		for (int32_t k = 0; k < m.nodes[(size_t)i].num_children; k++)
		{
			if (c < 0 || seen[(size_t)c])
			{
				log::error_at(b.file, m.nodes[(size_t)i].offset, "hierarchy node %d: its %d children are not a chain of "
					"distinct nodes\n", i, m.nodes[(size_t)i].num_children);
				return false;
			}
			seen[(size_t)c] = 1;
			if (m.nodes[(size_t)c].parent != i)
				bad_parent++;
			stack.push_back(c);
			c = m.nodes[(size_t)c].sibling;
		}
	}
	const long unreached = (long)std::count(seen.begin(), seen.end(), 0);
	if (unreached || bad_parent)
		log::warn("%s: hierarchy: %ld node(s) not reachable from node 0, %u parent link(s) disagreeing with the "
			"child links\n", b.file.c_str(), unreached, bad_parent);
	return ok;
}

// Reads as much of the BAF as possible into `b`. False after logging if anything could not be
// read; the parts read before the problem are kept (and written by the caller).
bool parse_baf(const std::string& file, const std::vector<uint8_t>& data, Baf& b)
{
	b.file = file;
	b.data = &data;
	BinReader r(data, file);
	const BafHeader& h = b.hdr;
	try
	{
		parse_header(r, b);
		if (b.has_colormap)
		{
			if (r.remaining() < kColormapBlockSize)
				r.fail("the colormap block (0x34C bytes) is cut off");
			parse_colormap(r, b);
			b.cmp_parsed = true;
		}
		const size_t mats_start = r.pos();
		for (uint32_t i = 0; i < h.num_mats; i++)
		{
			BafMaterial m;
			parse_material(r, m);
			b.mats.push_back(std::move(m));
		}
		const uint64_t mats_bytes = r.pos() - mats_start;
		if (h.mats_size != mats_bytes)
			log::warn_at(file, 0xD0, "materials size field is %u, the materials are %llu bytes\n", h.mats_size,
				(unsigned long long)mats_bytes);
		if ((uint64_t)h.off_model != r.pos() - kHeaderSize)
			log::warn_at(file, 0xE0, "model offset field is 0x%X, the model starts at payload offset 0x%llX\n",
				h.off_model, (unsigned long long)(r.pos() - kHeaderSize));

		parse_model(r, b.mats, b.model);
		b.model_parsed = true;
		const uint64_t model_bytes = b.model.end - b.model.start;
		if (h.model_size != model_bytes)
			log::warn_at(file, 0xC0, "model size field is %u, the model is %llu bytes\n", h.model_size,
				(unsigned long long)model_bytes);
		b.model_ok = validate_model(b);

		if ((uint64_t)h.off_keys != r.pos() - kHeaderSize)
			log::warn_at(file, 0xE4, "keys offset field is 0x%X, the keys start at payload offset 0x%llX\n",
				h.off_keys, (unsigned long long)(r.pos() - kHeaderSize));
		const size_t keys_start = r.pos();
		uint64_t sound_records = 0;
		for (uint32_t i = 0; i < h.num_keys; i++)
		{
			BafKey k;
			parse_key(r, k);
			sound_records += k.num_extra;
			b.keys.push_back(std::move(k));
		}
		if (sound_records * kSoundRecordSize > r.remaining())
			r.fail(format("the keys announce %llu sound-event records (%llu bytes); %llu bytes are left",
				(unsigned long long)sound_records, (unsigned long long)(sound_records * kSoundRecordSize),
				(unsigned long long)r.remaining()));
		r.skip((size_t)(sound_records * kSoundRecordSize));
		const uint64_t key_bytes = r.pos() - keys_start;
		if (h.key_size != key_bytes)
			log::warn_at(file, 0xD4, "key section size field is %u, the keys and their sound-event records are %llu "
				"bytes\n", h.key_size, (unsigned long long)key_bytes);
		if (!r.at_end())
			log::warn_at(file, r.file_offset(), "%llu bytes after the last sound-event record are not read\n",
				(unsigned long long)r.remaining());
		const uint64_t alloc = kHeaderSize + (b.has_colormap ? kColormapBlockSize : 0) + 4ull * h.num_mats +
			4ull * h.num_keys + model_bytes + key_bytes;
		if (h.alloc_size != alloc)
			log::warn_at(file, 0xA8, "allocation size field is %u, the layout gives %llu\n", h.alloc_size,
				(unsigned long long)alloc);
		b.complete = true;
	}
	catch (const ParseError& e)
	{
		log::error("%s\n", e.what());
		return false;
	}
	return b.model_ok;
}

// ------------------------------------------------------------------------------- geometry

// sin and cos of an angle in degrees using only + - * / (no library call), so every compiler and
// C library gives the same bits (library sin/cos may differ in the last bit, which could change a
// printed digit): reduction to [-45, 45] degrees (exact for the angles a BAF holds: the
// subtraction below is exact by Sterbenz's lemma), then Taylor series that are exact to double
// precision there. Angles beyond 1e15 degrees (meaningless: an ulp is over 0.1 degree) give NaN.
void sincos_deg(double deg, double& s, double& c)
{
	if (!(std::fabs(deg) < 1e15))
	{
		s = c = std::numeric_limits<double>::quiet_NaN();
		return;
	}
	double r = deg - 360.0 * (double)(int64_t)(deg / 360.0);
	if (r < 0.0)
		r += 360.0;
	const int q = (int)((r + 45.0) / 90.0);  // 0..4
	const double x = (r - 90.0 * q) * (kPi / 180.0);
	const double x2 = x * x;
	const double sn = x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0 +
		x2 * (-1.0 / 39916800.0 + x2 * (1.0 / 6227020800.0 + x2 * (-1.0 / 1307674368000.0 +
		x2 * (1.0 / 355687428096000.0 + x2 * (-1.0 / 121645100408832000.0))))))))));
	const double cs = 1.0 + x2 * (-1.0 / 2.0 + x2 * (1.0 / 24.0 + x2 * (-1.0 / 720.0 + x2 * (1.0 / 40320.0 +
		x2 * (-1.0 / 3628800.0 + x2 * (1.0 / 479001600.0 + x2 * (-1.0 / 87178291200.0 +
		x2 * (1.0 / 20922789888000.0 + x2 * (-1.0 / 6402373705728000.0 + x2 * (1.0 / 2432902008176640000.0))))))))));
	switch (q & 3)
	{
	case 0: s = sn; c = cs; break;
	case 1: s = cs; c = -sn; break;
	case 2: s = -sn; c = -cs; break;
	default: s = -cs; c = sn; break;
	}
}

struct Vec3d
{
	double x = 0, y = 0, z = 0;
};

// A JK rdMatrix34: the columns of m are rvec, lvec and uvec; t is the position.
struct Mat34
{
	double m[3][3];
	double t[3];
};

Mat34 mat_mul(const Mat34& a, const Mat34& b)
{
	Mat34 r;
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
		r.t[i] = a.m[i][0] * b.t[0] + a.m[i][1] * b.t[1] + a.m[i][2] * b.t[2] + a.t[i];
	}
	return r;
}

// JK's rdMatrix_Build34(pyr, pos): pitch about X, yaw about Z, roll about Y, in degrees,
// R = Rz(yaw) * Rx(pitch) * Ry(roll), term for term as the engine writes it.
Mat34 mat_build34(const Vec3d& pyr, const Vec3d& pos)
{
	double sp, cp, sy, cy, sr, cr;
	sincos_deg(pyr.x, sp, cp);
	sincos_deg(pyr.y, sy, cy);
	sincos_deg(pyr.z, sr, cr);
	Mat34 r;
	r.m[0][0] = -(sr * sy) * sp + cr * cy;  // rvec
	r.m[1][0] = (sr * cy) * sp + cr * sy;
	r.m[2][0] = -sr * cp;
	r.m[0][1] = -sy * cp;                   // lvec
	r.m[1][1] = cy * cp;
	r.m[2][1] = sp;
	r.m[0][2] = (cr * sy) * sp + sr * cy;   // uvec
	r.m[1][2] = -sp * (cy * cr) + sy * sr;
	r.m[2][2] = cr * cp;
	r.t[0] = pos.x;
	r.t[1] = pos.y;
	r.t[2] = pos.z;
	return r;
}

Vec3d mat_apply(const Mat34& a, double x, double y, double z)
{
	Vec3d v;
	v.x = a.m[0][0] * x + a.m[0][1] * y + a.m[0][2] * z + a.t[0];
	v.y = a.m[1][0] * x + a.m[1][1] * y + a.m[1][2] * z + a.t[1];
	v.z = a.m[2][0] * x + a.m[2][1] * y + a.m[2][2] * z + a.t[2];
	return v;
}

double length(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

// The model as it is written: scaled vertices, positions and pivots, written angles.
struct ModelOut
{
	float scale = 1.0f;                       // effective scale
	float draw_scale = 1.0f;                  // the applied draw scale (1 when switched off)
	std::string name_suffix;                  // for the 3DO and KEY names when draw_scale != 1
	bool reroot = false;
	float reroot_z = 0.0f;                    // node 0's z (model units) moved to the insert offset
	std::vector<std::vector<Vec3f>> verts;    // per mesh
	std::vector<double> mesh_radius;
	std::vector<Vec3f> node_pos, node_pivot;
	std::vector<Vec3d> node_rot;              // degrees as written (Raw or RadToDeg)
	double model_radius = 0;
	double insert[3] = {0, 0, 0};
	double rest_extent = 0;                   // max |v| at rest pose (no margin)
	double zmin = 0;
	bool any_vertex = false;
};

double angle_out(float v, AngleMode mode)
{
	// RadToDeg is the original's disabled experiment (a float product divided by M_PI); TPM
	// already stores degrees, so it is never right for TPM data.
	if (mode == AngleMode::RadToDeg)
		return (double)(v * 180.0f) / kPi;
	return v;
}

Vec3f scaled(const Vec3f& v, float s)
{
	Vec3f o;
	o.x = v.x * s;
	o.y = v.y * s;
	o.z = v.z * s;
	return o;
}

// A key entry's position as written (re-rooted for node 0, then scaled).
Vec3f entry_pos_out(const BafEntry& e, bool node0, const ModelOut& g)
{
	Vec3f p = e.pos;
	if (node0 && g.reroot)
		p.z = p.z - g.reroot_z;
	return scaled(p, g.scale);
}

// World matrices of every node for one pose, as JK's rdModel3_BuildExpandedRadius composes
// them: a walk from node 0 through the child and sibling links, each node's matrix being
// World(walk parent) * T(-parent.pivot) * Build34(rot, pos) * T(pivot). reached[i] stays 0 for
// nodes the walk does not visit; their meshes do not count, as in JK. The hierarchy must have
// passed validate_model().
void world_matrices(const std::vector<BafNode>& nodes, const ModelOut& g, const std::vector<Vec3d>& pos,
                    const std::vector<Vec3d>& rot, std::vector<Mat34>& world, std::vector<char>& reached)
{
	const size_t n = nodes.size();
	world.resize(n);
	reached.assign(n, 0);
	if (n == 0)
		return;
	struct Item { int32_t node; int32_t parent_world; };
	std::vector<Item> stack(1, Item{0, -1});
	while (!stack.empty())
	{
		const Item it = stack.back();
		stack.pop_back();
		const size_t i = (size_t)it.node;
		if (reached[i])
			continue;
		reached[i] = 1;
		Mat34 local = mat_build34(rot[i], pos[i]);
		const Vec3f& pv = g.node_pivot[i];
		const Vec3d tp = mat_apply(local, pv.x, pv.y, pv.z);  // Build34 * T(pivot)
		local.t[0] = tp.x;
		local.t[1] = tp.y;
		local.t[2] = tp.z;
		const int32_t parent = nodes[i].parent;
		if (parent >= 0 && (size_t)parent < n)
		{
			const Vec3f& pp = g.node_pivot[(size_t)parent];  // T(-parent.pivot)
			local.t[0] -= pp.x;
			local.t[1] -= pp.y;
			local.t[2] -= pp.z;
		}
		world[i] = it.parent_world >= 0 ? mat_mul(world[(size_t)it.parent_world], local) : local;
		int32_t c = nodes[i].child;
		for (int32_t k = 0; k < nodes[i].num_children && c >= 0 && (size_t)c < n; k++)
		{
			stack.push_back(Item{c, it.node});
			c = nodes[(size_t)c].sibling;
		}
	}
}

// Largest distance from the model origin, and lowest z, of every reachable mesh vertex.
void extent(const std::vector<BafNode>& nodes, const ModelOut& g, const std::vector<Mat34>& world,
            const std::vector<char>& reached, double& max_dist, double& zmin, bool& any, uint64_t& work)
{
	for (size_t i = 0; i < nodes.size(); i++)
	{
		if (!reached[i] || nodes[i].mesh < 0)
			continue;
		for (const Vec3f& v : g.verts[(size_t)nodes[i].mesh])
		{
			const Vec3d w = mat_apply(world[i], v.x, v.y, v.z);
			const double d = length(w.x, w.y, w.z);
			if (!std::isfinite(d))
				continue;
			if (!any || d > max_dist)
				max_dist = d;
			if (!any || w.z < zmin)
				zmin = w.z;
			any = true;
		}
		work += g.verts[(size_t)nodes[i].mesh].size() + 1;
	}
}

// InsertOffsetMode::Auto: how far a model is re-rooted, node 0's z rounded to
// 1/kAutoRerootSteps (0: not re-rooted, node 0 does not lie above z = 0).
float auto_reroot_height(const Baf& b)
{
	if (!b.model_parsed || b.model.nodes.empty())
		return 0.0f;
	const float z = b.model.nodes[0].pos.z;
	if (!(z > 0.0f) || !std::isfinite(z))
		return 0.0f;
	const float r = (float)(std::round((double)z * kAutoRerootSteps) / kAutoRerootSteps);
	return r > 0.0f && std::isfinite(r) ? r : 0.0f;
}

// The rest pose as written.
void rest_pose(const ModelOut& g, std::vector<Vec3d>& pos, std::vector<Vec3d>& rot)
{
	pos.resize(g.node_pos.size());
	rot = g.node_rot;
	for (size_t i = 0; i < pos.size(); i++)
	{
		pos[i].x = g.node_pos[i].x;
		pos[i].y = g.node_pos[i].y;
		pos[i].z = g.node_pos[i].z;
	}
}

// Largest distance over the BAF's keys (integer frames; JK extrapolates the entry at or before
// each frame by its deltas, which are absolute values in the hierarchy's own space).
double animated_extent(const Baf& b, const ModelOut& g, const Options& opts, uint64_t& work, bool& truncated)
{
	const std::vector<BafNode>& nodes = b.model.nodes;
	std::vector<Vec3d> rest_p, rest_r, pos, rot;
	rest_pose(g, rest_p, rest_r);
	std::vector<Mat34> world;
	std::vector<char> reached;
	double best = 0;
	for (const BafKey& k : b.keys)
	{
		const uint32_t frames = std::max<uint32_t>(1, k.num_frames);
		const uint32_t step = frames > kAnimatedRadiusMaxFramesPerKey ? (frames + kAnimatedRadiusMaxFramesPerKey - 1) /
			kAnimatedRadiusMaxFramesPerKey : 1;
		for (uint32_t t = 0; t < frames; t += step)
		{
			if (work > kAnimatedRadiusBudget)
			{
				truncated = true;
				return best;
			}
			pos = rest_p;
			rot = rest_r;
			for (size_t j = 0; j < k.joints.size() && j < nodes.size(); j++)
			{
				const std::vector<BafEntry>& es = k.joints[j].entries;
				if (es.empty())
					continue;
				size_t e = 0;
				while (e + 1 < es.size() && (double)es[e + 1].frame <= (double)t)
					e++;
				const BafEntry& en = es[e];
				const double dt = (double)t - (double)en.frame;
				const Vec3f p = entry_pos_out(en, j == 0, g);
				const Vec3f dp = scaled(en.dpos, g.scale);
				pos[j].x = p.x + ((en.flags & 1) ? dp.x * dt : 0.0);
				pos[j].y = p.y + ((en.flags & 1) ? dp.y * dt : 0.0);
				pos[j].z = p.z + ((en.flags & 1) ? dp.z * dt : 0.0);
				rot[j].x = angle_out(en.rot.x, opts.angles) + ((en.flags & 2) ? angle_out(en.drot.x, opts.angles) * dt : 0.0);
				rot[j].y = angle_out(en.rot.y, opts.angles) + ((en.flags & 2) ? angle_out(en.drot.y, opts.angles) * dt : 0.0);
				rot[j].z = angle_out(en.rot.z, opts.angles) + ((en.flags & 2) ? angle_out(en.drot.z, opts.angles) * dt : 0.0);
			}
			world_matrices(nodes, g, pos, rot, world, reached);
			double d = 0, zmin = 0;
			bool any = false;
			extent(nodes, g, world, reached, d, zmin, any, work);
			if (any && d > best)
				best = d;
		}
	}
	return best;
}

// Prints "-0.000000"-free values for computed RADIUS and INSERT OFFSET numbers.
double tidy(double v) { return std::fabs(v) < 5e-7 ? 0.0 : v; }

// The effective scale and the re-rooting, which the KEYs need even without a 3DO.
// InsertOffsetMode::Node0 re-roots the model at hierarchy node 0: node 0's z moves to 0 in the
// HIERARCHYDEF and in every KEY entry of node 0 (KEY positions are absolute, so JK's animation,
// entry minus rest position, is unchanged; deltas are unchanged), and INSERT OFFSET becomes
// (0, 0, node0.z * scale). TPM actors stand on their origin, and JK keeps a thing's origin
// max(insertOffset.z, moveSize + 0.005) above the floor, so this puts their feet on the floor;
// node 0's position as the insert offset alone would leave them hovering by node0.z * scale.
void init_model_out(const Baf& b, const Options& opts, const BafExtras& extras, ModelOut& g)
{
	g.draw_scale = 1.0f;
	g.name_suffix.clear();
	if (extras.draw_scale)
	{
		// parse_header() keeps the value within kMinDrawScale..kMaxDrawScale
		const long steps = std::lround((double)b.hdr.draw_scale * kDrawScaleSteps);
		g.draw_scale = (float)((double)steps / kDrawScaleSteps);
		if (steps != kDrawScaleSteps)
			g.name_suffix = format("%s%03d", kDrawScaleSuffix, (int)steps);
	}
	g.scale = opts.scale * g.draw_scale;
	g.reroot = opts.insert_offset.mode == InsertOffsetMode::Node0 && b.model_parsed && !b.model.nodes.empty();
	g.reroot_z = g.reroot ? b.model.nodes[0].pos.z : 0.0f;
	if (opts.insert_offset.mode == InsertOffsetMode::Auto)
	{
		// re-rooted as Node0 when node 0 lies above z = 0 (by its rounded height); the INSERT
		// OFFSET is then Floor's for the re-rooted model (compute_model())
		g.reroot_z = auto_reroot_height(b);
		g.reroot = g.reroot_z > 0.0f;
	}
}

// "<name>" with the draw-scale suffix inserted before its extension ("a.key" -> "a_s110.key").
std::string scaled_name(const std::string& name, const std::string& suffix)
{
	if (suffix.empty())
		return name;
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? name + suffix : name.substr(0, dot) + suffix + name.substr(dot);
}

// Everything the 3DO and the KEYs need from the model, in default mode.
void compute_model(const Baf& b, const Options& opts, const BafExtras& extras, ModelOut& g)
{
	const BafModel& m = b.model;
	init_model_out(b, opts, extras, g);

	g.verts.resize(m.meshes.size());
	g.mesh_radius.assign(m.meshes.size(), 0.0);
	for (size_t i = 0; i < m.meshes.size(); i++)
	{
		double d = 0;
		for (const Vec3f& v : m.meshes[i].verts)
		{
			const Vec3f s = scaled(v, g.scale);
			g.verts[i].push_back(s);
			const double l = length(s.x, s.y, s.z);
			if (std::isfinite(l) && l > d)
				d = l;
		}
		g.mesh_radius[i] = d * kJkRadiusMargin + d;  // JK measures mesh radii from the mesh origin
	}
	for (size_t i = 0; i < m.nodes.size(); i++)
	{
		Vec3f p = m.nodes[i].pos;
		if (i == 0 && g.reroot)
			p.z = p.z - g.reroot_z;
		g.node_pos.push_back(scaled(p, g.scale));
		g.node_pivot.push_back(scaled(m.nodes[i].pivot, g.scale));
		Vec3d r;
		r.x = angle_out(m.nodes[i].rot.x, opts.angles);
		r.y = angle_out(m.nodes[i].rot.y, opts.angles);
		r.z = angle_out(m.nodes[i].rot.z, opts.angles);
		g.node_rot.push_back(r);
	}

	std::vector<Vec3d> pos, rot;
	rest_pose(g, pos, rot);
	std::vector<Mat34> world;
	std::vector<char> reached;
	world_matrices(m.nodes, g, pos, rot, world, reached);
	uint64_t work = 0;
	extent(m.nodes, g, world, reached, g.rest_extent, g.zmin, g.any_vertex, work);

	double extent_for_radius = g.rest_extent;
	const bool computed = opts.radius.mode == RadiusMode::Computed || opts.radius.mode == RadiusMode::Animated;
	// A model InsertOffsetMode::Auto re-roots gets the radius --insert-offset node0 gives it: the
	// computed RADIUS then covers the keys too (see process_baf()).
	const bool animated = extras.animated_radius || opts.radius.mode == RadiusMode::Animated ||
		(opts.radius.mode == RadiusMode::Computed && opts.insert_offset.mode == InsertOffsetMode::Auto && g.reroot);
	if (computed && animated)
	{
		bool truncated = false;
		const double a = animated_extent(b, g, opts, work, truncated);
		if (truncated)
			log::warn("%s: the animated radius covers only part of the keys (work limit)\n", b.file.c_str());
		if (a > extent_for_radius)
		{
			log::verbose("%s: animation reaches %f, %.3f x the rest pose\n", b.file.c_str(), a,
				g.rest_extent > 0 ? a / g.rest_extent : 0.0);
			extent_for_radius = a;
		}
	}

	const double stored = (double)(m.radius * g.scale);
	switch (opts.radius.mode)
	{
	case RadiusMode::Fixed:
		g.model_radius = opts.radius.value;
		std::fill(g.mesh_radius.begin(), g.mesh_radius.end(), (double)opts.radius.value);
		break;
	case RadiusMode::Stored:
		// TPM's mesh radius (+0x54) is a sphere around the mesh's bounding-box centre, not the
		// mesh origin JK culls around, so meshes keep the computed radius.
		g.model_radius = stored;
		if (stored < g.rest_extent)
			log::warn_at(b.file, m.offset + 0x5C, "stored model radius %f is smaller than the model (%f): JK would "
				"cull it while visible\n", stored, g.rest_extent);
		break;
	case RadiusMode::Computed:
	case RadiusMode::Animated:
		g.model_radius = extent_for_radius * kJkRadiusMargin + extent_for_radius;
		if (stored < g.rest_extent)
			log::verbose("%s: TPM's stored model radius %f is smaller than the model (%f); computed %f\n",
				b.file.c_str(), stored, g.rest_extent, g.model_radius);
		break;
	}

	switch (opts.insert_offset.mode)
	{
	case InsertOffsetMode::Fixed:
		g.insert[0] = opts.insert_offset.x;
		g.insert[1] = opts.insert_offset.y;
		g.insert[2] = opts.insert_offset.z;
		break;
	case InsertOffsetMode::Floor:
		// The model's lowest rest-pose point on the floor: right for props. TPM actors stand on
		// their origin, so for them this is about 0 (see InsertOffsetMode::Node0 above).
		g.insert[2] = g.any_vertex ? -g.zmin : 0.0;
		break;
	case InsertOffsetMode::Node0:
		g.insert[2] = (double)(g.reroot_z * g.scale);
		break;
	case InsertOffsetMode::Auto:
		// the lowest point of the (possibly re-rooted) model on the floor
		g.insert[2] = g.any_vertex ? -g.zmin : 0.0;
		log::verbose("%s: insert offset: %s\n", b.file.c_str(), g.reroot ? format("re-rooted at node 0 by %f, then "
			"the lowest point", (double)g.reroot_z).c_str() : "the lowest point");
		break;
	}
}

// ------------------------------------------------------------------------------- writers

// "tail" of the legacy debug dumps: the 64 bytes at `pos` (zeros past the end of the file).
void print_tail(TextWriter& mf, const std::vector<uint8_t>& data, size_t pos)
{
	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- TAIL 64bytes ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	uint8_t tail[64] = {0};
	if (pos < data.size())
		memcpy(tail, data.data() + pos, std::min<size_t>(64, data.size() - pos));
	for (int ii = 0; ii < 16; ii++)
		mf.printf("tail[%d]=%d\t%f\n", ii, ld_i32(tail + 4 * ii), ld_f32(tail + 4 * ii));
}

// The model's debug dump in the original's text format (the same bytes as --legacy writes
// for every BAF the original read in sync).
void dump_model_text(TextWriter& mf, const Baf& b)
{
	const BafModel& m = b.model;
	mf.printf("\n---------------------------------------------------------\n");
	mf.printf("-------------------- HEADER ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	mf.printf("mdunno1=%d\n", ld_i32(m.raw + 0x20));
	const int32_t numNodes = ld_i32(m.raw + 0x24);
	mf.printf("numNodes=%d\n", numNodes);
	for (int ii = 0; ii < 23; ii++)
		mf.printf("mdunno[%d]=%d\n", ii, ld_i32(m.raw + 0x28 + 4 * ii));
	if (numNodes > 0)
	{
		mf.printf("\n---------------------------------------------------------\n");
		mf.printf("-------------------- NODE DEFINITIONS ------------------------\n");
		mf.printf("---------------------------------------------------------\n");
		for (const BafMesh& me : m.meshes)
		{
			mf.printf("\n----------------\n");
			mf.printf("nodeName=%s\n", fixed_str(me.raw, kMeshHeaderSize).c_str());
			mf.printf("someIndex1=%d\n", ld_i32(me.raw + 0x20));
			mf.printf("geoMode=%d\n", ld_i32(me.raw + 0x24));
			mf.printf("lightMode=%d\n", ld_i32(me.raw + 0x28));
			mf.printf("texMode=%d\n", ld_i32(me.raw + 0x2C));
			for (int ii = 0; ii < 6; ii++)
				mf.printf("ndunno[%d]=%d\n", ii, ld_i32(me.raw + 0x30 + 4 * ii));
			mf.printf("numVerts=%d\n", ld_i32(me.raw + 0x48));
			mf.printf("numTexVerts=%d\n", ld_i32(me.raw + 0x4C));
			mf.printf("numFaces=%d\n", ld_i32(me.raw + 0x50));
			for (int ii = 0; ii < 7; ii++)
				mf.printf("fdunno[%d]=%f\n", ii, ld_f32(me.raw + 0x54 + 4 * ii));
		}
	}
	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- NODES!!!!!!! ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	for (const BafMesh& me : m.meshes)
	{
		mf.printf("\nnode: %s\n---------------------------\n", fixed_str(me.raw, kMeshHeaderSize).c_str());
		const int nv = (int)me.verts.size();
		for (int ii = 0; ii < nv; ii++)
			mf.printf("vert[%d]: %f/%f/%f\n", ii, me.verts[(size_t)ii].x, me.verts[(size_t)ii].y, me.verts[(size_t)ii].z);
		for (int ii = 0; ii < nv; ii++)
		{
			const float fx = me.norms[(size_t)ii].x, fy = me.norms[(size_t)ii].y, fz = me.norms[(size_t)ii].z;
			const float mag = sqrtf(fx * fx + fy * fy + fz * fz);
			mf.printf("norm[%d]: mag=%f\t%f/%f/%f\n", ii, mag, fx, fy, fz);
		}
		// vertices_i and vertices_unk printed as the original's interleaved "uvscroll?" pairs
		for (int ii = 0; ii < nv; ii++)
			mf.printf("uvscroll?[%d]: %f/%f\n", ii, ld_f32(me.vi_raw + 8 * ii), ld_f32(me.vi_raw + 8 * ii + 4));
		for (size_t ii = 0; ii < me.uvs.size() / 2; ii++)
			mf.printf("texvert[%d]: %f/%f\n", (int)ii, me.uvs[ii * 2], me.uvs[ii * 2 + 1]);
		for (size_t fi = 0; fi < me.faces.size(); fi++)
		{
			const uint8_t* f = me.faces[fi].raw;
			const int32_t mi = ld_i32(f);
			const char* matName = (mi >= 0 && (size_t)mi < b.mats.size()) ? b.mats[(size_t)mi].name.c_str()
				: "(invalid material index)";
			mf.printf("face[%d]: matIndex=%d (%s), flags=%d, geo=%d, light=%d, tex=%d, numVerts=%d, d5=0x%08X, "
				"d6=0x%08X, d7=0x%08X, d8=%d, d9=%d, d10=%d, d11=%d, d12=%d, norm=(%f/%f/%f), d13=%d, d14=%d\n",
				(int)fi, mi, matName, ld_i32(f + 0x04), ld_i32(f + 0x08), ld_i32(f + 0x0C), ld_i32(f + 0x10),
				ld_i32(f + 0x14), ld_u32(f + 0x18), ld_u32(f + 0x1C), ld_u32(f + 0x20), ld_i32(f + 0x24),
				ld_i32(f + 0x28), ld_i32(f + 0x2C), ld_i32(f + 0x30), ld_i32(f + 0x34), ld_f32(f + 0x38),
				ld_f32(f + 0x3C), ld_f32(f + 0x40), ld_i32(f + 0x44), ld_i32(f + 0x48));
		}
		for (size_t fi = 0; fi < me.faces.size(); fi++)
		{
			mf.printf("faceindices[%d]=", (int)fi);
			for (int32_t i : me.faces[fi].pos)
				mf.printf("%d,", i);
			mf.printf(" | ");
			for (int32_t i : me.faces[fi].uv)
				mf.printf("%d,", i);
			mf.printf("\n");
		}
	}
	mf.printf("\n\n\n---------------------------------------------------------\n");
	mf.printf("-------------------- NODE HIERARCHY ------------------------\n");
	mf.printf("---------------------------------------------------------\n");
	for (size_t i = 0; i < m.nodes.size(); i++)
	{
		const uint8_t* h = m.nodes[i].raw;
		mf.printf("%d: hierarchy node=%s\n", (int)i, fixed_str(h, kNodeSize).c_str());
		for (int ii = 0; ii < 17; ii++)
			mf.printf("hdunno[%d]=%d\t%f\n", ii, ld_i32(h + 8 + 4 * ii), ld_f32(h + 8 + 4 * ii));
		mf.printf("meshIndex=%d\n", ld_i32(h + 0x4C));
		mf.printf("parentIndex=%d\n", ld_i32(h + 0x50));
		mf.printf("numChildren=%d\n", ld_i32(h + 0x54));
		mf.printf("firstChildIndex=%d\n", ld_i32(h + 0x58));
		mf.printf("siblingIndex=%d\n", ld_i32(h + 0x5C));
		mf.printf("pivot=%f/%f/%f\n", ld_f32(h + 0x60), ld_f32(h + 0x64), ld_f32(h + 0x68));
		mf.printf("pos=%f/%f/%f\n", ld_f32(h + 0x6C), ld_f32(h + 0x70), ld_f32(h + 0x74));
		mf.printf("pyr=%f/%f/%f\n", ld_f32(h + 0x78), ld_f32(h + 0x7C), ld_f32(h + 0x80));
		for (int ii = 0; ii < 12; ii++)
			mf.printf("hdunno2[%d]=%d\t%f\n", ii, ld_i32(h + 0x84 + 4 * ii), ld_f32(h + 0x84 + 4 * ii));
		mf.printf("\n");
	}
	print_tail(mf, *b.data, m.end);
}

// A key's debug dump in the original's text format.
void dump_key_text(TextWriter& mf, const Baf& b, const BafKey& k)
{
	const uint8_t* h = k.raw;
	mf.printf("dunno1: %d\n", ld_i32(h + 0x20));
	mf.printf("dunno2: %d\n", ld_i32(h + 0x24));
	mf.printf("dunno3: %d\n", ld_i32(h + 0x28));
	mf.printf("type: %d\n", ld_i32(h + 0x2C));
	mf.printf("fps: %f\n", ld_f32(h + 0x30));
	mf.printf("numFrames: %d\n", ld_i32(h + 0x34));
	mf.printf("numNodes: %d\n", ld_i32(h + 0x38));
	for (int x = 0; x < 18; x++)
		mf.printf("bleh[%d]=%d\t%f\n", x, ld_i32(h + 0x3C + 4 * x), ld_f32(h + 0x3C + 4 * x));
	for (const BafJoint& j : k.joints)
		mf.printf("node=%s\t%d\t%d\n", fixed_str(j.raw, kJointSize).c_str(), ld_i32(j.raw + 0x20), ld_i32(j.raw + 0x24));
	for (const BafJoint& j : k.joints)
	{
		for (size_t x = 0; x < j.entries.size(); x++)
		{
			const BafEntry& e = j.entries[x];
			mf.printf("frame[%d]: %f\t%d\tpos=%f/%f/%f\tpyr=%f/%f/%f\tdeltapos=%f/%f/%f\tdeltapyr=%f/%f/%f\n", (int)x,
				e.frame, (int32_t)e.flags, e.pos.x, e.pos.y, e.pos.z, e.rot.x, e.rot.y, e.rot.z, e.dpos.x, e.dpos.y,
				e.dpos.z, e.drot.x, e.drot.y, e.drot.z);
		}
	}
	print_tail(mf, *b.data, k.end);
}

bool close_text(TextWriter& mf)
{
	std::string err;
	if (mf.close(&err))
		return true;
	log::error("%s\n", err.c_str());
	return false;
}

bool write_model_3do(const std::string& path, const Baf& b, const ModelOut& g,
                     const std::vector<std::string>& mat_list)
{
	const BafModel& m = b.model;
	TextWriter mf;
	std::string err;
	if (!mf.open(path, &err))
	{
		log::error("%s\n", err.c_str());
		return false;
	}
	mf.printf("# MODEL '%s' created with TPMExtractor\n", m.name.c_str());
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HEADER\n");
	mf.printf("\n");
	mf.printf("3DO 2.1\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: MODELRESOURCE\n");
	mf.printf("\n");
	mf.printf("# Materials list\n");
	mf.printf("MATERIALS %d\n", (int)mat_list.size());
	mf.printf("\n");
	for (size_t x = 0; x < mat_list.size(); x++)
		mf.printf("%d: %s\n", (int)x, mat_list[x].c_str());
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: GEOMETRYDEF\n");
	mf.printf("\n");
	mf.printf("# Object radius\n");
	mf.printf("RADIUS %f\n", tidy(g.model_radius));
	mf.printf("\n");
	mf.printf("# Insertion offset\n");
	mf.printf("INSERT OFFSET %f %f %f\n", tidy(g.insert[0]), tidy(g.insert[1]), tidy(g.insert[2]));
	mf.printf("\n");
	mf.printf("# Number of Geometry Sets\n");
	mf.printf("GEOSETS 1\n");
	mf.printf("\n");
	mf.printf("# Geometry Set definition\n");
	mf.printf("GEOSET 0\n");
	mf.printf("\n");
	mf.printf("# Number of Meshes\n");
	mf.printf("MESHES %d\n", (int)m.meshes.size());
	mf.printf("\n");
	for (size_t mi = 0; mi < m.meshes.size(); mi++)
	{
		const BafMesh& me = m.meshes[mi];
		mf.printf("\n");
		mf.printf("# Mesh definition\n");
		mf.printf("MESH %d\n", (int)mi);
		mf.printf("\n");
		mf.printf("NAME %s\n", me.name.c_str());
		mf.printf("\n");
		mf.printf("RADIUS %f\n", tidy(g.mesh_radius[mi]));
		mf.printf("\n");
		mf.printf("GEOMETRYMODE %d\n", me.geo);
		mf.printf("LIGHTINGMODE %d\n", me.light);
		mf.printf("TEXTUREMODE %d\n", kJkTextureModePerspective);
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("VERTICES %d\n", (int)me.verts.size());
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:         i: \n");
		for (size_t vi = 0; vi < g.verts[mi].size(); vi++)
			mf.printf("%d: %f %f %f %f\n", (int)vi, g.verts[mi][vi].x, g.verts[mi][vi].y, g.verts[mi][vi].z, 0.0);
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("TEXTURE VERTICES %d\n", (int)(me.uvs.size() / 2));
		mf.printf("\n");
		for (size_t vi = 0; vi < me.uvs.size() / 2; vi++)
			mf.printf("%d: %f %f\n", (int)vi, me.uvs[vi * 2], me.uvs[vi * 2 + 1]);
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("VERTEX NORMALS\n");
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:\n");
		for (size_t vi = 0; vi < me.norms.size(); vi++)
			mf.printf("%d: %f %f %f\n", (int)vi, me.norms[vi].x, me.norms[vi].y, me.norms[vi].z);
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("FACES %d\n", (int)me.faces.size());
		mf.printf("\n");
		mf.printf("#  num:  material:   type:  geo:  light:   tex:  extralight:  verts:\n");
		for (size_t fi = 0; fi < me.faces.size(); fi++)
		{
			const BafFace& fc = me.faces[fi];
			const int32_t mat = (fc.material >= 0 && (size_t)fc.material < b.mats.size()) ? fc.material : -1;
			mf.printf("%d: %d 0x%X %d %d %d %f %d", (int)fi, mat, fc.flags & kJkFaceFlagMask, fc.geo, fc.light,
				kJkTextureModePerspective, 0.0, (int)fc.pos.size());
			for (size_t vi = 0; vi < fc.pos.size(); vi++)
				mf.printf(" %d, %d", fc.pos[vi], vi < fc.uv.size() ? fc.uv[vi] : 0);
			mf.printf("\n");
		}
		mf.printf("\n");
		mf.printf("\n");
		mf.printf("FACE NORMALS\n");
		mf.printf("\n");
		mf.printf("# num:     x:         y:         z:\n");
		for (size_t fi = 0; fi < me.faces.size(); fi++)
			mf.printf("%d: %f %f %f\n", (int)fi, me.faces[fi].normal.x, me.faces[fi].normal.y, me.faces[fi].normal.z);
	}
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HIERARCHYDEF\n");
	mf.printf("\n");
	mf.printf("# Hierarchy node list\n");
	mf.printf("HIERARCHY NODES %d\n", (int)m.nodes.size());
	mf.printf("\n");
	mf.printf("#  num:   flags:   type:    mesh:  parent:  child:  sibling:  numChildren:        x:         y:         z:     pitch:       yaw:      roll:    pivotx:    pivoty:    pivotz:  hnodename:\n");
	for (size_t i = 0; i < m.nodes.size(); i++)
	{
		const BafNode& nd = m.nodes[i];
		mf.printf("%d: 0x%X 0x%X %d %d %d %d %d %f %f %f %f %f %f %f %f %f %s\n", (int)i, 0x0u, nd.type, nd.mesh,
			nd.parent, nd.child, nd.sibling, nd.num_children,
			g.node_pos[i].x, g.node_pos[i].y, g.node_pos[i].z,
			g.node_rot[i].x, g.node_rot[i].y, g.node_rot[i].z,
			g.node_pivot[i].x, g.node_pivot[i].y, g.node_pivot[i].z,
			nd.name.c_str());
	}
	mf.printf("\n");
	return close_text(mf);
}

// JOINTS is the size of the joint array JK allocates for the key, and JK's animation code reads
// that array at every hierarchy node's index (the original engine without a bounds check), so
// it covers the model's nodes even when the key animates fewer of them; NODES stays the number
// of joints written, and the joints without a NODE block are left unanimated.
bool write_key(const std::string& path, const BafKey& k, const ModelOut& g, const Options& opts, size_t model_nodes)
{
	TextWriter mf;
	std::string err;
	if (!mf.open(path, &err))
	{
		log::error("%s\n", err.c_str());
		return false;
	}
	mf.printf("# KEYFRAME '%s' created with TPMExtractor\n", k.name.c_str());
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: HEADER\n");
	mf.printf("\n");
	mf.printf("FLAGS 0x0\n");
	mf.printf("TYPE 0x%04X\n", k.type);
	mf.printf("FRAMES %d\n", (int32_t)k.num_frames);
	mf.printf("FPS %f\n", k.fps);
	mf.printf("JOINTS %d\n", (int)std::max(k.joints.size(), model_nodes));
	mf.printf("\n");
	mf.printf("\n");
	mf.printf("###############\n");
	mf.printf("SECTION: KEYFRAME NODES\n");
	mf.printf("\n");
	mf.printf("NODES %d\n", (int)k.joints.size());
	mf.printf("\n");
	for (size_t j = 0; j < k.joints.size(); j++)
	{
		const BafJoint& jt = k.joints[j];
		mf.printf("NODE %d\n", (int)j);
		mf.printf("MESH NAME %s\n", jt.name.c_str());
		mf.printf("ENTRIES %d\n", (int)jt.entries.size());
		mf.printf("\n");
		mf.printf("# num:   frame:   flags:           x:           y:           z:           p:           y:           r:\n");
		mf.printf("#                                 dx:          dy:          dz:          dp:          dy:          dr:\n");
		for (size_t x = 0; x < jt.entries.size(); x++)
		{
			const BafEntry& e = jt.entries[x];
			const Vec3f p = entry_pos_out(e, j == 0, g);
			const Vec3f dp = scaled(e.dpos, g.scale);
			mf.printf("%d:\t%d\t0x%04X\t%f\t%f\t%f\t%f\t%f\t%f\n", (int)x, float_to_int(e.frame), e.flags, p.x, p.y, p.z,
				angle_out(e.rot.x, opts.angles), angle_out(e.rot.y, opts.angles), angle_out(e.rot.z, opts.angles));
			mf.printf("\t\t\t%f\t%f\t%f\t%f\t%f\t%f\n", dp.x, dp.y, dp.z, angle_out(e.drot.x, opts.angles),
				angle_out(e.drot.y, opts.angles), angle_out(e.drot.z, opts.angles));
		}
		mf.printf("\n");
	}
	return close_text(mf);
}

// --palette as a JK colormap: its palette and the "glowing" indices a remapped texel must avoid
// (they ignore lighting). Loaded once per run; a raw .pal has no light table, so nothing glows.
struct TargetPalette
{
	std::string path;
	bool loaded = false;
	jk::IndexSet glow;
};

const jk::IndexSet& palette_glow(const std::string& path)
{
	static TargetPalette cache;
	if (!cache.loaded || cache.path != path)
	{
		cache = TargetPalette();
		cache.path = path;
		cache.loaded = true;
		jk::Cmp cmp;
		std::string err;
		if (!path.empty() && jk::read_cmp(path, cmp, &err))
			cache.glow = jk::glow_indices(cmp);
	}
	return cache.glow;
}

bool is_pow2_side(int v) { return v >= 1 && v <= jk::kMat8MaxDimension && (v & (v - 1)) == 0; }

// Levels an 8-bit MAT gets: the stored ones (at most 4), and generated ones while both sides
// stay at least kMat8MinMipSide texels.
int mat8_levels(int w, int h, size_t stored)
{
	int levels = 1;
	while (levels < kMat8MipLevels && (std::min(w, h) >> levels) >= kMat8MinMipSide)
		levels++;
	return std::max(levels, (int)std::min<size_t>(stored, (size_t)kMat8MipLevels));
}

// The retail 8-bit MAT of one material: every texture (stored mips kept, missing ones
// generated), one cel per texture, masked when the texture's transparent flag is non-zero.
// Indices are remapped from the texture's palette (`src`) into `dst`; an unmasked texture's
// index 0 is an opaque colour and moves to its nearest other index, so it never turns into
// JK's transparent key.
// `error` receives the largest mean colour error of the material's textures: the RGB distance
// (0..441) between each opaque texel's colour and the colour it got, averaged over the first mip.
bool write_mat8_file(const std::string& path, const BafMaterial& m, const Palette& src, const Palette& dst,
                     const jk::IndexSet& allowed, const std::string& baf, double& error)
{
	error = 0.0;
	if (m.bpp != 8)
	{
		log::error_at(baf, m.offset, "material %s: %d bits per pixel; no 8-bit MAT\n", warn_name(m.name).c_str(), m.bpp);
		return false;
	}
	if (m.textures.empty() || m.textures.size() > (size_t)jk::kMat8MaxCels)
	{
		log::error_at(baf, m.offset, "material %s: %u textures; an 8-bit MAT holds 1..%d\n", warn_name(m.name).c_str(),
			(unsigned)m.textures.size(), jk::kMat8MaxCels);
		return false;
	}
	const bool same_palette = memcmp(src.c, dst.c, sizeof src.c) == 0;
	jk::Mat8 mat;
	for (const BafTexture& t : m.textures)
	{
		if (!is_pow2_side(t.width) || !is_pow2_side(t.height) || t.mips.empty())
		{
			log::error_at(baf, t.offset, "material %s: a %dx%d texture with %u mips cannot be a retail 8-bit MAT "
				"(power-of-two sides up to %d)\n", warn_name(m.name).c_str(), t.width, t.height, (unsigned)t.mips.size(),
				jk::kMat8MaxDimension);
			return false;
		}
		jk::Mat8Texture out;
		out.width = t.width;
		out.height = t.height;
		out.masked = t.transparent != 0;
		std::array<uint8_t, 256> table;
		if (same_palette)
		{
			for (int i = 0; i < 256; i++)
				table[(size_t)i] = (uint8_t)i;
			if (!out.masked)
				table[0] = jk::nearest_index(dst, dst.c[0], allowed, kMat8Metric);
		}
		else
			table = jk::remap_table(src, dst, allowed, out.masked, kMat8Metric);
		double sum = 0.0;
		size_t opaque = 0;
		for (uint8_t p : t.mips[0])
		{
			if (out.masked && p == 0)
				continue;
			const Rgb a = src.c[p], c = dst.c[table[p]];
			sum += length((double)a.r - c.r, (double)a.g - c.g, (double)a.b - c.b);
			opaque++;
		}
		if (opaque)
			error = std::max(error, sum / (double)opaque);
		for (size_t k = 0; k < t.mips.size() && k < (size_t)kMat8MipLevels; k++)
		{
			std::vector<uint8_t> level(t.mips[k]);
			for (uint8_t& p : level)
				p = table[p];
			out.mips.push_back(std::move(level));
		}
		std::string err;
		if (!jk::generate_mips(out, mat8_levels(t.width, t.height, t.mips.size()), dst, allowed, kMat8Metric, &err))
		{
			log::error_at(baf, t.offset, "material %s: %s\n", warn_name(m.name).c_str(), err.c_str());
			return false;
		}
		jk::Mat8Cel cel;
		cel.texture = (int)mat.textures.size();
		cel.solid_color = jk::representative_index(out, dst, allowed, kMat8Metric);
		mat.cels.push_back(cel);
		mat.textures.push_back(std::move(out));
	}
	std::string err;
	if (!jk::write_mat8(path, mat, &err))
	{
		log::error("%s: %s\n", path.c_str(), err.c_str());
		return false;
	}
	return true;
}

std::string lower_ascii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return s;
}

// The 8-bit MAT names: "<name minus its last 4 characters>8.mat", unless a raw MAT of the same
// BAF already has that name (none on the retail disc); then "<base>8_<n>.mat".
std::vector<std::string> mat8_names(const Baf& b)
{
	std::set<std::string> raw;
	for (const BafMaterial& m : b.mats)
		raw.insert(lower_ascii(m.name));
	std::vector<std::string> out;
	for (const BafMaterial& m : b.mats)
	{
		const std::string base = m.name.size() >= 4 ? m.name.substr(0, m.name.size() - 4) : m.name;
		std::string name = base + kMat8Suffix;
		for (int n = 2; raw.count(lower_ascii(name)); n++)
			name = base + format("8_%d.mat", n);
		if (name != base + kMat8Suffix)
			log::warn_at(b.file, m.offset, "material %s: %s is the name of a raw MAT here; the 8-bit MAT is %s\n",
				warn_name(m.name).c_str(), (base + kMat8Suffix).c_str(), name.c_str());
		out.push_back(name);
	}
	return out;
}

// The colormap written for a BAF that has none: "<BAF file name up to its first '.'>.cmp" (the
// name most retail colormaps have), or "<that>_<n>.cmp" when a name from the BAF already takes it.
std::string own_cmp_name(const Baf& b)
{
	std::string stem = platform::base_name(b.file);
	stem = stem.substr(0, stem.find('.'));
	if (stem.empty())
		stem = "colormap";
	std::set<std::string> taken;
	for (const BafMaterial& m : b.mats)
		taken.insert(lower_ascii(m.name));
	if (b.model_parsed)
		taken.insert(lower_ascii(b.model.name));
	for (const BafKey& k : b.keys)
		taken.insert(lower_ascii(k.name));
	std::string name = stem + ".cmp";
	for (int n = 2; taken.count(lower_ascii(name)); n++)
		name = stem + format("_%d.cmp", n);
	return name;
}

bool write_retail_cmp(const std::string& szdump, const std::string& name, const Palette& pal, const std::string& baf)
{
	std::string path, why;
	if (!out_path(szdump, name, baf, path))
		return false;
	jk::Cmp cmp;
	if (!jk::build_cmp(pal, jk::CmpBuildOptions(), cmp, &why) || !jk::write_cmp(path, cmp, &why))
	{
		log::error("%s: %s\n", path.c_str(), why.c_str());
		return false;
	}
	return true;
}

bool default_dumpbaf(const std::string& szfile, const std::vector<uint8_t>& data, const std::string& szdump,
                     const Options& opts, const Palette* fallback_palette, const BafExtras& extras)
{
	Baf b;
	bool ok = parse_baf(szfile, data, b);
	std::string path, why;

	// The palette of textures without their own: the colormap, else --palette or grayscale.
	const Palette shared = b.has_colormap ? b.cmp_palette : (fallback_palette ? *fallback_palette : Palette::grayscale());
	// The palette every 8-bit MAT indexes: --palette, else the colormap, else (a BAF without a
	// colormap) the first material's own palette, else the grayscale ramp of the original.
	const BafMaterial* own_first = nullptr;
	for (const BafMaterial& m : b.mats)
		if (m.own_palette)
		{
			own_first = &m;
			break;
		}
	const Palette mat8_palette = fallback_palette ? *fallback_palette
		: b.has_colormap ? b.cmp_palette : own_first ? own_first->palette : shared;

	// colormap: a retail CMP (light and transparency tables) under the BAF's colormap name. A BAF
	// without a colormap gets one named after the BAF with the palette its 8-bit MATs index, so
	// they come with the colormap they need, as every other BAF's do (not with --palette: the
	// MATs then index that palette).
	if (b.cmp_parsed)
		ok = write_retail_cmp(szdump, b.cmp_name, b.cmp_palette, szfile) && ok;
	else if (!b.has_colormap && opts.mat8 && !fallback_palette && !b.mats.empty())
	{
		const std::string name = own_cmp_name(b);
		ok = write_retail_cmp(szdump, name, mat8_palette, szfile) && ok;
		const std::string what = own_first ? "the palette of material " + warn_name(own_first->name)
			: std::string("the grayscale palette");
		log::verbose("%s: no colormap: %s holds %s, which the 8-bit MATs index\n", szfile.c_str(), name.c_str(),
			what.c_str());
	}

	jk::IndexSet allowed;
	allowed.set();
	allowed.reset(0);
	if (fallback_palette)
		allowed &= ~palette_glow(opts.palette);
	const std::vector<std::string> names8 = mat8_names(b);

	std::vector<std::string> mat_list;
	unsigned off_colour = 0;          // 8-bit MATs over kMat8RemapWarnError
	double worst_error = 0.0;
	std::string worst_mat;
	for (size_t i = 0; i < b.mats.size(); i++)
	{
		const BafMaterial& m = b.mats[i];
		const Palette& pal = m.own_palette ? m.palette : shared;
		mat_list.push_back(opts.mat8 ? names8[i] : mat16_name(m.name));

		if (out_path(szdump, m.name, szfile, path))
			ok = write_bytes(path, std::vector<uint8_t>(m.stream, m.stream + m.stream_size)) && ok;
		else
			ok = false;

		// BMP and 16-bit MAT of the first texture's first mip, as the original
		const BafTexture* first = nullptr;
		for (const BafTexture& t : m.textures)
			if (!t.mips.empty())
			{
				first = &t;
				break;
			}
		if (first && (opts.bmp || opts.mat16))
		{
			if (m.bpp != 8)
			{
				log::error_at(szfile, m.offset, "material %s: %d bits per pixel; no BMP or 16-bit MAT\n",
					warn_name(m.name).c_str(), m.bpp);
				ok = false;
			}
			else
				ok = write_preview_and_mat16(szdump, szfile, m.name, first->width, first->height, first->transparent != 0,
					first->mips[0], pal, opts, BmpRows::Padded) && ok;
		}

		if (opts.mat8)
		{
			double error = 0.0;
			if (out_path(szdump, names8[i], szfile, path))
				ok = write_mat8_file(path, m, pal, mat8_palette, allowed, szfile, error) && ok;
			else
				ok = false;
			log::debug("%s: material %s: mean colour error %.1f in %s\n", szfile.c_str(), warn_name(m.name).c_str(), error,
				names8[i].c_str());
			if (error > kMat8RemapWarnError)
			{
				off_colour++;
				if (error > worst_error)
				{
					worst_error = error;
					worst_mat = m.name;
				}
			}
		}
	}
	if (off_colour)
		log::warn("%s: %u 8-bit MAT(s) lose their colours in the palette they index (%s): mean RGB error over %.0f, "
			"worst %s at %.1f of 441\n", szfile.c_str(), off_colour, fallback_palette ? "--palette" : "the BAF's colormap",
			kMat8RemapWarnError, warn_name(worst_mat).c_str(), worst_error);

	// The 3DO and the KEYs carry the applied draw scale in their names when it is not 1; the
	// debug dumps keep the BAF's own names (they show its records, unscaled).
	ModelOut g;
	init_model_out(b, opts, extras, g);
	if (!g.name_suffix.empty() && (b.model_parsed || !b.keys.empty()))
		log::info("%s: drawn at scale %.2f: its 3DO and KEYs are named *%s.* (JK holds one model or key per name, "
			"and other BAFs may share these names at another scale)\n", szfile.c_str(), (double)g.draw_scale,
			g.name_suffix.c_str());
	std::string out_name;
	if (b.model_parsed)
	{
		const bool name_ok = out_path(szdump, b.model.name, szfile, path) &&
			out_path(szdump, scaled_name(b.model.name, g.name_suffix), szfile, out_name);
		if (name_ok && opts.debug_dumps)
		{
			TextWriter mf;
			if (mf.open(path + ".txt", &why))
			{
				dump_model_text(mf, b);
				ok = close_text(mf) && ok;
			}
			else
			{
				log::error("%s\n", why.c_str());
				ok = false;
			}
		}
		if (b.model_ok)
		{
			compute_model(b, opts, extras, g);
			if (name_ok)
				ok = write_model_3do(out_name, b, g, mat_list) && ok;
		}
		if (!name_ok)
			ok = false;
	}
	if (opts.insert_offset.mode == InsertOffsetMode::Node0 && !b.keys.empty() && !g.reroot)
		log::warn("%s: no hierarchy node 0: keys are not re-rooted\n", szfile.c_str());

	for (const BafKey& k : b.keys)
	{
		if (!out_path(szdump, k.name, szfile, path) || !out_path(szdump, scaled_name(k.name, g.name_suffix), szfile,
			out_name))
		{
			ok = false;
			continue;
		}
		if (opts.debug_dumps)
		{
			TextWriter mf;
			if (mf.open(path + ".txt", &why))
			{
				dump_key_text(mf, b, k);
				ok = close_text(mf) && ok;
			}
			else
			{
				log::error("%s\n", why.c_str());
				ok = false;
			}
		}
		const size_t model_nodes = b.model_parsed ? b.model.nodes.size() : 0;
		ok = write_key(out_name, k, g, opts, model_nodes) && ok;
		if (k.joints.size() < model_nodes)
			log::verbose("%s: key %s animates %u of the model's %u hierarchy nodes (JOINTS %u)\n", szfile.c_str(),
				warn_name(k.name).c_str(), (unsigned)k.joints.size(), (unsigned)model_nodes, (unsigned)model_nodes);
		if (b.model_parsed && k.joints.size() > b.model.nodes.size())
			log::warn_at(szfile, k.offset, "key %s animates %u joints; the model has %u hierarchy nodes\n",
				warn_name(k.name).c_str(), (unsigned)k.joints.size(), (unsigned)b.model.nodes.size());
		for (size_t j = 0; j < k.joints.size(); j++)
			if (k.joints[j].node != (int32_t)j)
			{
				log::warn_at(szfile, k.offset, "key %s: joint %u names node %d; JK applies it to node %u\n",
					warn_name(k.name).c_str(), (unsigned)j, k.joints[j].node, (unsigned)j);
				break;
			}
	}

	// REMAIN.HEX: what the original's read left over (the sound-event records after the first
	// 64 bytes, which its last "tail" read took)
	if (opts.debug_dumps && b.complete)
	{
		const size_t last = b.keys.empty() ? b.model.end : b.keys.back().end;
		const size_t from = std::min(data.size(), last + 64);
		if (out_path(szdump, "REMAIN.HEX", szfile, path))
			ok = write_bytes(path, std::vector<uint8_t>(data.begin() + (std::ptrdiff_t)from, data.end())) && ok;
		else
			ok = false;
	}
	return ok;
}

} // namespace

bool process_baf(const std::string& baf_path, const std::string& out_dir, const Options& opts,
                 const Palette* fallback_palette)
{
	BafExtras extras;
	extras.draw_scale = opts.draw_scale;
	// The computed RADIUS also covers the keys with --insert-offset node0: the re-rooted model's
	// origin is node 0 (the pelvis of TPM's people), and their keys swing limbs and weapons past
	// the rest-pose RADIUS around it (in 164 of the 303 retail BAFs), so JK, which culls the whole
	// model with that sphere, would drop actors while they are on screen. --insert-offset auto
	// does the same for each model it re-roots (compute_model()).
	extras.animated_radius = opts.radius.mode == RadiusMode::Animated ||
		(opts.radius.mode == RadiusMode::Computed && opts.insert_offset.mode == InsertOffsetMode::Node0);
	return process_baf(baf_path, out_dir, opts, fallback_palette, extras);
}

bool process_baf(const std::string& baf_path, const std::string& out_dir, const Options& opts,
                 const Palette* fallback_palette, const BafExtras& extras)
{
	try
	{
		log::info("Processing: %s\n", baf_path.c_str());
		uint64_t size = 0;
		if (platform::file_size(baf_path, size) && size > kMaxBafFileSize)
		{
			log::error("%s: %llu bytes; a BAF is at most %llu\n", baf_path.c_str(), (unsigned long long)size,
				(unsigned long long)kMaxBafFileSize);
			return false;
		}
		std::vector<uint8_t> data;
		std::string err;
		if (!platform::read_file(baf_path, data, &err))
		{
			log::error("%s\n", err.c_str());
			return false;
		}
		if (opts.legacy)
			return legacy_dumpbaf(baf_path, data, out_dir, opts, fallback_palette);
		if (opts.angles == AngleMode::RadToDeg)
		{
			static bool warned = false;
			if (!warned)
				log::warn("--angles rad2deg multiplies TPM's angles by 57.3; TPM stores degrees, so this is never "
					"right for TPM data\n");
			warned = true;
		}
		return default_dumpbaf(baf_path, data, out_dir, opts, fallback_palette, extras);
	}
	catch (const ParseError& e)
	{
		log::error("%s\n", e.what());
		return false;
	}
	catch (const std::exception& e)
	{
		log::error("%s: giving up on this file: %s\n", baf_path.c_str(), e.what());
		return false;
	}
}

} // namespace tpmx
