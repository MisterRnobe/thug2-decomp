// Optional integration check against locally supplied game data. Not in CTest:
// the repository does not distribute game assets.
#include "thug2/assets.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace {
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::string alternate(std::string name) {
    for (char& c : name) {
        if (c >= 'a' && c <= 'z') c -= 'a'-'A';
        if (c == '\\') c = '/';
    }
    return name;
}
void compare(const thug2::Bytes& expected, const thug2::AssetFile& file, const std::string& name) {
    check(std::equal(expected.begin(), expected.end(), file.bytes().begin(), file.bytes().end()), "Data mismatch: " + name);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3) throw std::runtime_error("Usage: assets-corpus DATA_ROOT [REPORT.json]");
        auto root = std::filesystem::canonical(argv[1]);
        std::vector<std::filesystem::path> paths;
        for (const auto& e : std::filesystem::directory_iterator(root / "pre"))
            if (e.path().extension() == ".prx") paths.push_back(e.path());
        std::sort(paths.begin(), paths.end());
        check(!paths.empty(), "No PRX archives found");
        thug2::AssetStore store;
        std::vector<thug2::MountId> ids;
        for (const auto& path : paths) ids.push_back(store.mount_archive(path));
        auto disk = store.mount_directory(root);
        std::unordered_set<std::uint32_t> seen;
        std::size_t unique = 0, shadowed = 0, bytes = 0, loose = 0, loose_bytes = 0;
        for (std::size_t p = 0; p < paths.size(); ++p) {
            auto archive = thug2::Archive::load(paths[p]);
            for (std::size_t i = 0; i < archive.entries().size(); ++i) {
                const auto& e = archive.entries()[i];
                auto info = store.find(alternate(e.name));
                check(bool(info), "Missing entry: " + e.name);
                if (!seen.insert(e.name_checksum).second) {
                    check(info->mount <= ids[p], "Later source incorrectly won: " + e.name);
                    ++shadowed;
                    continue;
                }
                check(info->mount == ids[p] && info->kind == thug2::AssetSource::Archive && info->size == e.size,
                      "Wrong archive provenance: " + e.name);
                auto file = store.open(alternate(e.name));
                check(bool(file), "Cannot open entry: " + e.name);
                thug2::PreDecoder decoder;
                compare(archive.read(i, decoder), *file, e.name);
                ++unique; bytes += file->size();
            }
        }
        // Test actual disk fallback without loading large movies/audio into RAM.
        for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
            if (e.is_symlink() || !e.is_regular_file() || e.file_size() > 65536) continue;
            auto name = e.path().lexically_relative(root).generic_string();
            if (seen.count(thug2::name_crc(name))) continue;
            auto info = store.find(alternate(name));
            check(info && info->mount == disk && info->kind == thug2::AssetSource::Directory, "Loose fallback: " + name);
            auto file = store.open(alternate(name));
            check(bool(file), "Missing loose file: " + name);
            std::ifstream input(e.path(), std::ios::binary);
            thug2::Bytes expected((std::istreambuf_iterator<char>(input)), {});
            compare(expected, *file, name);
            ++loose; loose_bytes += expected.size();
        }
        check(!store.find("__thug2_native_missing_asset__"), "Unexpected missing-file match");
        std::cout << "PASS: " << paths.size() << " archives; " << unique << " resolved assets (" << bytes
                  << " bytes); " << shadowed << " shadowed entries; " << loose << " loose fallbacks ("
                  << loose_bytes << " bytes)\n";
        if (argc == 3) {
            std::ofstream report(argv[2]);
            report << "{\n  \"archives\": " << paths.size() << ",\n  \"resolved_assets\": " << unique
                   << ",\n  \"decoded_bytes\": " << bytes << ",\n  \"shadowed_entries\": " << shadowed
                   << ",\n  \"loose_files\": " << loose << ",\n  \"loose_bytes\": " << loose_bytes << "\n}\n";
            report.close(); check(bool(report), "Cannot write report");
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
