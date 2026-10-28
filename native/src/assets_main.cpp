#include "thug2/assets.hpp"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

int main(int argc, char** argv) {
    try {
        if (argc < 5 || ((argc-3) % 2) || (std::string(argv[1]) != "locate" && std::string(argv[1]) != "cat"))
            throw std::runtime_error("Usage: thug2-assets locate|cat NAME [--prx ARCHIVE | --dir ROOT]... (first source wins)");
        thug2::AssetStore assets;
        for (int i = 3; i < argc; i += 2) {
            std::string option = argv[i];
            if (option == "--prx") assets.mount_archive(argv[i+1]);
            else if (option == "--dir") assets.mount_directory(argv[i+1]);
            else throw std::runtime_error("Unknown source option: " + option);
        }
        if (std::string(argv[1]) == "locate") {
            auto info = assets.find(argv[2]);
            if (!info) { std::cerr << "Asset not found: " << argv[2] << '\n'; return 2; }
            std::cout << (info->kind == thug2::AssetSource::Archive ? "archive" : "directory") << '\t'
                      << info->source.generic_string() << '\t' << info->name << '\t' << info->size << '\n';
        } else {
            auto file = assets.open(argv[2]);
            if (!file) { std::cerr << "Asset not found: " << argv[2] << '\n'; return 2; }
#ifdef _WIN32
            if (_setmode(_fileno(stdout), _O_BINARY) == -1) throw std::runtime_error("Cannot set binary stdout");
#endif
            std::cout.write(reinterpret_cast<const char*>(file->bytes().data()), static_cast<std::streamsize>(file->size()));
            std::cout.flush();
            if (!std::cout) throw std::runtime_error("Cannot write asset to stdout");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "thug2-assets: " << e.what() << '\n';
        return 1;
    }
}
