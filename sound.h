// TPMExtractor fork - TPM's voice files (INDYWV), decoded to WAV.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// The voice files code their samples with VIMA ADPCM (vima.h). The layout below was found by
// static analysis of the game's code (no game code was run) and checked on every voice file of a
// retail copy. All multi-byte INDYWV fields are little-endian.
//
// Voice file (VOICE.LAB entries *.WV):
//   +0x00 "INDYWV", +0x06 u32 sample rate, +0x0A u32 bits per sample (16), +0x0E u32 channels,
//   +0x12 u32 data size (the bytes after the sync block; the game does not use it),
//   +0x16 u32 sync size, +0x1A sync block, then u32 PCM byte count and one VIMA stream
//   (vima.h; the voice decoder's header predictor, VimaPredictor::Quirk) to the end of the file.

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

const uint64_t kMaxSoundFileSize = 32u * 1024 * 1024;  // a larger voice file is refused
const uint32_t kMaxSampleRate = 384000;
const size_t kWavHeaderBytes = 44;

// The canonical 44-byte header of a 16-bit PCM WAV: "RIFF", size, "WAVE", a 16-byte "fmt "
// chunk (PCM, channels, rate, rate * channels * 2, channels * 2, 16 bits) and the "data" chunk
// header, the data following directly (retail Jedi Knight's loader expects "data" right after
// "fmt ").
std::vector<uint8_t> wav_header(uint32_t rate, int channels, uint32_t data_bytes);

// Writes header + samples (16-bit little-endian) to `path`; a partly written file is removed.
bool write_wav(const std::string& path, uint32_t rate, int channels, const std::vector<int16_t>& samples,
               std::string* err);

// The name of a sound output file: `file_name` without its extension (from the last '.') plus
// `suffix` ("x.WV" -> "x" + ".wav").
std::string sound_output_name(const std::string& file_name, const char* suffix);

// ---------------------------------------------------------------------------------- voice

struct WvSound
{
    uint32_t rate = 0;
    int channels = 0;
    uint32_t pcm_bytes = 0;        // as the file says
    bool wvsm = false;             // coded with WVSM blocks instead of ADPCM
    unsigned escapes = 0, clamps = 0;
    std::vector<int16_t> samples;  // interleaved, whole frames
};

// Decodes the voice file in data[0..size), which starts at file offset `base` of the file `name`.
// Warnings are logged as "<name> @0x<offset>: <what><message>" (`what` names a LAB entry, or
// is empty). Throws ParseError (with `name` and the absolute offset) when the file cannot be
// decoded.
WvSound wv_decode(const uint8_t* data, size_t size, const std::string& name, uint64_t base, const std::string& what);

// wv_decode, then write_wav to `out_path`. Logs errors as above; returns false after one (no
// file is written then).
bool wv_write_wav(const uint8_t* data, size_t size, const std::string& name, uint64_t base, const std::string& what,
                  const std::string& out_path);

// The `wv` command for one file: <out_dir>/<file_name without extension>.wav (file_name: the
// name the outputs are named after, e.g. as typed).
bool process_wv(const std::string& wv_path, const std::string& file_name, const std::string& out_dir,
                const Options& opts);

} // namespace tpmx
