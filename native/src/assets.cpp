#include "thug2/assets.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace thug2 {
namespace {
std::string key(std::string_view name) {
    auto result = relative_path(name).generic_string();
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
std::size_t loose_size(const std::filesystem::path& path) {
    auto n = std::filesystem::file_size(path);
    if (n > std::numeric_limits<std::size_t>::max() ||
        n > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("Loose file is too large: " + path.string());
    return static_cast<std::size_t>(n);
}
bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto r = root.begin(), p = path.begin();
    for (; r != root.end(); ++r, ++p) if (p == path.end() || *p != *r) return false;
    return p != path.end();
}
} // namespace

std::size_t AssetFile::read(std::span<std::uint8_t> output) {
    auto count = std::min(output.size(), size()-position_);
    std::copy_n(bytes_.begin()+position_, count, output.begin());
    position_ += count;
    return count;
}
void AssetFile::seek(std::size_t position) {
    if (position > size()) throw std::out_of_range("Asset seek exceeds file size");
    position_ = position;
}

MountId AssetStore::mount_archive(const std::filesystem::path& path) {
    auto source = std::filesystem::canonical(path);
    ArchiveMount data{Archive::load(source), {}};
    for (std::size_t i = 0; i < data.archive.entries().size(); ++i) {
        const auto& e = data.archive.entries()[i];
        key(e.name); // Same path validation for both source types.
        if (e.active) data.index.emplace(e.name_checksum, i);
    }
    if (!next_id_) throw std::overflow_error("Mount IDs exhausted");
    mounts_.push_back({next_id_, std::move(source), std::move(data)});
    return next_id_++;
}
MountId AssetStore::mount_directory(const std::filesystem::path& path) {
    auto source = std::filesystem::canonical(path);
    if (!std::filesystem::is_directory(source)) throw std::runtime_error("Not a directory: " + source.string());
    DirectoryMount data;
    for (const auto& e : std::filesystem::recursive_directory_iterator(source)) {
        // Do not import linked files/directories into the mount snapshot.
        if (e.is_symlink() || !e.is_regular_file()) continue;
        auto relative = e.path().lexically_relative(source);
        auto name = key(relative.generic_string());
        if (!data.index.emplace(name, LooseEntry{relative, loose_size(e.path())}).second)
            throw std::runtime_error("Ambiguous case-insensitive loose path: " + relative.string());
    }
    if (!next_id_) throw std::overflow_error("Mount IDs exhausted");
    mounts_.push_back({next_id_, std::move(source), std::move(data)});
    return next_id_++;
}
bool AssetStore::unmount(MountId id) {
    auto found = std::find_if(mounts_.begin(), mounts_.end(), [=](const Mount& m) { return m.id == id; });
    if (found == mounts_.end()) return false;
    mounts_.erase(found);
    return true;
}
std::optional<AssetStore::Match> AssetStore::resolve(std::string_view name) const {
    auto normalized = key(name);
    auto crc = name_crc(normalized);
    for (const auto& m : mounts_) {
        if (auto data = std::get_if<ArchiveMount>(&m.data)) {
            auto found = data->index.find(crc);
            if (found == data->index.end()) continue;
            const auto& entry = data->archive.entries()[found->second];
            return Match{&m, found->second, nullptr,
                         {m.id, AssetSource::Archive, m.source, entry.name, entry.size}};
        } else {
            const auto& directory = std::get<DirectoryMount>(m.data);
            auto found = directory.index.find(normalized);
            if (found == directory.index.end()) continue;
            return Match{&m, 0, &found->second,
                         {m.id, AssetSource::Directory, m.source, found->second.path.generic_string(), found->second.size}};
        }
    }
    return std::nullopt;
}
std::optional<AssetInfo> AssetStore::find(std::string_view name) const {
    auto match = resolve(name);
    if (!match) return std::nullopt;
    return match->info;
}
std::optional<AssetFile> AssetStore::open(std::string_view name) const {
    auto match = resolve(name);
    if (!match) return std::nullopt;
    Bytes bytes;
    if (auto data = std::get_if<ArchiveMount>(&match->mount->data)) {
        PreDecoder decoder; // Independent opens have independent decoder state.
        bytes = data->archive.read(match->archive_index, decoder);
    } else {
        auto path = std::filesystem::canonical(match->mount->source / match->loose->path);
        if (!inside(match->mount->source, path)) throw std::runtime_error("Loose file escapes mounted root");
        if (loose_size(path) != match->info.size) throw std::runtime_error("Loose file size changed; remount directory");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open loose file: " + path.string());
        bytes.resize(match->info.size);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.peek() != std::char_traits<char>::eof() || stream.bad())
            throw std::runtime_error("Cannot read loose file or file changed during read: " + path.string());
    }
    return AssetFile(std::move(match->info), std::move(bytes));
}
} // namespace thug2
