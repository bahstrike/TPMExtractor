// TPMExtractor fork - InstallShield v3 (.Z) extraction glue around unshieldv3.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).

#pragma once

#include "options.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tpmx {

// Extracts every file of the InstallShield v3 archive `z_path` into `out_dir`, which must exist.
//   - The table of contents is read by unshieldv3's ISArchiveV3, which throws on a malformed
//     archive; that error is reported and nothing is extracted.
//   - Each file's data is decompressed here with unshieldv3's blast() (PKWARE DCL "implode"),
//     streamed from the archive to the output file in chunks, so memory use does not grow with
//     the file size. Files with attribute 0x10 are stored and copied as they are.
//   - Archive paths ("dir\\name") become out_dir/dir/name; every path goes through
//     platform::safe_output_path (subdirectories allowed) and missing directories are created.
//   - A file is accepted only if its data lies inside the archive and decompresses without
//     error to exactly the size the table of contents records. A file that fails is reported
//     with the archive name and its data offset, its partial output is removed, and the next
//     file is extracted. Files split across volumes are reported and skipped.
//   - Prints the same per-file lines as unshieldv3's extract() (name, compressed size,
//     uncompressed size).
// Appends the paths written to *written (if given). Returns true only if every file was written.
bool extract_z(const std::string& z_path, const std::string& out_dir, const Options& opts,
               std::vector<std::string>* written = nullptr);

} // namespace tpmx
