#pragma once
#include "thug2/core.hpp"
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>

namespace thug2 {
using MountId = std::uint64_t;
enum class AssetSource { Archive, Directory };
struct AssetInfo {
    MountId mount;
    AssetSource kind;
    std::filesystem::path source; // Canonical archive path or directory root.
    std::string name; // Actual entry name, preserving its case.
    std::size_t size;
};

// An owning, read-only file. Its bytes and cursor survive unmounting the source.
class AssetFile {
public:
    AssetFile(AssetFile&& other) noexcept
        : info_(std::move(other.info_)), bytes_(std::move(other.bytes_)),
          position_(std::exchange(other.position_, 0)) {}
    AssetFile& operator=(AssetFile&& other) noexcept {
        if (this != &other) {
            info_ = std::move(other.info_);
            bytes_ = std::move(other.bytes_);
            position_ = std::exchange(other.position_, 0);
        }
        return *this;
    }
    const AssetInfo& info() const { return info_; }
    std::span<const std::uint8_t> bytes() const { return bytes_; }
    std::size_t size() const { return bytes_.size(); }
    std::size_t tell() const { return position_; }
    bool eof() const { return position_ == size(); }
    std::size_t read(std::span<std::uint8_t> output);
    void seek(std::size_t position); // Absolute, 0..size inclusive.
private:
    friend class AssetStore;
    AssetFile(AssetInfo info, Bytes bytes) : info_(std::move(info)), bytes_(std::move(bytes)) {}
    AssetInfo info_;
    Bytes bytes_;
    std::size_t position_ = 0;
};

// Read-only mount table. First mounted source wins; errors in a selected
// source propagate instead of silently falling through to a different asset.
// Mounts are immutable snapshots. Loose contents are read on open; remount
// after adding/removing files. No global state or shared decoder cursor.
class AssetStore {
public:
    MountId mount_archive(const std::filesystem::path& path);
    MountId mount_directory(const std::filesystem::path& path);
    bool unmount(MountId id);
    std::size_t mount_count() const { return mounts_.size(); }
    std::optional<AssetInfo> find(std::string_view name) const;
    std::optional<AssetFile> open(std::string_view name) const;
private:
    struct ArchiveMount {
        Archive archive;
        std::unordered_map<std::uint32_t, std::size_t> index;
    };
    struct LooseEntry { std::filesystem::path path; std::size_t size; };
    struct DirectoryMount { std::unordered_map<std::string, LooseEntry> index; };
    struct Mount {
        MountId id;
        std::filesystem::path source;
        std::variant<ArchiveMount, DirectoryMount> data;
    };
    struct Match {
        const Mount* mount;
        std::size_t archive_index;
        const LooseEntry* loose;
        AssetInfo info;
    };
    std::optional<Match> resolve(std::string_view name) const;
    std::vector<Mount> mounts_;
    MountId next_id_ = 1;
};
} // namespace thug2
