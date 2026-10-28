// Name-alias lookup and permanent replacement/unload semantics informed by
// upstream script/struct.cpp and load.cpp; portable ownership/transactions new.
#include "thug2/symbols.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace thug2::qb {
namespace {
std::uint32_t word(std::span<const std::uint8_t> b, std::size_t offset) {
    if (offset > b.size() || b.size()-offset < 4) throw std::runtime_error("Truncated symbol word");
    return std::uint32_t(b[offset]) | std::uint32_t(b[offset+1])<<8 |
           std::uint32_t(b[offset+2])<<16 | std::uint32_t(b[offset+3])<<24;
}
ValueType token_type(std::uint8_t t) {
    switch (t) {
    case 0x16: return ValueType::Name; case 0x17: case 0x18: return ValueType::Integer;
    case 0x1a: return ValueType::Float; case 0x1b: return ValueType::String;
    case 0x1c: return ValueType::LocalString; case 0x1e: return ValueType::Vector;
    case 0x1f: return ValueType::Pair; case 3: return ValueType::Structure;
    case 5: return ValueType::Array; default: return ValueType::None;
    }
}
} // namespace
struct SymbolTable::Prepared {
    std::shared_ptr<const SymbolModule> module;
    std::vector<std::shared_ptr<const Definition>> definitions;
    std::vector<std::pair<std::uint32_t, std::string>> names;
};
std::string_view resolve_status_name(ResolveStatus s) {
    switch (s) {
    case ResolveStatus::Resolved: return "resolved"; case ResolveStatus::Missing: return "missing";
    case ResolveStatus::Cycle: return "cycle"; case ResolveStatus::Limit: return "limit";
    }
    return "unknown";
}
SymbolTable::Prepared SymbolTable::prepare(ModuleId id, std::string name,
                                          std::optional<AssetInfo> origin, Bytes bytes) {
    if (!id) throw std::overflow_error("Module IDs exhausted");
    try {
        std::variant<Document, PrecompiledImage> data = is_precompiled(bytes)
            ? std::variant<Document, PrecompiledImage>(std::in_place_type<PrecompiledImage>, std::move(bytes))
            : std::variant<Document, PrecompiledImage>(std::in_place_type<Document>, std::move(bytes));
        auto module = std::make_shared<const SymbolModule>(SymbolModule{id, name, std::move(origin), std::move(data)});
        Prepared result{module, {}, {}};
        if (auto doc = std::get_if<Document>(&module->data)) {
            for (std::size_t i = 0; i < doc->symbols().size(); ++i) {
                const auto& s = doc->symbols()[i];
                auto token = read_token(doc->bytes(), s.value_offset);
                auto type = token_type(token.opcode);
                std::optional<std::uint32_t> alias;
                // An expression beginning with a name is not necessarily an alias.
                if (token.opcode == 0x16 && token.size == s.end_offset-s.value_offset)
                    alias = word(doc->bytes(), s.value_offset+1);
                if (token.opcode != 3 && token.opcode != 5 && token.size != s.end_offset-s.value_offset)
                    type = ValueType::None;
                result.definitions.push_back(std::make_shared<const Definition>(Definition{
                    s.name, type, DefinitionStorage::TokenValue, s.offset, i, alias, module, {}}));
            }
            for (std::size_t i = 0; i < doc->scripts().size(); ++i) {
                const auto& s = doc->scripts()[i];
                if (s.container_depth) continue; // Structure components are not global definitions.
                result.definitions.push_back(std::make_shared<const Definition>(Definition{
                    s.name, ValueType::Script, DefinitionStorage::TokenScript, s.offset, i, {}, module, s.checksum}));
            }
            std::unordered_set<std::uint32_t> seen;
            for (const auto& n : doc->names())
                if (seen.insert(n.checksum).second) result.names.emplace_back(n.checksum, doc->label(n.checksum));
        } else {
            const auto& image = std::get<PrecompiledImage>(module->data);
            for (std::size_t i = 0; i < image.symbols().size(); ++i) {
                const auto& s = image.symbols()[i];
                const auto& value = image.values()[s.value];
                std::optional<std::uint32_t> alias;
                if (value.type == ValueType::Name) alias = value.raw;
                std::optional<std::uint32_t> checksum;
                if (value.script) checksum = image.scripts()[*value.script].checksum;
                result.definitions.push_back(std::make_shared<const Definition>(Definition{
                    s.name, value.type, DefinitionStorage::ImageValue, s.offset, i, alias, module, checksum}));
            }
        }
        std::stable_sort(result.definitions.begin(), result.definitions.end(),
                         [](const auto& a, const auto& b) { return a->offset < b->offset; });
        return result;
    } catch (const std::exception& e) { throw std::runtime_error(name + ": " + e.what()); }
}
std::vector<ModuleId> SymbolTable::commit(std::vector<Prepared> prepared) {
    // Candidate maps make parsing, allocation and publication one transaction.
    auto definitions = definitions_;
    auto names = names_;
    auto modules = modules_;
    std::vector<ModuleId> ids;
    for (const auto& p : prepared) {
        modules.emplace(p.module->id, p.module);
        for (const auto& d : p.definitions) {
            auto previous = definitions.find(d->name);
            if (d->storage == DefinitionStorage::TokenScript && previous != definitions.end() &&
                previous->second->script_checksum && previous->second->script_checksum == d->script_checksum)
                continue; // ParseQB retains the old script and its file ownership.
            definitions[d->name] = d;
        }
        for (const auto& [crc, label] : p.names) names[crc] = {p.module->id, label};
        ids.push_back(p.module->id);
    }
    definitions_.swap(definitions); names_.swap(names); modules_.swap(modules);
    if (!ids.empty()) next_id_ = ids.back()+1;
    return ids;
}
ModuleId SymbolTable::load_bytes(std::string name, Bytes bytes) {
    std::vector<Prepared> prepared;
    prepared.push_back(prepare(next_id_, std::move(name), {}, std::move(bytes)));
    return commit(std::move(prepared)).front();
}
ModuleId SymbolTable::load(const AssetStore& assets, std::string_view name) {
    const std::array<std::string, 1> names{std::string(name)};
    return load_batch(assets, names).front();
}
std::vector<ModuleId> SymbolTable::load_batch(const AssetStore& assets, std::span<const std::string> names) {
    std::vector<Prepared> prepared;
    auto id = next_id_;
    for (const auto& name : names) {
        auto file = assets.open(name);
        if (!file) throw std::runtime_error("QB module not found: " + name);
        prepared.push_back(prepare(id, name, file->info(), Bytes(file->bytes().begin(), file->bytes().end())));
        ++id;
    }
    return commit(std::move(prepared));
}
bool SymbolTable::unload(ModuleId id) {
    if (!modules_.count(id)) return false;
    std::erase_if(definitions_, [=](const auto& item) { return item.second->module->id == id; });
    std::erase_if(names_, [=](const auto& item) { return item.second.module == id; });
    modules_.erase(id);
    return true;
}
void SymbolTable::clear() { definitions_.clear(); names_.clear(); modules_.clear(); }
std::shared_ptr<const Definition> SymbolTable::find(std::uint32_t name) const {
    auto found = definitions_.find(name);
    return found == definitions_.end() ? nullptr : found->second;
}
Resolution SymbolTable::resolve(std::uint32_t name, std::size_t max_alias_hops) const {
    Resolution result{ResolveStatus::Missing, {}, {}, {}};
    std::unordered_set<std::uint32_t> seen;
    std::size_t hops = 0;
    for (;;) {
        result.path.push_back(name);
        if (!seen.insert(name).second) { result.status = ResolveStatus::Cycle; return result; }
        auto d = find(name);
        if (!d) return result;
        if (!d->alias) { result.status = ResolveStatus::Resolved; result.target = std::move(d); return result; }
        result.last_alias = d;
        if (hops == max_alias_hops) { result.status = ResolveStatus::Limit; return result; }
        ++hops; name = *d->alias;
    }
}
std::string SymbolTable::label(std::uint32_t name) const {
    auto found = names_.find(name);
    if (found != names_.end()) return found->second.label;
    std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << name;
    return out.str();
}
std::vector<std::shared_ptr<const Definition>> SymbolTable::definitions() const {
    std::vector<std::shared_ptr<const Definition>> out;
    for (const auto& [crc, d] : definitions_) { (void)crc; out.push_back(d); }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a->name < b->name; });
    return out;
}
std::vector<std::shared_ptr<const SymbolModule>> SymbolTable::modules() const {
    std::vector<std::shared_ptr<const SymbolModule>> out;
    for (const auto& [id, m] : modules_) { (void)id; out.push_back(m); }
    return out;
}
} // namespace thug2::qb
