#include "unshieldv3.h"
#include "ISArchiveV3.h"
#include <iostream>

namespace fs = std::filesystem;
using namespace std;


// Strike: this function taken from unshieldv3's main.cpp

bool extract(const char* _archive, const char* _destination) {
    ISArchiveV3 archive(_archive);
    const fs::path& destination = _destination;

    if (destination.empty()) {
        cerr << "Please specify a destination directory." << endl;
        return false;
    }
    if (!fs::exists(destination)) {
        cerr << "Destination directory not found: " << destination << endl;
        return false;
    }
    for (auto& file : archive.files()) {
        cout << file.full_path << endl;
        cout << "      Compressed size: " << setw(10) << file.compressed_size << endl;
        auto contents = archive.decompress(file.full_path);
        cout << "    Uncompressed size: " << setw(10) << contents.size() << endl;

        fs::path dest = destination / file.path();
        fs::path dest_dir = dest.parent_path();
        if (!fs::create_directories(dest_dir)) {
            if (!fs::exists(dest_dir)) {
                cerr << "Could not create directory: " << dest_dir << endl;
                return false;
            }
        }
        ofstream fout(dest, ios::binary | ios::out);
        if (fout.fail()) {
            cerr << dest << endl;
            cerr << "Could not create file: " << dest << endl;
            return false;
        }
        fout.write(reinterpret_cast<char*>(contents.data()), long(contents.size()));
        if (fout.fail()) {
            cerr << "Could not write to: " << dest << endl;
            return false;
        }
        fout.close();
    }
    return true;
}