#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace thug2 {
using Bytes = std::vector<std::uint8_t>;
// Game CRC: reflected polynomial 0xedb88320, no final XOR.
std::uint32_t extend_crc(std::uint32_t seed, std::span<const std::uint8_t> bytes);
std::uint32_t buffer_crc(std::span<const std::uint8_t> bytes);
std::uint32_t name_crc(std::string_view name); // ASCII lower case, '/' -> '\\'

// Pre::Decode's window is explicit, initially zero, and belongs to the caller.
// Each decode resets only the first 4078 bytes, as in the original game.
class PreDecoder {
public:
    Bytes decode(std::span<const std::uint8_t> input, std::size_t expected_size);
private:
    std::array<std::uint8_t, 4096> ring_{};
};

struct Entry {
    std::string name;
    std::uint32_t size, compressed_size, stored_checksum, name_checksum;
    std::uint16_t use_count;
    std::size_t data_offset;
    bool active; // The first entry with a name CRC wins, including hash collisions.
};

class Archive {
public:
    explicit Archive(Bytes bytes);
    static Archive load(const std::filesystem::path& path);
    const std::vector<Entry>& entries() const { return entries_; }
    std::span<const std::uint8_t> stored(std::size_t index) const;
    Bytes read(std::size_t index, PreDecoder& decoder) const;
private:
    Bytes bytes_;
    std::vector<Entry> entries_;
};

// Validates names before any extraction. Both game separators are accepted.
std::filesystem::path relative_path(std::string_view name);
// Destination must not exist. A new directory prevents accidental overwrites.
void extract(const Archive& archive, const std::filesystem::path& destination);
} // namespace thug2
