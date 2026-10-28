// Token layouts/checksum convention from thug2-decomp script/qb.cpp, load.cpp
// and parse.cpp at 2d82ed2. Bounds/structure validation is new. See NOTICE.md.
#include "thug2/qb.hpp"
#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace thug2::qb {
namespace {
[[noreturn]] void error(std::size_t offset, const std::string& message) {
    std::ostringstream out;
    out << "QB offset 0x" << std::hex << offset << ": " << message;
    throw std::runtime_error(out.str());
}
void require(std::span<const std::uint8_t> b, std::size_t offset, std::size_t count) {
    if (offset > b.size() || count > b.size()-offset) error(offset, "truncated token payload");
}
std::uint32_t word(std::span<const std::uint8_t> b, std::size_t offset, unsigned count = 4) {
    require(b, offset, count);
    std::uint32_t n = 0;
    for (unsigned i = 0; i < count; ++i) n |= std::uint32_t(b[offset+i]) << (8*i);
    return n;
}
bool table(std::uint8_t t) { return t == 0x2f || t == 0x37 || t == 0x40 || t == 0x41; }
bool line(std::uint8_t t) { return t == 1 || t == 2; }
std::string hex(std::uint32_t n) {
    std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << n;
    return out.str();
}
std::string quote(std::span<const std::uint8_t> b) {
    std::ostringstream out; out << '"';
    for (auto c : b) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c >= 32 && c < 127) out << char(c);
        else out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    out << '"'; return out.str();
}
} // namespace

std::string_view token_name(std::uint8_t t) {
    switch (t) {
    case 0: return "eof"; case 1: return "eol"; case 2: return "line";
    case 3: return "{"; case 4: return "}"; case 5: return "["; case 6: return "]";
    case 7: return "="; case 8: return "."; case 9: return ",";
    case 0x0a: return "-"; case 0x0b: return "+"; case 0x0c: return "/"; case 0x0d: return "*";
    case 0x0e: return "("; case 0x0f: return ")";
    case 0x11: return "token_11"; case 0x12: return "<"; case 0x13: return "<=";
    case 0x14: return ">"; case 0x15: return ">=";
    case 0x16: return "name"; case 0x17: return "integer"; case 0x18: return "hex_integer";
    case 0x1a: return "float"; case 0x1b: return "string"; case 0x1c: return "local_string";
    case 0x1e: return "vector"; case 0x1f: return "pair";
    case 0x20: return "begin"; case 0x21: return "repeat"; case 0x22: return "break";
    case 0x23: return "script"; case 0x24: return "endscript";
    case 0x25: return "if"; case 0x26: return "else"; case 0x27: return "elseif";
    case 0x28: return "endif"; case 0x29: return "return"; case 0x2b: return "checksum_name";
    case 0x2c: return "all_args"; case 0x2d: return "arg"; case 0x2e: return "jump";
    case 0x2f: return "random"; case 0x30: return "random_range";
    case 0x32: return "or"; case 0x33: return "and"; case 0x34: return "token_34";
    case 0x35: return "token_35"; case 0x36: return "token_36";
    case 0x37: return "random2"; case 0x38: return "random_range2"; case 0x39: return "not";
    case 0x3a: return "token_3a"; case 0x3b: return "token_3b";
    case 0x3c: return "switch"; case 0x3d: return "endswitch";
    case 0x3e: return "case"; case 0x3f: return "default";
    case 0x40: return "random_no_repeat"; case 0x41: return "random_permute";
    case 0x42: return "colon"; case 0x43: return "resolved_c_function";
    case 0x44: return "resolved_member"; case 0x45: return "section";
    case 0x46: return "resolved_member_entry"; case 0x47: return "fast_if";
    case 0x48: return "fast_else"; case 0x49: return "short_jump";
    default: return "unknown";
    }
}
Token read_token(std::span<const std::uint8_t> b, std::size_t offset) {
    require(b, offset, 1);
    auto t = b[offset];
    std::size_t size = 1;
    switch (t) {
    case 2: case 0x16: case 0x17: case 0x18: case 0x1a: case 0x2e:
    case 0x43: case 0x44: case 0x46: size = 5; break;
    case 0x1b: case 0x1c: {
        auto n = word(b, offset+1);
        require(b, offset+5, n);
        if (!n || b[offset+4+n] != 0) error(offset, "string lacks terminating NUL");
        size = 5+std::size_t(n); break;
    }
    case 0x1e: size = 13; break;
    case 0x1f: size = 9; break;
    case 0x2b: {
        require(b, offset+1, 4);
        auto start = b.begin()+offset+5;
        auto end = std::find(start, b.end(), 0);
        if (end == b.end()) error(offset, "unterminated checksum name");
        size = static_cast<std::size_t>(end-(b.begin()+offset))+1; break;
    }
    case 0x2f: case 0x37: case 0x40: case 0x41: {
        auto n = word(b, offset+1);
        if (!n || n > (b.size()-offset-5)/6) error(offset, "invalid random table count");
        size = 5+std::size_t(n)*6; break;
    }
    case 0x47: case 0x48: case 0x49: size = 3; break;
    default:
        if (token_name(t) == "unknown") error(offset, "unknown token " + hex(t));
    }
    require(b, offset, size);
    return {offset, size, t};
}

