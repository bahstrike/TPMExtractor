// Code provided by shiny
// Modified 2026 in the TPMExtractor fork (see CHANGES.md): the decoder works on memory buffers:
// bounds-checked bit reads, input and output size limits, and an explicit result (end marker,
// truncated input, output limit) instead of silently stopping. It reproduces the bit order, ring
// and token rules of shiny's port of the game's reader byte for byte, including what that code
// wrote for a token cut off by the end of the file (see lzss_decode).

#include "b3d_decompress.h"

#include "platform.h"
#include "textout.h"

#include <cstdint>
#include <vector>

namespace tpmx {

namespace {

const unsigned kRingSize = 0x1000;       // 4 KiB window
const unsigned kRingMask = kRingSize - 1;
const unsigned kRingStart = 1;           // first write position (the ring starts zeroed)
const int kLiteralBits = 8;
const int kOffsetBits = 12;
const int kLengthBits = 4;
const unsigned kMinCopy = 2;             // a copy of length field L writes L + 2 bytes

// Most-significant-bit-first reader with the original reader's state: `mask_` is the next bit
// to take from `cur_`, and 0x80 means the next bit needs a new input byte. The original reader
// fetched a byte only when it needed its first bit, so a value cut off by the end of the input
// leaves the reader waiting for a byte; bit() then fails for good.
class BitReader
{
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // One bit into b; false if a new byte is needed and the input has none left.
    bool bit(unsigned& b)
    {
        if (mask_ == 0x80) {
            if (pos_ >= size_)
                return false;
            cur_ = data_[pos_++];
        }
        b = (cur_ & mask_) ? 1u : 0u;
        mask_ = (uint8_t)(mask_ >> 1);
        if (!mask_)
            mask_ = 0x80;
        return true;
    }

    // An n-bit value, most significant bit first; false if the input ran out first.
    bool bits(int n, unsigned& v)
    {
        v = 0;
        for (int i = n - 1; i >= 0; i--) {
            unsigned b = 0;
            if (!bit(b))
                return false;
            v |= b << i;
        }
        return true;
    }

