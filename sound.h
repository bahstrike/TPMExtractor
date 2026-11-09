// TPMExtractor fork - TPM's voice files (INDYWV) and music files (MCMP with an iMUS map), decoded
// to WAV, and the music file's map as text.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Both formats code their samples with VIMA ADPCM (vima.h). The layouts below were found by
// static analysis of the game's code (no game code was run) and checked on every voice file and
// music track of a retail copy. All multi-byte INDYWV fields are little-endian; all MCMP and iMUS
// fields are big-endian, except the iMUS size word (unused by the game).
//
// Voice file (VOICE.LAB entries *.WV):
//   +0x00 "INDYWV", +0x06 u32 sample rate, +0x0A u32 bits per sample (16), +0x0E u32 channels,
//   +0x12 u32 data size (the bytes after the sync block; the game does not use it),
//   +0x16 u32 sync size, +0x1A sync block, then u32 PCM byte count and one VIMA stream
//   (vima.h; the voice decoder's header predictor, VimaPredictor::Quirk) to the end of the file.
//
// Music file (*.IMC):
//   "MCMP", u16 block count n, n x {u8 codec, u32 decoded size, u32 stored size}, u16 size of the
//   codec name table, the codec names (NUL-terminated; a block's codec is an index into them:
//   "NULL" or "NONE" stores the block as it is, "VIMA" codes it), then the stored blocks back to
//   back. The decoded blocks form one stream: the raw blocks first, holding the iMUS header, then
//   the VIMA blocks, holding the audio (each VIMA block starts with its own stream header, read
//   with VimaPredictor::Clean). The iMUS header: "iMUS", u32 (unused), "MAP ", u32 map size,
//   the map's events, "DATA", u32 audio byte count; the audio starts at map size + 24.
//   Map events are {char tag[4], u32 size, body}; every body starts with the event's position, a
//   byte position in the decoded stream:
//     FRMT  position, byte-order flag (non-zero: little-endian samples), bits, rate, channels
//     TEXT  position, NUL-terminated text (a marker: the game's music script reacts to "_end";
//           "lp" names the loop point). The game walks a TEXT body by its NUL, not its size.
//     REGN  position, length in bytes: a region the engine plays
//     JUMP  position, destination, hook, crossfade in milliseconds: where playback continues
//           after the region that ends at the position (the loop)
//     STOP  position: the end of the track
//     SYNC  position, data
//   The game requires FRMT as the first event, at the start of the audio, and a map of at most
//   0x2000 - 8 bytes.
//
// Checks beyond what the game does (each failure is an error naming the file and offset):
// the block sizes add up to the file size, the raw blocks are stored as they are and come first
// and hold exactly the iMUS header, the decoded audio is exactly the DATA size and whole frames,
// every block has the FRMT's channel count, event positions lie inside the audio on whole frames
// and never go backwards, regions end inside the audio, every JUMP lands where a region starts
// and, when the map names a loop point ("lp"), on the loop point, and every VIMA block's codes
// fit in its stored bytes.

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

const uint64_t kMaxSoundFileSize = 32u * 1024 * 1024;  // a larger voice or music file is refused
const uint32_t kMaxSampleRate = 384000;
const uint32_t kImusMaxMapBytes = 0x2000 - 8;  // the game's limit (map size + 8 <= 0x2000)
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

// ---------------------------------------------------------------------------------- music

struct ImusEvent
{
    std::string tag;          // FRMT, TEXT, REGN, JUMP, STOP or SYNC
    uint64_t offset = 0;      // file offset of the event
    uint32_t pos = 0;         // byte position in the decoded stream
    uint32_t values[4] = {0, 0, 0, 0};  // FRMT: flag, bits, rate, channels; REGN: length;
                                        // JUMP: destination, hook, fade (ms)
    std::string text;         // TEXT
    uint32_t data_bytes = 0;  // SYNC: body bytes after the position
};

struct ImcTrack
{
    uint32_t header_bytes = 0;  // the iMUS header = where the audio starts in the stream
    uint32_t data_bytes = 0;    // the DATA size
    uint32_t rate = 0;
    int channels = 0;
    int frame_bytes = 0;
    uint32_t blocks = 0;
    std::vector<ImusEvent> events;
    std::vector<int16_t> samples;  // the decoded audio, interleaved
    unsigned escapes = 0, clamps = 0;
};

// Decodes the music file in data[0..size) (the whole file `name`). Warnings are logged with file
// and offset. Throws ParseError when the file cannot be decoded or fails a check (see the top).
ImcTrack imc_decode(const uint8_t* data, size_t size, const std::string& name);

// The map as text: a comment naming `file_label`, a "format" line, then one line per event, its
// position as a frame of the audio ((position - header bytes) / frame bytes), tab-separated:
//   FRMT  <frame>  flag=<n>  bits=<n>  rate=<n>  channels=<n>
//   TEXT  <frame>  text="<text>"          (bytes other than printable ASCII, '"' and '\' as \xNN)
//   REGN  <frame>  frames=<length in frames>
//   JUMP  <frame>  dest=<frame>  hook=<n>  fade_ms=<n>
//   STOP  <frame>
//   SYNC  <frame>  bytes=<n>
// Lines end with "\n" here; files get "\r\n" (TextWriter).
std::string imc_map_text(const ImcTrack& t, const std::string& file_label);

// The `imc` command for one file: <out_dir>/<name>.wav and <out_dir>/<name>.imap.txt, <name> being
// file_name without its extension. Nothing is written for a file that fails.
bool process_imc(const std::string& imc_path, const std::string& file_name, const std::string& out_dir,
                 const Options& opts);

// Control and non-ASCII bytes, '"' and '\' as \xNN (for names and texts in messages and files).
std::string sound_printable(const std::string& s);

} // namespace tpmx
