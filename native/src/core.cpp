// Format and valid-stream behavior: thug2-decomp game/file/pre.cpp and
// game/core/crc.cpp, upstream 2d82ed2. See ../NOTICE.md for provenance.
#include "thug2/core.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace thug2 {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
std::uint32_t step(std::uint32_t crc, std::uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    return crc;
}
std::uint32_t le(std::span<const std::uint8_t> b, std::size_t off, unsigned count) {
    if (off > b.size() || count > b.size() - off) fail("Truncated PRX header");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint32_t(b[off+i]) << (8*i);
    return value;
}
void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) fail("Cannot create extracted file");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) fail("Cannot write extracted file");
}
} // namespace

std::uint32_t extend_crc(std::uint32_t seed, std::span<const std::uint8_t> bytes) {
    for (auto c : bytes) seed = step(seed, c);
    return seed;
}
std::uint32_t buffer_crc(std::span<const std::uint8_t> bytes) {
    return bytes.empty() ? 0 : extend_crc(0xffffffffu, bytes);
}
std::uint32_t name_crc(std::string_view name) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '/') c = '\\';
        crc = step(crc, c);
    }
    return crc;
}

Bytes PreDecoder::decode(std::span<const std::uint8_t> input, std::size_t expected_size) {
    if (input.size() >= 0x2000000) fail("Compressed stream exceeds game's size limit");
    // Prevent a corrupt header from requesting a disproportionate allocation.
    if (expected_size > input.size() * 9) fail("Impossible decompressed size");
    auto ring = ring_; // Commit state only after successful validation.
    std::fill_n(ring.begin(), 4078, ' ');
    std::size_t r = 4078, pos = 0;
    unsigned flags = 0;
    Bytes out;
    out.reserve(expected_size);
    auto emit = [&](std::uint8_t byte) {
        if (out.size() == expected_size) fail("LZSS output exceeds entry size");
        out.push_back(byte);
        ring[r] = byte;
        r = (r + 1) & 4095;
    };
    while (pos < input.size()) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            flags = input[pos++] | 0xff00u;
            if (pos == input.size()) break; // Partial final flag group is legal.
        }
        if (flags & 1) emit(input[pos++]);
        else {
            if (input.size() - pos < 2) fail("Truncated LZSS reference");
            unsigned offset = input[pos] | ((input[pos+1] & 0xf0u) << 4);
            unsigned length = (input[pos+1] & 15u) + 3;
            pos += 2;
            for (unsigned k = 0; k < length; ++k) emit(ring[(offset+k) & 4095]);
        }
    }
    if (out.size() != expected_size) fail("LZSS output does not match entry size");
    ring_ = ring;
    return out;
}

Archive::Archive(Bytes bytes) : bytes_(std::move(bytes)) {
    const std::span<const std::uint8_t> b(bytes_);
    if (le(b, 0, 4) != b.size()) fail("PRX file size does not match header");
    if (le(b, 4, 4) != 0xabcd0003u) fail("Unsupported PRX version (expected 3)");
    auto count = le(b, 8, 4);
    if (count > (b.size()-12)/17) fail("Invalid PRX entry count");
    std::unordered_set<std::uint32_t> seen;
    std::size_t off = 12;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto size = le(b, off, 4), compressed = le(b, off+4, 4);
        auto names = le(b, off+8, 2), uses = le(b, off+10, 2);
        auto checksum = le(b, off+12, 4);
        if (size > 0x7fffffffu || compressed > 0x7fffffffu || !names || names > 0x7fff)
            fail("Invalid signed PRX entry size");
        off += 16;
        if (off > b.size() || names > b.size()-off) fail("Truncated PRX name");
        auto begin = b.begin() + off;
        auto end = std::find(begin, begin+names, 0);
        if (end == begin+names || end == begin) fail("Missing PRX filename");
        std::string name(begin, end);
        off += names;
        std::size_t stored_size = compressed ? compressed : size;
        auto padded = (stored_size+3) & ~std::size_t(3);
        if (padded > b.size()-off) fail("Truncated PRX payload");
        auto crc = name_crc(name);
        entries_.push_back({std::move(name), size, compressed, checksum, crc,
                            static_cast<std::uint16_t>(uses), off, seen.insert(crc).second});
        off += padded;
    }
    if (off != b.size()) fail("Unexpected trailing PRX data");
}
Archive Archive::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("Cannot open PRX file");
    Bytes bytes((std::istreambuf_iterator<char>(stream)), {});
    if (stream.bad()) fail("Cannot read PRX file");
    return Archive(std::move(bytes));
}
std::span<const std::uint8_t> Archive::stored(std::size_t index) const {
    const auto& e = entries_.at(index);
    return std::span<const std::uint8_t>(bytes_).subspan(e.data_offset, e.compressed_size ? e.compressed_size : e.size);
}
Bytes Archive::read(std::size_t index, PreDecoder& decoder) const {
    const auto& e = entries_.at(index);
    auto data = stored(index);
    if (e.compressed_size) return decoder.decode(data, e.size);
    return Bytes(data.begin(), data.end());
}

std::filesystem::path relative_path(std::string_view name) {
    if (name.empty()) fail("Empty output path");
    std::string portable(name);
    std::replace(portable.begin(), portable.end(), '\\', '/');
    if (portable.front() == '/' || portable.find(':') != std::string::npos ||
        portable.find('\0') != std::string::npos) fail("Absolute or invalid output path");
    std::filesystem::path result;
    std::size_t pos = 0;
    while (pos < portable.size()) {
        auto end = portable.find('/', pos);
        if (end == std::string::npos) end = portable.size();
        auto part = portable.substr(pos, end-pos);
        if (part.empty() || part == "." || part == "..") fail("Unsafe output path component");
        result /= part;
        pos = end+1;
    }
    if (portable.back() == '/') fail("Output path ends with separator");
    return result;
}
void extract(const Archive& archive, const std::filesystem::path& destination) {
    std::vector<std::filesystem::path> paths;
    std::unordered_set<std::string> seen;
    for (const auto& e : archive.entries()) {
        if (!e.active) { paths.emplace_back(); continue; }
        auto path = relative_path(e.name);
        if (!seen.insert(path.generic_string()).second) fail("Conflicting output paths");
        paths.push_back(std::move(path));
    }
    // Resolve caller-selected parent aliases (e.g. macOS /tmp -> /private/tmp).
    // Archive names cannot supply symlinks: the root is new and files are regular.
    auto absolute = std::filesystem::absolute(destination).lexically_normal();
    absolute = std::filesystem::canonical(absolute.parent_path()) / absolute.filename();
    if (!std::filesystem::create_directory(absolute)) fail("Extraction destination already exists");
    PreDecoder decoder;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].empty()) continue;
        auto out = absolute / paths[i];
        std::filesystem::create_directories(out.parent_path());
        write_file(out, archive.read(i, decoder));
    }
}
} // namespace thug2
