#include "thug2/assets.hpp"
#include "thug2/qb.hpp"
#include "thug2/precompiled.hpp"
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        const std::string command = argc > 1 ? argv[1] : "";
        if (argc < 5 || (argc-3)%2 || (command != "summary" && command != "symbols" && command != "dump"))
            throw std::runtime_error("Usage: thug2-qb summary|symbols|dump NAME [--prx ARCHIVE | --dir ROOT]... (first source wins)");
        thug2::AssetStore assets;
        for (int i = 3; i < argc; i += 2) {
            std::string option = argv[i];
            if (option == "--prx") assets.mount_archive(argv[i+1]);
            else if (option == "--dir") assets.mount_directory(argv[i+1]);
            else throw std::runtime_error("Unknown source option: " + option);
        }
        auto file = assets.open(argv[2]);
        if (!file) { std::cerr << "QB asset not found: " << argv[2] << '\n'; return 2; }
        if (thug2::qb::is_precompiled(file->bytes())) {
            thug2::qb::PrecompiledImage image(thug2::Bytes(file->bytes().begin(), file->bytes().end()));
            if (command == "summary") {
                std::cout << "format\tprecompiled\nsource\t" << file->info().source.generic_string() << '\t' << file->info().name
                          << "\nbytes\t" << image.bytes().size() << "\nsymbols\t" << image.symbols().size()
                          << "\nvalues\t" << image.values().size() << "\nscripts\t" << image.scripts().size()
                          << "\nreferences\t" << image.references().size() << '\n';
            } else if (command == "symbols") {
                for (const auto& s : image.symbols())
                    std::cout << "symbol\t" << std::hex << s.name << "\toffset=" << s.offset << std::dec
                              << "\tvalue=" << s.value << '\t' << image.describe(s.value) << '\n';
            } else {
                for (std::size_t i = 0; i < image.values().size(); ++i) {
                    const auto& v = image.values()[i];
                    std::cout << "value " << i << "\toffset=0x" << std::hex << v.offset << "\tname=0x" << v.name
                              << std::dec << '\t' << image.describe(i);
                    if (!v.children.empty()) {
                        std::cout << "\tchildren=";
                        for (auto id : v.children) std::cout << id << ',';
                    }
                    std::cout << '\n';
                    if (v.script) {
                        const auto& script = image.scripts()[*v.script];
                        thug2::Bytes wrapped{0x23, 0x16};
                        for (unsigned n = 0; n < 4; ++n) wrapped.push_back(v.name >> (n*8));
                        wrapped.insert(wrapped.end(), script.code.begin(), script.code.end()); wrapped.push_back(0);
                        thug2::qb::Document body(std::move(wrapped));
                        for (std::size_t n = 0; n < body.tokens().size(); ++n) {
                            const auto& token = body.tokens()[n];
                            if (token.offset < 6 || token.offset >= script.code.size()+6) continue;
                            std::cout << "  script " << *v.script << "+0x" << std::hex << token.offset-6
                                      << std::dec << '\t' << body.describe(n, 6) << '\n';
                        }
                    }
                }
            }
            return 0;
        }
        thug2::qb::Document doc(thug2::Bytes(file->bytes().begin(), file->bytes().end()));
        if (command == "summary") {
            std::cout << "source\t" << file->info().source.generic_string() << '\t' << file->info().name << '\n'
                      << "bytes\t" << doc.bytes().size() << "\ntokens\t" << doc.tokens().size()
                      << "\nscripts\t" << doc.scripts().size() << "\nglobals\t" << doc.symbols().size()
                      << "\nchecksum_names\t" << doc.names().size() << "\njump_targets\t" << doc.jumps().size() << '\n';
        } else if (command == "symbols") {
            for (const auto& s : doc.scripts())
                std::cout << "script\t" << std::hex << s.name << '\t' << doc.label(s.name) << "\toffset=" << s.offset
                          << "\tcrc=" << s.checksum << std::dec << "\tbody_bytes=" << s.end_offset-s.body_offset << '\n';
            for (const auto& s : doc.symbols())
                std::cout << "global\t" << std::hex << s.name << '\t' << doc.label(s.name) << "\toffset=" << s.offset
                          << std::dec << "\tvalue=" << thug2::qb::token_name(doc.bytes()[s.value_offset]) << '\n';
        } else {
            for (std::size_t i = 0; i < doc.tokens().size(); ++i)
                std::cout << std::hex << std::setw(8) << std::setfill('0') << doc.tokens()[i].offset
                          << "  " << doc.describe(i) << '\n';
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "thug2-qb: " << e.what() << '\n'; return 1; }
}
