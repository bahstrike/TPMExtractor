// TPMExtractor fork - a driver for the fuzz targets when libFuzzer is not available.
// Copyright 2026 the TPMExtractor fork contributors. Licensed under the Apache License,
// Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
//
// Usage: fuzz_<name> [-mutations N] [-truncations T] [-seed S] FILE_OR_FOLDER...
// Runs the target on every given file (a folder: every file in it), on T truncations of each
// (default 64), and on N seeded random mutations of each (default 200: bytes overwritten,
// inserted, removed, and 32-bit fields set to extreme values). Pair it with -DTPMX_SANITIZE=ON.
// Exit 0 when every run returned.

#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

void run(const std::vector<uint8_t>& d)
{
    // a copy of exactly the right size, so that ASan sees every read past the end
    std::vector<uint8_t> copy(d);
    LLVMFuzzerTestOneInput(copy.empty() ? nullptr : copy.data(), copy.size());
}

void mutate_and_run(const std::vector<uint8_t>& base, std::mt19937& rng, int mutations, int truncations)
{
    run(base);
    for (int k = 1; k <= truncations && !base.empty(); k++)
        run(std::vector<uint8_t>(base.begin(), base.begin() + (long)(base.size() * k / (truncations + 1))));
    static const uint32_t extremes[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x10000, 0x100};
    for (int i = 0; i < mutations; i++) {
        std::vector<uint8_t> m = base;
        const int edits = 1 + (int)(rng() % 4);
        for (int e = 0; e < edits; e++) {
            const uint32_t what = rng() % 4;
            if (m.empty() || what == 0) {
                m.insert(m.begin() + (long)(m.empty() ? 0 : rng() % m.size()), (uint8_t)rng());
            } else if (what == 1) {
                m[rng() % m.size()] = (uint8_t)rng();
            } else if (what == 2 && m.size() > 1) {
                m.erase(m.begin() + (long)(rng() % m.size()));
            } else if (m.size() >= 4) {
                const size_t at = (rng() % (m.size() - 3)) & ~(size_t)3;
                const uint32_t v = extremes[rng() % (sizeof extremes / sizeof *extremes)];
                std::memcpy(&m[at], &v, 4);
            }
        }
        run(m);
    }
}

} // namespace

int main(int argc, char** argv)
{
    int mutations = 200;
    int truncations = 64;
    unsigned seed = 1;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-mutations") && i + 1 < argc)
            mutations = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-truncations") && i + 1 < argc)
            truncations = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-seed") && i + 1 < argc)
            seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else
            inputs.push_back(argv[i]);
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: %s [-mutations N] [-truncations T] [-seed S] FILE_OR_FOLDER...\n", argv[0]);
        return 2;
    }
    std::mt19937 rng(seed);
    size_t files = 0;
    for (const std::string& in : inputs) {
        std::vector<std::string> paths;
        if (tpmx::platform::is_dir(in)) {
            for (const tpmx::platform::DirEntry& e : tpmx::platform::list_dir(in, "*"))
                if (!e.is_dir)
                    paths.push_back(tpmx::platform::path_join(in, e.name));
        } else {
            paths.push_back(in);
        }
        for (const std::string& p : paths) {
            std::vector<uint8_t> d;
            if (!tpmx::platform::read_file(p, d)) {
                std::fprintf(stderr, "cannot read %s\n", p.c_str());
                return 2;
            }
            mutate_and_run(d, rng, mutations, truncations);
            files++;
        }
    }
    std::printf("%zu input(s), %d mutation(s) each: no crash\n", files, mutations);
    return 0;
}
