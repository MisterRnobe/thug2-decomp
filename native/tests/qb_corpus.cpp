#include "thug2/assets.hpp"
#include "thug2/qb.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3) throw std::runtime_error("Usage: qb-corpus DATA_ROOT [REPORT.json]");
        auto root = std::filesystem::canonical(argv[1]);
        std::vector<std::filesystem::path> archives;
        for (const auto& e : std::filesystem::directory_iterator(root/"pre"))
            if (e.path().extension() == ".prx") archives.push_back(e.path());
        std::sort(archives.begin(), archives.end());
        thug2::AssetStore assets;
        std::map<std::uint32_t, std::string> names;
        auto add_name = [&](const std::string& name) {
            if (name.size() >= 3 && name.substr(name.size()-3) == ".qb") names.emplace(thug2::name_crc(name), name);
        };
        for (const auto& path : archives) {
            assets.mount_archive(path);
            auto archive = thug2::Archive::load(path);
            for (const auto& e : archive.entries()) add_name(e.name);
        }
        assets.mount_directory(root);
        std::size_t precompiled = 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || e.is_symlink()) continue;
            auto name = e.path().lexically_relative(root).generic_string();
            add_name(name);
            if (name.ends_with(".qb.xbx")) ++precompiled;
        }
        if (names.empty()) throw std::runtime_error("No token QB files found");
        std::size_t files = 0, bytes = 0, tokens = 0, scripts = 0, symbols = 0, checksums = 0, jumps = 0;
        std::size_t from_archive = 0, from_disk = 0;
        for (const auto& [crc, name] : names) {
            (void)crc;
            try {
                auto file = assets.open(name);
                if (!file) throw std::runtime_error("Asset missing");
                thug2::qb::Document doc(thug2::Bytes(file->bytes().begin(), file->bytes().end()));
                ++files; bytes += file->size(); tokens += doc.tokens().size(); scripts += doc.scripts().size();
                symbols += doc.symbols().size(); checksums += doc.names().size(); jumps += doc.jumps().size();
                if (file->info().kind == thug2::AssetSource::Archive) ++from_archive; else ++from_disk;
            } catch (const std::exception& e) { throw std::runtime_error(name + ": " + e.what()); }
        }
        std::cout << "PASS: " << files << " token QB assets (" << from_archive << " archived, " << from_disk
                  << " loose), " << tokens << " tokens, " << scripts << " scripts, " << symbols
                  << " globals, " << jumps << " jump targets; " << precompiled << " loose precompiled images excluded\n";
        if (argc == 3) {
            std::ofstream report(argv[2]);
            report << "{\n  \"files\": " << files << ",\n  \"archived\": " << from_archive << ",\n  \"loose\": " << from_disk
                   << ",\n  \"bytes\": " << bytes << ",\n  \"tokens\": " << tokens << ",\n  \"scripts\": " << scripts
                   << ",\n  \"globals\": " << symbols << ",\n  \"names\": " << checksums << ",\n  \"jump_targets\": " << jumps
                   << ",\n  \"excluded_loose_precompiled\": " << precompiled << "\n}\n";
            report.close(); if (!report) throw std::runtime_error("Cannot write report");
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
