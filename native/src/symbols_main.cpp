#include "thug2/symbols.hpp"
#include "thug2/values.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint32_t query(std::string_view text) {
    if (!text.starts_with("0x")) return thug2::name_crc(text);
    std::uint32_t n = 0;
    auto [end, error] = std::from_chars(text.data()+2, text.data()+text.size(), n, 16);
    if (error != std::errc{} || end != text.data()+text.size()) throw std::runtime_error("Invalid CRC query");
    return n;
}
void print(const thug2::qb::SymbolTable& table, const thug2::qb::Definition& d) {
    std::cout << "0x" << std::hex << d.name << std::dec << '\t' << table.label(d.name) << '\t'
              << thug2::qb::value_type_name(d.type) << "\tmodule=" << d.module->id << '\t' << d.module->name
              << "\toffset=0x" << std::hex << d.offset << std::dec;
    if (d.alias) std::cout << "\talias=" << table.label(*d.alias);
    std::cout << '\n';
}
}
int main(int argc, char** argv) {
    try {
        std::string command = argc > 1 ? argv[1] : "";
        if (command != "list" && command != "resolve" && command != "audit" && command != "value")
            throw std::runtime_error("Usage: thug2-symbols list|audit|resolve|value [NAME or 0xCRC] --load QB... [--prx ARCHIVE|--dir ROOT]...");
        int first = command == "resolve" || command == "value" ? 3 : 2;
        if (argc <= first || (argc-first)%2) throw std::runtime_error("Expected source/load option pairs");
        thug2::AssetStore assets;
        std::vector<std::string> files;
        for (int i = first; i < argc; i += 2) {
            std::string option = argv[i];
            if (option == "--load") files.emplace_back(argv[i+1]);
            else if (option == "--prx") assets.mount_archive(argv[i+1]);
            else if (option == "--dir") assets.mount_directory(argv[i+1]);
            else throw std::runtime_error("Unknown option: " + option);
        }
        if (files.empty()) throw std::runtime_error("At least one --load is required");
        thug2::qb::SymbolTable table;
        table.load_batch(assets, files);
        if (command == "value") {
            thug2::qb::ValueEvaluator evaluator(table);
            auto v = evaluator.global(query(argv[2]));
            std::cout << thug2::qb::value_type_name(v->type) << '\t' << thug2::qb::format_value(v) << '\n';
        } else if (command == "list") {
            for (const auto& d : table.definitions()) print(table, *d);
        } else if (command == "audit") {
            std::size_t aliases = 0, resolved = 0, missing = 0, cycles = 0, limits = 0;
            for (const auto& d : table.definitions()) {
                if (!d->alias) continue;
                ++aliases;
                switch (table.resolve(d->name).status) {
                case thug2::qb::ResolveStatus::Resolved: ++resolved; break;
                case thug2::qb::ResolveStatus::Missing: ++missing; break;
                case thug2::qb::ResolveStatus::Cycle: ++cycles; break;
                case thug2::qb::ResolveStatus::Limit: ++limits; break;
                }
            }
            std::cout << "modules\t" << table.modules().size() << "\ndefinitions\t" << table.definitions().size()
                      << "\naliases\t" << aliases << "\nresolved\t" << resolved << "\nmissing\t" << missing
                      << "\ncycles\t" << cycles << "\nlimits\t" << limits << '\n';
        } else {
            auto result = table.resolve(query(argv[2]));
            std::cout << thug2::qb::resolve_status_name(result.status) << '\n';
            for (auto crc : result.path) {
                if (auto d = table.find(crc)) print(table, *d);
                else std::cout << "missing\t" << table.label(crc) << '\n';
            }
            switch (result.status) {
            case thug2::qb::ResolveStatus::Resolved: return 0;
            case thug2::qb::ResolveStatus::Missing: return 2;
            case thug2::qb::ResolveStatus::Cycle: return 3;
            case thug2::qb::ResolveStatus::Limit: return 4;
            }
        }
    } catch (const thug2::qb::EvalError& e) {
        std::cerr << "thug2-symbols: " << thug2::qb::eval_failure_name(e.kind) << ": " << e.what() << '\n'; return 5;
    } catch (const std::exception& e) { std::cerr << "thug2-symbols: " << e.what() << '\n'; return 1; }
}
