// On-disk ABI verified from the supplied files. Recursive traversal follows
// upstream script/precompiled.cpp (2d82ed2); does not cast serialized structs.
#include "thug2/precompiled.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace thug2::qb {
namespace {
constexpr std::array<std::uint8_t, 20> layout{
    0x1c,0x08,0x02,0x04,0x10,0x04,0x08,0x0c,0x0c,0x08,
    0x02,0x04,0x14,0x02,0x04,0x0c,0x10,0x10,0x0c,0x00};
[[noreturn]] void fail(std::size_t offset, const std::string& message) {
    std::ostringstream out; out << "QB image offset 0x" << std::hex << offset << ": " << message;
    throw std::runtime_error(out.str());
}
std::size_t align4(std::size_t n) { return (n+3) & ~std::size_t(3); }
bool pointer_type(ValueType type) {
    return type == ValueType::String || type == ValueType::LocalString || type == ValueType::Pair ||
           type == ValueType::Vector || type == ValueType::Script || type == ValueType::Structure || type == ValueType::Array;
}
} // namespace
bool is_precompiled(std::span<const std::uint8_t> b) {
    return b.size() >= 4 && b[0] == 0x6f && b[1] == 0xef && b[2] == 0x75 && b[3] == 0xb0;
}
std::string_view value_type_name(ValueType type) {
    switch (type) {
    case ValueType::None: return "none"; case ValueType::Integer: return "integer";
    case ValueType::Float: return "float"; case ValueType::String: return "string";
    case ValueType::LocalString: return "local_string"; case ValueType::Pair: return "pair";
    case ValueType::Vector: return "vector"; case ValueType::Script: return "script";
    case ValueType::CFunction: return "c_function"; case ValueType::MemberFunction: return "member_function";
    case ValueType::Structure: return "structure"; case ValueType::Array: return "array";
    case ValueType::Name: return "name"; default: return "opaque";
    }
}
void PrecompiledImage::require(std::size_t offset, std::size_t count) const {
    if (offset > bytes_.size() || count > bytes_.size()-offset) fail(offset, "truncated image data");
}
std::uint32_t PrecompiledImage::word(std::size_t offset) const {
    require(offset, 4);
    return std::uint32_t(bytes_[offset]) | std::uint32_t(bytes_[offset+1])<<8 |
           std::uint32_t(bytes_[offset+2])<<16 | std::uint32_t(bytes_[offset+3])<<24;
}
void PrecompiledImage::reference(std::size_t field, std::size_t target) {
    if (!target || target >= bytes_.size() || target%4) fail(field, "invalid or unaligned stored offset");
    references_.push_back({field, target});
}
PrecompiledImage::PrecompiledImage(Bytes bytes) : bytes_(std::move(bytes)) {
    require(0, 28);
    if (!is_precompiled(bytes_)) fail(0, "not a precompiled QB image");
    if (word(4) != bytes_.size()) fail(4, "declared image size differs from file size");
    if (!std::equal(layout.begin(), layout.end(), bytes_.begin()+8)) fail(8, "unsupported serialized ABI fingerprint");
    if (bytes_.size()%4) fail(4, "image size is not aligned");
    std::size_t cursor = 28;
    while (cursor < bytes_.size()) {
        require(cursor, 20);
        auto type = static_cast<ValueType>(bytes_[cursor+2]);
        auto name = word(cursor+4);
        auto result = parse(type, cursor+12, cursor+20, name, 0);
        symbols_.push_back({name, cursor, result.end, result.value});
        cursor = result.end;
    }
    if (cursor != bytes_.size()) fail(cursor, "symbols do not end at file boundary");
}
PrecompiledImage::Result PrecompiledImage::parse(ValueType type, std::size_t field, std::size_t cursor,
                                                std::uint32_t name, unsigned depth, bool component) {
    if (depth > 128) fail(field, "container nesting exceeds 128 levels");
    auto raw = word(field);
    auto id = values_.size();
    values_.push_back({type, name, raw, field, field, cursor, {}, {}, {}, {}, ValueType::None});
    if (!pointer_type(type)) return {id, cursor}; // Preserve unknown scalar types as opaque words.
    if (raw != cursor) fail(field, "value does not follow its packed header/table");
    reference(field, raw);
    values_[id].data_offset = raw;
    switch (type) {
    case ValueType::String: case ValueType::LocalString: {
        auto start = bytes_.begin()+cursor;
        auto end = std::find(start, bytes_.end(), 0);
        if (end == bytes_.end()) fail(cursor, "unterminated string");
        values_[id].text.assign(start, end);
        cursor = align4(static_cast<std::size_t>(end-bytes_.begin())+1);
        require(raw, cursor-raw); break;
    }
    case ValueType::Pair: case ValueType::Vector: {
        auto count = type == ValueType::Pair ? 2u : 3u;
        require(cursor, 4+count*4);
        for (unsigned i = 0; i < count; ++i) values_[id].coordinates.push_back(word(cursor+4+i*4));
        cursor += 4+count*4; break;
    }
    case ValueType::Script: cursor = script(id, cursor, component); break;
    case ValueType::Structure: {
        require(cursor, 8);
        auto head_field = cursor+4;
        auto next = word(head_field);
        cursor += 8;
        if (next) {
            if (next != cursor) fail(head_field, "structure head does not follow header");
            reference(head_field, next);
        }
        while (next) {
            require(next, 16);
            auto t = static_cast<ValueType>(bytes_[next+1]>>1);
            auto result = parse(t, next+8, next+16, word(next+4), depth+1, true);
            values_[id].children.push_back(result.value);
            cursor = result.end;
            auto link_field = next+12;
            next = word(link_field);
            if (next) {
                if (next != cursor) fail(link_field, "component chain is cyclic or not packed");
                reference(link_field, next);
            }
        }
        break;
    }
    case ValueType::Array: {
        require(cursor, 12);
        auto t = static_cast<ValueType>(bytes_[cursor+2]);
        auto count = word(cursor+4);
        auto table_field = cursor+8;
        auto slots = table_field;
        cursor += 12;
        values_[id].element_type = t;
        if (count > 1) {
            if (count > (bytes_.size()-cursor)/4) fail(table_field, "array count exceeds remaining data");
            slots = word(table_field);
            if (slots != cursor) fail(table_field, "array table does not follow header");
            reference(table_field, slots);
            cursor += std::size_t(count)*4;
        }
        if (t == ValueType::Script) fail(table_field, "script arrays have no defined packed payload layout");
        // Strings in arrays are packed together and padded only once, unlike
        // individual symbol/component strings. Handle them before parse().
        for (std::size_t i = 0; i < count; ++i) {
            auto slot = slots+i*4;
            Result child{};
            if (t == ValueType::String || t == ValueType::LocalString) {
                auto value = word(slot);
                if (value != cursor || value >= bytes_.size()) fail(slot, "invalid packed array string");
                // String starts can be unaligned, unlike all other references.
                references_.push_back({slot, value});
                auto end = std::find(bytes_.begin()+cursor, bytes_.end(), 0);
                if (end == bytes_.end()) fail(cursor, "unterminated array string");
                std::string text(bytes_.begin()+cursor, end);
                auto after = static_cast<std::size_t>(end-bytes_.begin())+1;
                auto child_id = values_.size();
                values_.push_back({t, 0, value, slot, value, after, {}, std::move(text), {}, {}, ValueType::None});
                child = {child_id, after};
            } else child = parse(t, slot, cursor, 0, depth+1);
            values_[id].children.push_back(child.value);
            cursor = child.end;
        }
        auto end = align4(cursor); require(cursor, end-cursor); cursor = end;
        break;
    }
    default: break;
    }
    values_[id].end_offset = cursor;
    return {id, cursor};
}
std::size_t PrecompiledImage::script(std::size_t value, std::size_t offset, bool component) {
    std::size_t end;
    std::uint32_t checksum = 0, stored = 0;
    Bytes code;
    bool compressed = false;
    if (component) {
        auto pos = offset;
        for (;;) {
            auto t = read_token(bytes_, pos);
            if (!t.opcode) fail(pos, "script component reaches EOF without endscript");
            pos += t.size;
            if (t.opcode == 0x24) break;
        }
        if (pos < bytes_.size() && bytes_[pos] == 2) pos += read_token(bytes_, pos).size;
        code.assign(bytes_.begin()+offset, bytes_.begin()+pos);
        stored = static_cast<std::uint32_t>(code.size()); end = align4(pos);
    } else {
        require(offset, 12);
        checksum = word(offset);
        auto size = word(offset+4); stored = word(offset+8);
        if (!size || !stored || stored > size) fail(offset, "invalid script block sizes");
        require(offset+12, align4(stored));
        auto start = bytes_.begin()+offset+12;
        if (stored < size) {
            compressed = true;
            PreDecoder decoder; // Same valid-stream LZSS variant, with explicit state.
            code = decoder.decode(std::span<const std::uint8_t>(bytes_).subspan(offset+12, stored), size);
        } else code.assign(start, start+stored);
        end = offset+12+align4(stored);
    }
    require(offset, end-offset);
    // Wrap the stored body in a synthetic script header/EOF so the existing
    // bounded token/control-target inspector can validate it without execution.
    Bytes wrapped{0x23, 0x16, 0, 0, 0, 0};
    wrapped.insert(wrapped.end(), code.begin(), code.end()); wrapped.push_back(0);
    try {
        Document doc(std::move(wrapped));
        if (doc.scripts().size() != 1) fail(offset, "script block has extra definitions");
        for (const auto& jump : doc.jumps())
            if (jump.target < 6 || jump.target >= code.size()+6) fail(offset, "script jump escapes stored body");
        auto after = doc.scripts()[0].end_offset;
        if (after < code.size()+6) {
            auto t = read_token(doc.bytes(), after);
            if (t.opcode != 2 || after+t.size != code.size()+6) fail(offset, "unexpected tokens after endscript");
        }
        auto computed = doc.scripts()[0].checksum;
        if (!component && computed != checksum) fail(offset, "stored script checksum mismatch");
        values_[value].script = scripts_.size();
        scripts_.push_back({value, offset, doc.tokens().size()-3, computed, stored, compressed, component, std::move(code)});
    } catch (const std::exception& e) { fail(offset, "invalid stored script: " + std::string(e.what())); }
    return end;
}
std::string PrecompiledImage::describe(std::size_t id) const {
    const auto& v = values_.at(id);
    std::ostringstream out; out << value_type_name(v.type);
    if (v.type == ValueType::Integer) out << ' ' << std::bit_cast<std::int32_t>(v.raw);
    else if (v.type == ValueType::Float) out << ' ' << std::setprecision(9) << std::bit_cast<float>(v.raw);
    else if (v.type == ValueType::String || v.type == ValueType::LocalString) {
        out << " \"";
        for (unsigned char c : v.text) {
            if (c == '"' || c == '\\') out << '\\' << char(c);
            else if (c >= 32 && c < 127) out << char(c);
            else out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        }
        out << '"';
    } else if (!v.coordinates.empty()) {
        for (auto n : v.coordinates) out << ' ' << std::setprecision(9) << std::bit_cast<float>(n);
    } else if (v.type == ValueType::Array) out << '<' << value_type_name(v.element_type) << "> count=" << v.children.size();
    else if (v.type == ValueType::Structure) out << " components=" << v.children.size();
    else if (v.script) {
        const auto& s = scripts_[*v.script];
        out << " decoded=" << s.code.size() << " stored=" << s.stored_size << " crc=0x" << std::hex << s.checksum;
    } else out << " 0x" << std::hex << v.raw;
    return out.str();
}
} // namespace thug2::qb
