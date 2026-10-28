#pragma once
#include "thug2/core.hpp"
#include <unordered_map>

namespace thug2::qb {
// Token QB, not the relocated symbol-image format with magic 0xb075ef6f.
struct Token { std::size_t offset, size; std::uint8_t opcode; };
Token read_token(std::span<const std::uint8_t> bytes, std::size_t offset);
std::string_view token_name(std::uint8_t opcode);
struct Script {
    std::uint32_t name, checksum;
    std::size_t offset, body_offset, end_offset; // End is after endscript.
    std::size_t container_depth; // Scripts can be values inside global structs.
};
struct Symbol {
    std::uint32_t name;
    std::size_t offset, value_offset, end_offset;
};
struct Name { std::uint32_t checksum; std::string text; };
struct Jump { std::size_t offset, target; };

// Owns bytes and offset-based metadata. Does not relocate, evaluate or execute.
// Rejects malformed/truncated tokens, unbalanced delimiters/scripts, missing
// EOF and jumps that do not land on token boundaries.
class Document {
public:
    explicit Document(Bytes bytes);
    const std::vector<Token>& tokens() const { return tokens_; }
    const std::vector<Script>& scripts() const { return scripts_; }
    const std::vector<Symbol>& symbols() const { return symbols_; }
    const std::vector<Name>& names() const { return names_; }
    const std::vector<Jump>& jumps() const { return jumps_; }
    std::span<const std::uint8_t> bytes() const { return bytes_; }
    std::string label(std::uint32_t checksum) const;
    std::string describe(std::size_t token_index, std::size_t address_origin = 0) const;
private:
    Bytes bytes_;
    std::vector<Token> tokens_;
    std::vector<Script> scripts_;
    std::vector<Symbol> symbols_;
    std::vector<Name> names_;
    std::vector<Jump> jumps_;
    std::unordered_map<std::uint32_t, std::size_t> name_index_;
};
} // namespace thug2::qb
