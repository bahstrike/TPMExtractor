/* unshieldv3 -- extract InstallShield V3 archives.
Copyright (c) 2019 Wolfgang Frisch <wfrisch@riseup.net>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

// Modified 2026 in the TPMExtractor fork (see CHANGES.md): a malformed archive makes the
// constructor throw std::runtime_error (with the file offset) instead of failing an assert()
// or reading past the data: the three header checks, short reads in read(), readString8() and
// readString16(), and table chunks shorter than their fields or cut off by the end of the file.
// Valid archives are read exactly as before. Changed lines are marked "(fork)". decompress() is
// unchanged; the fork does not call it (zextract.cpp decompresses with blast() itself).

#include "ISArchiveV3.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <exception>
extern "C" {
    #include "blast.h"
};

namespace fs = std::filesystem;


class Directory {
public:
    std::string name;
    uint16_t file_count;
};

// (fork) Reports a malformed archive.
[[noreturn]] static void corrupt(const std::string& what, std::streamoff at) {
    std::ostringstream os;
    os << what << " at offset 0x" << std::hex << std::uppercase << at;
    throw std::runtime_error(os.str());
}

// (fork) Skips the rest of a table chunk after the `used` bytes of its fields (upstream:
// fin.ignore(chunk_size - used), which silently skipped nothing when the chunk was too short).
static void skipChunkRest(std::ifstream& fin, uint16_t chunk_size, size_t used) {
    const std::streamoff at = fin.tellg();
    if (chunk_size < used)
        corrupt("Table of contents entry shorter than its fields", at);
    const std::streamsize n = std::streamsize(chunk_size - used);
    fin.ignore(n);
    if (fin.gcount() != n)
        corrupt("Unexpected end of archive", at);
}


ISArchiveV3::ISArchiveV3(const std::filesystem::path& apath)
    : m_path(apath)
{
    std::vector<Directory> directories;

    fin.open(apath, std::ios::in | std::ios::binary);
    if (!fin.is_open()) {
        std::ostringstream os;
        os << "Cannot open archive: " << apath;
        throw std::runtime_error(os.str());
    }
    uint64_t file_size = fs::file_size(apath);
    if (file_size <= sizeof(Header))  // (fork) was assert()
        throw std::runtime_error("Not an InstallShield v3 archive (smaller than its header)");
    fin.read(reinterpret_cast<char*>(&hdr), sizeof(Header));
    if (!fin || hdr.signature1 != 0x8C655D13 || hdr.signature2 != 0x02013a)  // (fork) was assert()
        throw std::runtime_error("Not an InstallShield v3 archive (no signature at offset 0)");
    if (hdr.toc_address >= file_size)  // (fork) was assert()
        corrupt("Table of contents offset beyond the end of the archive, header field", 0x29);
    fin.seekg(hdr.toc_address, std::ios::beg);

    for (int i = 0; i < hdr.dir_count; i++) {
        uint16_t file_count = read<uint16_t>();
        uint16_t chunk_size = read<uint16_t>();
        std::string name = readString16();
        skipChunkRest(fin, chunk_size, name.length() + 6);  // (fork)
        directories.push_back({name, file_count});
    }

    for (Directory& directory : directories) {
        for (int i = 0; i < directory.file_count; i++) {
            File f;

            f.volume_end = read<uint8_t>();
            f.index = read<uint16_t>();
            f.uncompressed_size = read<uint32_t>();
            f.compressed_size = read<uint32_t>();
            f.offset = read<uint32_t>();
            f.datetime = read<uint32_t>();
            uint32_t u2 = read<uint32_t>();
            (void)u2;
            uint16_t chunk_size = read<uint16_t>();
            f.attrib = read<uint8_t>();
            f.is_split = read<uint8_t>();
            uint8_t u3 = read<uint8_t>();
            (void)u3;
            f.volume_start = read<uint8_t>();
            f.name = readString8();
            skipChunkRest(fin, chunk_size, f.name.length() + 30);  // (fork)

            if (directory.name.length()) {
                f.full_path = directory.name + "\\" + f.name;
            } else {
                f.full_path = f.name;
            }
            if (!isValidName(f.full_path)) {
                throw std::runtime_error(std::string("Invalid file path: ") + f.full_path);
            }

            m_files.push_back(f);
        }
    }
}

std::tm ISArchiveV3::File::tm() const {
    // source: https://github.com/lephilousophe/idecomp
    uint16_t file_date = datetime & 0xffff;
    uint16_t file_time = (datetime >> 16) & 0xffff;
    std::tm tm = { /* .tm_sec  = */ (file_time & 0x1f) * 2,
                   /* .tm_min  = */ (file_time >> 5) & 0x3f,
                   /* .tm_hour = */ (file_time >> 11) & 0x1f,
                   /* .tm_mday = */ (file_date) & 0x1f,
                   /* .tm_mon  = */ ((file_date >> 5) & 0xf) - 1,
                   /* .tm_year = */ (((file_date >> 9) & 0x7f) + 1980) - 1900,
                 };
    tm.tm_isdst = -1;
    return tm;
}

