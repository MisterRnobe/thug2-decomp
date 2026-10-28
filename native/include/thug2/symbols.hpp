#pragma once
#include "thug2/assets.hpp"
#include "thug2/precompiled.hpp"
#include <map>
#include <memory>
#include <variant>

namespace thug2::qb {
using ModuleId = std::uint64_t;
struct SymbolModule {
    ModuleId id;
    std::string name;
    std::optional<AssetInfo> origin;
    std::variant<Document, PrecompiledImage> data;
};
enum class DefinitionStorage { TokenValue, TokenScript, ImageValue };
struct Definition {
    std::uint32_t name;
    ValueType type; // None for an unevaluated/unknown token expression.
    DefinitionStorage storage;
    std::size_t offset, index; // Index in module symbols/scripts/image symbols.
    std::optional<std::uint32_t> alias;
    std::shared_ptr<const SymbolModule> module;
    std::optional<std::uint32_t> script_checksum;
};
enum class ResolveStatus { Resolved, Missing, Cycle, Limit };
std::string_view resolve_status_name(ResolveStatus status);
struct Resolution {
    ResolveStatus status;
    std::vector<std::uint32_t> path; // Includes missing/repeated/limited endpoint.
    std::shared_ptr<const Definition> target; // Set only when resolved.
    // Original LookUpSymbol returns this last alias if its target is absent.
    // Keep that information while reporting Missing explicitly.
    std::shared_ptr<const Definition> last_alias;
};

// Permanent-load semantics: last definition wins, except unchanged token
// scripts reuse the current script as ParseQB does. Unloading removes only the
// current definitions owned by that module and never restores overwritten ones.
// No expression evaluation, temporary loads, engine functions or script calls.
class SymbolTable {
public:
    ModuleId load_bytes(std::string name, Bytes bytes);
    ModuleId load(const AssetStore& assets, std::string_view name);
    // Parse all assets first; missing/bad inputs leave the whole table unchanged.
    std::vector<ModuleId> load_batch(const AssetStore& assets, std::span<const std::string> names);
    bool unload(ModuleId id);
    void clear();
    std::shared_ptr<const Definition> find(std::uint32_t name) const;
    Resolution resolve(std::uint32_t name, std::size_t max_alias_hops = 256) const;
    std::string label(std::uint32_t name) const; // Escaped spelling or hexadecimal CRC.
    std::vector<std::shared_ptr<const Definition>> definitions() const; // Sorted by CRC.
    std::vector<std::shared_ptr<const SymbolModule>> modules() const; // Load order.
private:
    struct Prepared;
    static Prepared prepare(ModuleId id, std::string name, std::optional<AssetInfo> origin, Bytes bytes);
    std::vector<ModuleId> commit(std::vector<Prepared> prepared);
    struct NameRecord { ModuleId module; std::string label; };
    std::unordered_map<std::uint32_t, std::shared_ptr<const Definition>> definitions_;
    std::unordered_map<std::uint32_t, NameRecord> names_;
    std::map<ModuleId, std::shared_ptr<const SymbolModule>> modules_;
    ModuleId next_id_ = 1;
};
} // namespace thug2::qb
