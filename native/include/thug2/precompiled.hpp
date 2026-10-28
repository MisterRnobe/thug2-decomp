#pragma once
#include "thug2/qb.hpp"
#include <optional>

namespace thug2::qb {
enum class ValueType : std::uint8_t {
    None=0, Integer=1, Float=2, String=3, LocalString=4, Pair=5, Vector=6,
    Script=7, CFunction=8, MemberFunction=9, Structure=10, Array=12, Name=13
};
std::string_view value_type_name(ValueType type);
bool is_precompiled(std::span<const std::uint8_t> bytes);
struct ImageReference { std::size_t field, target; };
struct ImageValue {
    ValueType type;
    std::uint32_t name, raw;
    std::size_t offset, data_offset, end_offset;
    std::vector<std::size_t> children; // Indices, never pointers into this vector.
    std::string text;
    std::vector<std::uint32_t> coordinates; // IEEE-754 bits, preserving NaNs etc.
    std::optional<std::size_t> script;
    ValueType element_type = ValueType::None;
};
struct ImageSymbol { std::uint32_t name; std::size_t offset, end_offset, value; };
struct ImageScript {
    std::size_t value, offset, token_count;
    std::uint32_t checksum, stored_size;
    bool compressed, component;
    Bytes code; // Decoded body, through endscript and optional numbered EOL.
};

// Reads the packed layout identified by the supplied 20-byte ABI fingerprint.
// Immutable original bytes, explicit offsets/indices, no relocation or globals.
// Enforces inline packed values, bounded linked lists, and maximum depth 128.
class PrecompiledImage {
public:
    explicit PrecompiledImage(Bytes bytes);
    std::span<const std::uint8_t> bytes() const { return bytes_; }
    const std::vector<ImageSymbol>& symbols() const { return symbols_; }
    const std::vector<ImageValue>& values() const { return values_; }
    const std::vector<ImageScript>& scripts() const { return scripts_; }
    const std::vector<ImageReference>& references() const { return references_; }
    std::string describe(std::size_t value) const;
private:
    struct Result { std::size_t value, end; };
    std::uint32_t word(std::size_t offset) const;
    void require(std::size_t offset, std::size_t count) const;
    void reference(std::size_t field, std::size_t target);
    Result parse(ValueType type, std::size_t field, std::size_t cursor,
                 std::uint32_t name, unsigned depth, bool component = false);
    std::size_t script(std::size_t value, std::size_t offset, bool component);
    Bytes bytes_;
    std::vector<ImageSymbol> symbols_;
    std::vector<ImageValue> values_;
    std::vector<ImageScript> scripts_;
    std::vector<ImageReference> references_;
};
} // namespace thug2::qb
