// TPMExtractor fork - the VIMA ADPCM codec of TPM's voice files and music blocks.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Both of TPM's audio formats code their samples with the same variable-width IMA-style ADPCM,
// known as VIMA: the voice files (INDYWV, decoded by the game executable) and the music blocks
// (MCMP, decoded by the music DLL). The two copies of the decoder in the game differ in one
// detail only, the way a stream header's 16-bit predictor is read (VimaPredictor). This decoder
// follows both, found by static analysis of the game's code; no game code was run.
//
// A stream is:
//   header   u8 index of channel 0; when its top bit is set the stream is stereo and the index
//            is the byte's complement. Then the u16 big-endian predictor of channel 0, and for a
//            stereo stream channel 1's u8 index and u16 predictor (3 or 6 bytes in all).
//            A stereo header with predictors 0x1111 and 0x2222, channel-1 index 100, followed
//            by "WVSM", selects the WVSM block codec instead (vima_decode_body).
//   codes    one bitstream, read most significant bit first: every sample of channel 0, then
//            every sample of channel 1. A code has WIDTH[index] bits (4 to 7) including its
//            sign bit; the all-ones magnitude is an escape followed by the sample's 16 raw bits.
//            Other codes add or subtract a delta from the step table, clamped to 16 bits.
// Output samples are interleaved little-endian int16.

#pragma once

#include "binreader.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tpmx {

const int kVimaSteps = 89;                // step indices 0..88
const int kVimaMaxIndex = kVimaSteps - 1;
const size_t kVimaWvsmBlockBytes = 0x1000;  // output bytes per WVSM block

// The step table (the standard IMA ADPCM table) and the code width of each step index.
extern const int16_t kVimaStep[kVimaSteps];
extern const uint8_t kVimaWidth[kVimaSteps];

// The step index change for a code of `width` bits (2..7) with magnitude `magnitude`
// (0 .. 2^(width-1) - 1). The all-ones magnitude, the escape, always adds 6.
int vima_index_adjust(int width, int magnitude);

// Entry index * 64 + i6 of the 89 x 64 table the decoders build at start-up: the sum, over the
// set bits of i6 from 0x20 down to 0x01, of the step shifted right by 0, 1, ... 5 (as an
// unsigned 16-bit value).
uint16_t vima_delta_table(int index, int i6);

// The amount a non-escape code adds or subtracts: delta_table(index, magnitude << (7 - width)),
// plus step >> (width - 1) when the magnitude is not zero.
int vima_delta(int index, int width, int magnitude);

// How a stream header's 16-bit predictor is read.
enum class VimaPredictor {
    Quirk,  // the voice decoder: (byte0 << 8) | sign-extended byte1, kept to 16 bits, so a
            // second byte of 0x80 or more makes the high byte 0xFF
    Clean,  // the music decoder: a plain big-endian 16-bit value
};

// The predictor `p` reads from the two header bytes b0, b1 (as a signed 16-bit value).
int vima_header_predictor(uint8_t b0, uint8_t b1, VimaPredictor p);

// Channels of a stream whose header starts with `first_byte`: 2 when its top bit is set.
inline int vima_stream_channels(uint8_t first_byte) { return (first_byte & 0x80) ? 2 : 1; }

struct VimaHeader
{
    int channels = 1;
    int index[2] = {0, 0};     // initial step index per channel
    int predictor[2] = {0, 0}; // initial sample per channel
    bool wvsm = false;         // the WVSM signature: WVSM blocks follow
    size_t size = 0;           // header bytes: 3 or 6, plus 4 for the "WVSM" signature
    uint64_t offset = 0;       // file offset of the header's first byte
};

// Reads a stream header at r's position and advances past it (and past "WVSM" when it is
// there). Throws ParseError for a truncated header, and, unless the stream is WVSM, for an
// initial step index outside 0..88 (the game's decoders do not check it and index past their
// tables), at the offset of that index byte.
VimaHeader vima_read_header(BinReader& r, VimaPredictor p);

// The whole frames `pcm_bytes` output bytes hold: pcm_bytes / (2 * channels). The game's
// decoders produce exactly these (ADPCM) or a few bytes more (WVSM); bytes past the last whole
// frame are never written to the WAV.
uint64_t vima_frames(const VimaHeader& h, uint64_t pcm_bytes);

// The samples (all channels together) decoding `pcm_bytes` output bytes reads codes for, and
// the most samples `coded_bytes` bytes after the header can hold (every code takes at least 4
// bits, every WVSM sample at least one byte). A caller refuses a count whose samples cannot fit
// before it allocates anything.
uint64_t vima_samples_requested(const VimaHeader& h, uint64_t pcm_bytes);
uint64_t vima_max_samples(const VimaHeader& h, uint64_t coded_bytes);

struct VimaResult
{
    uint64_t code_bits = 0;  // ADPCM: sum of the code widths plus 16 per escape
    size_t coded_bytes = 0;  // bytes the codes occupy: ceil(code_bits / 8), or the WVSM bytes read
    unsigned escapes = 0;
    unsigned clamps = 0;
};

// Decodes the codes after a header read by vima_read_header, from r's position to its end, for
// `pcm_bytes` output bytes, into `out`: vima_frames(h, pcm_bytes) interleaved frames of
// h.channels samples (out is resized). As the game's decoders do, the bit reader looks up to two
// bytes ahead of the bits it uses and past the end of the data reads zeros; a stream whose codes
// need more bits than the data holds is refused with a ParseError at the end of the data. WVSM
// streams are decoded in blocks of 0x1000 output bytes as the game does (a block's last pair of
// samples may run past the bytes asked for), and must not read past the end of the data either.
// Advances r past the bytes the codes occupy.
VimaResult vima_decode_body(BinReader& r, const VimaHeader& h, uint64_t pcm_bytes, std::vector<int16_t>& out);

} // namespace tpmx
