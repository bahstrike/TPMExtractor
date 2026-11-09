// TPMExtractor fork - the music tables of TPM's IMUSE.DLL, read from the file as data.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// The game chooses music by number: its level data asks the music DLL for a state (an area's
// loop) or a sequence (a cue) by id, and the DLL's compiled-in tables map each id to a key and
// each key to a music file. This module finds those tables in the DLL file by their structure
// (it never loads or runs the DLL) and writes them as text, so that a level's music ids can be
// resolved to files. Found by static analysis of the game's code.
//
// The tables lie in an initialized-data section of the DLL, one after another:
//   states     76-byte records {char key[32]; u32 transition; u32 id; char symbol[24];
//              u32 attribute; u32 hook; u32 fade}, the first being the null state (empty key,
//              transition 0, id 1000), then zero padding up to
//   sequences  records of the same layout, the first being the null sequence (id 2000), directly
//              followed by
//   files      64-byte records {char key[32]; char file[32]}, ended by a record with an empty key.
// Strings are NUL-terminated and zero-padded. Transition 0 is none, 3 a crossfade into the file,
// 4 a crossfade and a return to the state at the file's "_end" marker. The engine looks a key up
// in the file table byte for byte, the first match winning; a state or sequence id is looked up
// the same way, so an id that occurs twice resolves to its first record.

#pragma once

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// The record counts of the retail DLL (the engine's loops stop at fixed table ends); other
// counts are refused.
const unsigned kImuseStates = 87;
const unsigned kImuseSequences = 82;
const unsigned kImuseFiles = 167;
const uint32_t kImuseNullState = 1000;
const uint32_t kImuseNullSequence = 2000;
const size_t kImuseRecordBytes = 76;
const size_t kImuseFileRecordBytes = 64;
const uint64_t kMaxDllSize = 64u * 1024 * 1024;

struct ImuseRecord
{
    std::string key, symbol;
    uint32_t transition = 0, id = 0, attribute = 0, hook = 0, fade = 0;
};

struct ImuseFile
{
    std::string key, file;
};

struct ImuseTables
{
    std::vector<ImuseRecord> states, sequences;
    std::vector<ImuseFile> files;
    uint64_t state_offset = 0, sequence_offset = 0, file_offset = 0;  // file offsets of the tables
    uint64_t state_address = 0, sequence_address = 0, file_address = 0;  // their addresses in the image
};

// Finds and reads the tables in the DLL file data[0..size) named `name`. Throws ParseError
// (with the file offset) when the file is not a PE file, the tables are not found exactly once,
// a record inside them is malformed, or a table's record count differs from the retail DLL's.
ImuseTables imuse_read_tables(const uint8_t* data, size_t size, const std::string& name);

// The index of the file record the engine plays for `key`: the first whose key equals it byte for
// byte; -1 for an empty key or none.
int imuse_find_file(const ImuseTables& t, const std::string& key);

// The tables as text, tab-separated, strings quoted with bytes other than printable ASCII, '"'
// and '\' as \xNN:
//   state     <index>  <id>  <transition>  <attribute>  <hook>  <fade>  "<key>"  "<symbol>"  <file>
//   sequence  (the same)
//   file      <index>  "<key>"  "<file>"
// where <file> is "<file name>" of the record imuse_find_file gives, or - for none. Lines end
// with "\n" (files get "\r\n").
std::string imuse_tables_text(const ImuseTables& t, const std::string& file_label);

// The `imuse` command: <out_dir>/imuse_tables.txt. Nothing is written when the tables are not
// found.
bool process_imuse(const std::string& dll_path, const std::string& out_dir, const Options& opts);

} // namespace tpmx