Document::Document(Bytes bytes) : bytes_(std::move(bytes)) {
    const std::span<const std::uint8_t> b(bytes_);
    if (b.size() >= 4 && word(b, 0) == 0xb075ef6fu)
        error(0, "precompiled QB symbol image is not a token QB (unsupported format)");
    std::size_t offset = 0;
    bool eof = false;
    while (offset < b.size()) {
        auto t = read_token(b, offset);
        tokens_.push_back(t); offset += t.size;
        if (!t.opcode) { eof = true; break; }
    }
    if (!eof) error(offset, "missing end-of-file token");
    if (offset != b.size()) error(offset, "data after end-of-file token");

    std::vector<std::uint8_t> delimiters;
    std::optional<Script> script;
    std::vector<std::size_t> depth(tokens_.size());
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
        const auto& t = tokens_[i];
        depth[i] = delimiters.size();
        if (t.opcode == 3 || t.opcode == 5 || t.opcode == 0x0e) delimiters.push_back(t.opcode);
        if (t.opcode == 4 || t.opcode == 6 || t.opcode == 0x0f) {
            if (script && delimiters.size() <= script->container_depth) error(t.offset, "script closes its outer container");
            auto expected = t.opcode == 4 ? 3 : t.opcode == 6 ? 5 : 0x0e;
            if (delimiters.empty() || delimiters.back() != expected) error(t.offset, "mismatched closing delimiter");
            delimiters.pop_back();
        }
        if (t.opcode == 0x23) {
            if (script) error(t.offset, "nested script definition");
            if (i+1 >= tokens_.size() || tokens_[i+1].opcode != 0x16) error(t.offset, "script lacks name");
            script = Script{word(b, t.offset+2), 0xffffffffu, t.offset, t.offset+6, 0, delimiters.size()};
        } else if (t.opcode == 0x24) {
            if (!script || delimiters.size() != script->container_depth) error(t.offset, "unmatched endscript or unclosed delimiter");
            script->end_offset = t.offset+1; scripts_.push_back(*script); script.reset();
        } else if (script && t.offset >= script->body_offset && !line(t.opcode)) {
            script->checksum = extend_crc(script->checksum, b.subspan(t.offset, t.size));
        }
        if (t.opcode == 0x2b) {
            auto checksum = word(b, t.offset+1);
            std::string text(b.begin()+t.offset+5, b.begin()+t.offset+t.size-1);
            name_index_.emplace(checksum, names_.size()); // Tool lookup: first spelling wins.
            names_.push_back({checksum, std::move(text)});
        }
        auto add_jump = [&](std::size_t base, std::int64_t delta) {
            if (base > b.size() || delta < -static_cast<std::int64_t>(base) ||
                delta >= static_cast<std::int64_t>(b.size()-base)) error(t.offset, "jump target outside file");
            jumps_.push_back({t.offset, static_cast<std::size_t>(static_cast<std::int64_t>(base)+delta)});
        };
        if (t.opcode == 0x2e) add_jump(t.offset+5, std::bit_cast<std::int32_t>(word(b, t.offset+1)));
        if (t.opcode >= 0x47 && t.opcode <= 0x49) add_jump(t.offset+1, word(b, t.offset+1, 2));
        if (table(t.opcode)) {
            auto n = word(b, t.offset+1);
            for (std::size_t j = 0; j < n; ++j) {
                auto p = t.offset+5+std::size_t(n)*2+j*4;
                add_jump(p+4, std::bit_cast<std::int32_t>(word(b, p)));
            }
        }
    }
    if (script) error(script->offset, "script lacks endscript");
    if (!delimiters.empty()) error(b.size(), "unclosed delimiter");
    for (const auto& j : jumps_) {
        auto found = std::lower_bound(tokens_.begin(), tokens_.end(), j.target,
                                      [](const Token& t, std::size_t n) { return t.offset < n; });
        if (found == tokens_.end() || found->offset != j.target) error(j.offset, "jump lands inside token payload");
    }
    // Index top-level name = value definitions without evaluating expressions,
    // following global references, or materializing runtime game structures.
    std::size_t script_index = 0;
    for (std::size_t i = 0; i+1 < tokens_.size(); ++i) {
        const auto& t = tokens_[i];
        while (script_index < scripts_.size() && t.offset >= scripts_[script_index].end_offset) ++script_index;
        if (script_index < scripts_.size() && t.offset >= scripts_[script_index].offset) continue;
        if (depth[i] || t.opcode != 0x16) continue;
        auto next = i+1;
        while (next < tokens_.size() && line(tokens_[next].opcode)) ++next;
        if (next == tokens_.size() || tokens_[next].opcode != 7) continue;
        ++next;
        while (next < tokens_.size() && line(tokens_[next].opcode)) ++next;
        if (next == tokens_.size() || !tokens_[next].opcode) error(t.offset, "symbol lacks value");
        auto end = next;
        for (; end < tokens_.size(); ++end) {
            if (depth[end] != 0) continue;
            auto op = tokens_[end].opcode;
            if (line(op) || !op || op == 0x2b || op == 0x45 || (end > next && op == 0x23)) break;
            // Compilers can omit EOL between complete global definitions.
            // A following name = starts another definition, not this value.
            if (end > next && op == 0x16) {
                auto following = end+1;
                while (following < tokens_.size() && line(tokens_[following].opcode)) ++following;
                if (following < tokens_.size() && tokens_[following].opcode == 7) break;
            }
        }
        if (end == next) error(t.offset, "symbol lacks value");
        symbols_.push_back({word(b, t.offset+1), t.offset, tokens_[next].offset,
                            end == tokens_.size() ? b.size() : tokens_[end].offset});
        i = end-1;
    }
}
std::string Document::label(std::uint32_t checksum) const {
    auto found = name_index_.find(checksum);
    if (found == name_index_.end()) return hex(checksum);
    return quote({reinterpret_cast<const std::uint8_t*>(names_[found->second].text.data()), names_[found->second].text.size()});
}
std::string Document::describe(std::size_t token_index, std::size_t address_origin) const {
    const auto& t = tokens_.at(token_index);
    const std::span<const std::uint8_t> b(bytes_);
    std::ostringstream out; out << token_name(t.opcode);
    auto p = t.offset+1;
    switch (t.opcode) {
    case 0x16: out << ' ' << label(word(b, p)); break;
    case 0x2b: out << ' ' << hex(word(b, p)) << ' ' << quote(b.subspan(p+4, t.size-6)); break;
    case 0x17: out << ' ' << std::bit_cast<std::int32_t>(word(b, p)); break;
    case 2: case 0x18: case 0x2e: case 0x43: case 0x44: case 0x46: out << ' ' << hex(word(b, p)); break;
    case 0x1a: case 0x1e: case 0x1f: {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        for (std::size_t n = 0; n < (t.size-1)/4; ++n)
            out << ' ' << std::setprecision(std::numeric_limits<float>::max_digits10) << std::bit_cast<float>(word(b, p+n*4));
        break;
    }
    case 0x1b: case 0x1c: out << ' ' << quote(b.subspan(p+4, t.size-6)); break;
    case 0x47: case 0x48: case 0x49: {
        auto target = p+word(b, p, 2);
        out << " -> ";
        if (target < address_origin) out << '-' << hex(static_cast<std::uint32_t>(address_origin-target));
        else out << hex(static_cast<std::uint32_t>(target-address_origin));
        break;
    }
    default: if (table(t.opcode)) out << " choices=" << word(b, p);
    }
    return out.str();
}
} // namespace thug2::qb
