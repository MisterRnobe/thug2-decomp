#include "thug2/core.hpp"
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: thug2-prx list|verify ARCHIVE... | extract ARCHIVE NEW_DIRECTORY | crc NAME");
        const std::string command = argv[1];
        if (command == "crc" && argc == 3) {
            std::cout << std::hex << std::setw(8) << std::setfill('0') << thug2::name_crc(argv[2]) << '\n';
        } else if (command == "extract" && argc == 4) {
            auto archive = thug2::Archive::load(argv[2]);
            thug2::extract(archive, argv[3]);
            std::cout << "Extracted " << argv[2] << " to " << argv[3] << '\n';
        } else if ((command == "list" || command == "verify") && argc >= 3) {
            std::size_t files = 0, bytes = 0;
            for (int arg = 2; arg < argc; ++arg) {
                auto archive = thug2::Archive::load(argv[arg]);
                thug2::PreDecoder decoder;
                for (std::size_t i = 0; i < archive.entries().size(); ++i) {
                    const auto& e = archive.entries()[i];
                    if (command == "verify") {
                        // Each entry must also decode with a fresh window: this
                        // detects dependencies on another file's retained tail.
                        thug2::PreDecoder fresh;
                        auto data = archive.read(i, decoder);
                        if (data != archive.read(i, fresh)) throw std::runtime_error("Entry depends on prior decoder state: " + e.name);
                        bytes += data.size();
                    } else {
                        std::cout << argv[arg] << '\t' << e.size << '\t' << e.compressed_size << '\t'
                                  << (e.active ? "active" : "duplicate") << '\t' << e.name << '\n';
                    }
                    ++files;
                }
            }
            if (command == "verify") std::cout << "Verified " << argc-2 << " archives, " << files << " entries, " << bytes << " decoded bytes\n";
        } else throw std::runtime_error("Invalid command or argument count");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "thug2-prx: " << e.what() << '\n';
        return 1;
    }
}
