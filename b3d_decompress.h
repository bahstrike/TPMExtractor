#pragma once

// Code provided by shiny
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): new entry point signature with
// error reporting; the decoder works on memory buffers with bounds checks and size limits and
// reports truncated input. It decodes exactly as before (the same bytes on every input).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// A level file starts with bytes that are always stored as they are: the 12-byte header of its
// first section ("B3D FILE") and that section's 2,200-byte body. LZSS data follows them.
const size_t kB3dRawHeaderSize = 0x8A4;

// File offset of the header's 32-bit "compressed" flag (body offset 0x31C). It is 1 in every
// retail .B3D; like the original tool, the decompressed image clears its low byte.
const size_t kB3dCompressedFlagOffset = 0x328;

// Sanity limit for level files and for decompressed images (input and output alike). The
// largest retail level is 2,567,529 bytes compressed and 10,410,819 bytes decompressed; LZSS
// expands at most 8:1 (a 17-bit token copies up to 17 bytes).
const size_t kB3dMaxImageSize = (size_t)128 << 20;

// How an LZSS stream ended.
enum class LzssEnd {
    Marker,       // at the end marker (a back reference with ring offset 0): the stream is complete
    InputEnd,     // the input ran out before an end marker: the data is truncated
    OutputLimit,  // the output reached the size limit; decoding stopped there
};

struct LzssResult
{
    LzssEnd end = LzssEnd::InputEnd;
    size_t consumed = 0;  // input bytes read, including the byte that holds the end marker's last bit
    size_t produced = 0;  // bytes appended to the output
};

// Decodes TPM's level LZSS stream and appends at most max_output bytes to `out`.
// Format: bits are read most significant first with no byte alignment between tokens. A 1 bit
// is followed by an 8-bit literal. A 0 bit is followed by a 12-bit ring offset: offset 0 ends the
// stream, any other offset is followed by a 4-bit length L, and L+2 bytes are copied one at a
// time from ring[(offset + i) & 0xFFF]. Every output byte is also written to the ring, a
// 4,096-byte buffer that starts zeroed with its write position at 1.
// When the input ends inside a token, original_quirks reproduces what the original decoder
// wrote for that token (a literal becomes one 0 byte; a copy whose length is cut off copies 2
// bytes); without it the incomplete token is dropped. Complete streams decode identically
// either way.
LzssResult lzss_decode(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out, size_t max_output,
                       bool original_quirks);

// Builds the decompressed image of a level file held in memory, as the original decompress()
// did: the first kB3dRawHeaderSize bytes are copied, the byte at kB3dCompressedFlagOffset is
// cleared, and everything after the raw header is LZSS-decoded (whatever the flag says).
// `name` (the file name) is used in messages. Returns true for a complete stream. On false,
// *err says why and *result (if given) says how decoding ended: after LzssEnd::InputEnd `out`
// holds everything that could be decoded; after a file shorter than the raw header `out` is
// empty. Bytes after the end marker are ignored (result->consumed tells how many were used).
bool b3d_decompress(const uint8_t* in, size_t in_size, const std::string& name, std::vector<uint8_t>& out,
                    bool original_quirks, LzssResult* result, std::string* err);

// Reads a whole level file into memory; refuses files larger than kB3dMaxImageSize.
bool b3d_read_file(const std::string& path, std::vector<uint8_t>& out, std::string* err);

// Decompresses a TPM level file to out_path. The output is byte-identical to the original
// decompress() (raw header copied, byte 0x328 cleared, LZSS-decoded remainder), including the
// original's bytes for a truncated stream. Returns false and sets *err if a file cannot be read
// or written, the input is shorter than the raw header or larger than kB3dMaxImageSize, the
// output would exceed kB3dMaxImageSize, or the stream ends without its end marker (truncated
// input: the decoded part is still written, as the original wrote it).
bool b3d_decompress_file(const std::string& in_path, const std::string& out_path, std::string* err);

} // namespace tpmx