std::filesystem::path ISArchiveV3::File::path() const {
    std::string fp = full_path;
    std::replace(fp.begin(), fp.end(),
        '\\', '/');//fs::path::preferred_separator);
    return std::filesystem::path(fp);
}

std::string ISArchiveV3::File::attribString() const {
    bool ro = attrib & File::Attributes::READONLY;
    bool hidden = attrib & File::Attributes::HIDDEN;
    bool system = attrib & File::Attributes::SYSTEM;
    bool archive = attrib & File::Attributes::ARCHIVE;
    std::ostringstream os;
    os << (archive ? 'A': '_');
    os << (hidden ? 'H': '_');
    os << (ro ? 'R': '_');
    os << (system ? 'S': '_');
    return os.str();
}

const std::vector<ISArchiveV3::File>& ISArchiveV3::files() const {
    return m_files;
}

const ISArchiveV3::File* ISArchiveV3::fileByPath(const std::string& full_path) const {
    // std::map would be more efficient but let's keep it simple for now.
    for (const auto& file : m_files) {
        if (file.full_path == full_path) {
            return &file;
        }
    }
    return nullptr;
}

bool ISArchiveV3::exists(const std::string& full_path) const {
    return fileByPath(full_path) != nullptr;
}

unsigned _blast_in(void *how, unsigned char **buf) {
    std::vector<unsigned char> *inbuf = reinterpret_cast<std::vector<unsigned char>*>(how);
    *buf = inbuf->data();
    return unsigned(inbuf->size());
}

int _blast_out(void *how, unsigned char *buf, unsigned len) {
    std::vector<unsigned char> *outbuf = reinterpret_cast<std::vector<unsigned char>*>(how);
    outbuf->insert(outbuf->end(), &buf[0], &buf[len]);
    return false; // would indicate write error
}

std::vector<uint8_t> ISArchiveV3::decompress(const std::string& full_path) {
    if (!exists(full_path)) {
        std::ostringstream os;
        os << "decompress() called with invalid path: " << full_path;
        throw std::runtime_error(os.str());
    }
    const File* file = fileByPath(full_path);
    assert(file != nullptr);
    fin.seekg(file->offset, std::ios::beg);
    std::vector<unsigned char> buf(file->compressed_size);
    std::vector<unsigned char> out;
    fin.read(reinterpret_cast<char*>(&buf[0]), file->compressed_size);
    if (fin.fail()) {
        throw std::runtime_error("Read failed");
    }

    if (file->attrib & File::Attributes::UNCOMPRESSED) {
        return buf;
    }

    int ret;
    unsigned left = 0;
    ret = blast(_blast_in, static_cast<void*>(&buf), _blast_out, static_cast<void*>(&out), &left, nullptr);
    if (ret != 0) {
        std::ostringstream os;
        os << "Blast decompression error: " << ret;
        throw std::runtime_error(os.str());
        return {};
    }

    return out;
}

template<class T> T ISArchiveV3::read() {
    T re;
    const std::streamoff at = fin.tellg();  // (fork)
    fin.read(reinterpret_cast<char*>(&re), sizeof(re));
    if (!fin)  // (fork) short read
        corrupt("Unexpected end of archive", at);
    return re;
}

std::string ISArchiveV3::readString8() {
    uint8_t len = read<uint8_t>();
    if (len == 0) return "";  // (fork) as readString16: &buf[0] of an empty vector is undefined
    std::vector<char> buf(len);
    const std::streamoff at = fin.tellg();  // (fork)
    fin.read(&buf[0], len);
    if (!fin)  // (fork) short read
        corrupt("Unexpected end of archive", at);
    return std::string(buf.begin(), buf.end());
}

std::string ISArchiveV3::readString16() {
    uint16_t len = read<uint16_t>();
    if (len == 0) return "";  // Strike: fix crash on empty string
    std::vector<char> buf(len);
    const std::streamoff at = fin.tellg();  // (fork)
    fin.read(&buf[0], len);
    if (!fin)  // (fork) short read
        corrupt("Unexpected end of archive", at);
    return std::string(buf.begin(), buf.end());
}

bool ISArchiveV3::isValidName(const std::string& name) const {
    if (name.find("..\\") != std::string::npos) {
        return false;
    }
    if (name.find("../") != std::string::npos) {
        return false;
    }
    return true;
}