    size_t consumed() const { return pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint8_t cur_ = 0;
    uint8_t mask_ = 0x80;
};

std::string mib(size_t bytes) { return format("%zu MiB", bytes >> 20); }

} // namespace

LzssResult lzss_decode(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out, size_t max_output,
                       bool original_quirks)
{
    LzssResult r;
    const size_t start = out.size();
    uint8_t ring[kRingSize] = {};
    unsigned wpos = kRingStart;
    BitReader br(in, in_size);

    // Appends one byte to the output and the ring; false once max_output bytes were produced.
    auto put = [&](uint8_t c) -> bool {
        if (out.size() - start >= max_output)
            return false;
        out.push_back(c);
        ring[wpos] = c;
        wpos = (wpos + 1) & kRingMask;
        return true;
    };

    for (;;) {
        unsigned flag = 0;
        if (!br.bit(flag)) {
            r.end = LzssEnd::InputEnd;
            break;
        }
        if (flag) {
            unsigned c = 0;
            if (!br.bits(kLiteralBits, c)) {
                // The original's bit reader returned 0 for a value it could not finish, so a
                // literal cut off by the end of the file became a 0 byte.
                if (original_quirks && !put(0)) {
                    r.end = LzssEnd::OutputLimit;
                    break;
                }
                r.end = LzssEnd::InputEnd;
                break;
            }
            if (!put((uint8_t)c)) {
                r.end = LzssEnd::OutputLimit;
                break;
            }
            continue;
        }

        unsigned offset = 0;
        if (!br.bits(kOffsetBits, offset)) {
            // The original read the cut-off offset as 0, the end marker: nothing more is written.
            r.end = LzssEnd::InputEnd;
            break;
        }
        if (offset == 0) {
            r.end = LzssEnd::Marker;
            break;
        }
        unsigned length = 0;
        const bool have_length = br.bits(kLengthBits, length);
        if (!have_length) {
            if (!original_quirks) {
                r.end = LzssEnd::InputEnd;
                break;
            }
            length = 0;  // the original's value for a cut-off length: the copy still writes 2 bytes
        }
        bool room = true;
        // Byte by byte: a copy may read ring positions it has just written (overlapping runs).
        for (unsigned i = 0; i < length + kMinCopy && room; i++)
            room = put(ring[(offset + i) & kRingMask]);
        if (!room) {
            r.end = LzssEnd::OutputLimit;
            break;
        }
        if (!have_length) {
            r.end = LzssEnd::InputEnd;
            break;
        }
    }
    r.consumed = br.consumed();
    r.produced = out.size() - start;
    return r;
}

bool b3d_decompress(const uint8_t* in, size_t in_size, const std::string& name, std::vector<uint8_t>& out,
                    bool original_quirks, LzssResult* result, std::string* err)
{
    out.clear();
    if (result)
        *result = LzssResult();
    if (!in || in_size < kB3dRawHeaderSize) {
        if (err)
            *err = format("%s: file shorter than the 0x%zX-byte B3D header (%zu bytes)", name.c_str(),
                          kB3dRawHeaderSize, in ? in_size : (size_t)0);
        return false;
    }
    if (in_size > kB3dMaxImageSize) {
        if (err)
            *err = format("%s: %zu bytes, larger than the %s accepted for a level file", name.c_str(), in_size,
                          mib(kB3dMaxImageSize).c_str());
        return false;
    }

    // Retail levels expand 3.3x to 4.8x; reserve for 5x up to the limit.
    const size_t payload = in_size - kB3dRawHeaderSize;
    const size_t max_payload = kB3dMaxImageSize - kB3dRawHeaderSize;
    out.reserve(kB3dRawHeaderSize + (payload < max_payload / 5 ? payload * 5 : max_payload));
    out.assign(in, in + kB3dRawHeaderSize);
    out[kB3dCompressedFlagOffset] = 0;  // decompressed

    const LzssResult r = lzss_decode(in + kB3dRawHeaderSize, payload, out, max_payload, original_quirks);
    if (result)
        *result = r;
    switch (r.end) {
    case LzssEnd::Marker:
        return true;
    case LzssEnd::InputEnd:
        if (err)
            *err = format("%s @0x%zX: the compressed data ends without its end marker (truncated file?); "
                          "%zu bytes decoded",
                          name.c_str(), in_size, out.size());
        return false;
    case LzssEnd::OutputLimit:
        if (err)
            *err = format("%s @0x%zX: decompressed data exceeds the %s limit; not a level file?", name.c_str(),
                          kB3dRawHeaderSize + r.consumed, mib(kB3dMaxImageSize).c_str());
        return false;
    }
    return false;
}

bool b3d_read_file(const std::string& path, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    uint64_t size = 0;
    if (platform::file_size(path, size) && size > kB3dMaxImageSize) {
        if (err)
            *err = format("%s: %llu bytes, larger than the %s accepted for a level file", path.c_str(),
                          (unsigned long long)size, mib(kB3dMaxImageSize).c_str());
        return false;
    }
    if (!platform::read_file(path, out, err))
        return false;
    if (out.size() > kB3dMaxImageSize) {  // the file grew while it was read
        if (err)
            *err = format("%s: larger than the %s accepted for a level file", path.c_str(),
                          mib(kB3dMaxImageSize).c_str());
        out.clear();
        return false;
    }
    return true;
}

bool b3d_decompress_file(const std::string& in_path, const std::string& out_path, std::string* err)
{
    std::vector<uint8_t> in;
    if (!b3d_read_file(in_path, in, err))
        return false;

    std::vector<uint8_t> out;
    LzssResult r;
    std::string why;
    const bool complete = b3d_decompress(in.data(), in.size(), in_path, out, true, &r, &why);
    // Nothing is written for an input shorter than the raw header or for output over the limit;
    // a truncated stream is written as the original wrote it, then reported.
    if (!complete && (out.empty() || r.end == LzssEnd::OutputLimit)) {
        if (err)
            *err = why;
        return false;
    }
    if (!platform::write_file(out_path, out, err))
        return false;
    if (!complete) {
        if (err)
            *err = why;
        return false;
    }
    return true;
}

} // namespace tpmx
