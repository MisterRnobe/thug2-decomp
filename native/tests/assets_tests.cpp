#include "thug2/assets.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected exception");
}
void put(thug2::Bytes& b, std::uint32_t n, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) b.push_back(static_cast<std::uint8_t>(n >> (i*8)));
}
struct Entry { std::string name, stored; std::uint32_t decoded_size; bool compressed = false; };
void write(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    check(bool(out), "Fixture write failed");
}
void text(const std::filesystem::path& path, std::string_view data) {
    write(path, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
}
void archive(const std::filesystem::path& path, std::initializer_list<Entry> entries) {
    thug2::Bytes b;
    put(b, 0); put(b, 0xabcd0003); put(b, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        put(b, e.decoded_size); put(b, e.compressed ? static_cast<std::uint32_t>(e.stored.size()) : 0);
        put(b, static_cast<std::uint32_t>(e.name.size()+1), 2); put(b, 0, 2); put(b, 0);
        b.insert(b.end(), e.name.begin(), e.name.end()); b.push_back(0);
        b.insert(b.end(), e.stored.begin(), e.stored.end());
        for (std::size_t n = e.stored.size(); n % 4; ++n) b.push_back(0);
    }
    for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(b.size() >> (i*8));
    write(path, b);
}
std::string content(const thug2::AssetFile& file) {
    return {file.bytes().begin(), file.bytes().end()};
}
}
int main() {
    try {
        auto root = std::filesystem::temp_directory_path() /
            ("thug2-assets-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "loose" / "dir");
        struct Cleanup {
            std::filesystem::path root;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
        } cleanup{root};
        archive(root / "one.prx", {{"Dir\\Item.bin", "\x07" "abc", 3, true},
                                   {"DIR/ITEM.BIN", "ignored", 7}, {"empty", "", 0}});
        archive(root / "two.prx", {{"dir/item.bin", "second", 6}, {"second-only", "2", 1}});
        text(root / "loose" / "dir" / "ITEM.BIN", "disk");
        text(root / "loose" / "LooseOnly.TXT", "loose");
        text(root / "loose" / "zero", "");
        thug2::AssetStore store;
        check(!store.find("missing") && !store.open("missing"), "Empty store lookup");
        auto first = store.mount_archive(root / "one.prx");
        auto second = store.mount_archive(root / "two.prx");
        auto disk = store.mount_directory(root / "loose");
        check(first != second && second != disk && store.mount_count() == 3, "Mount IDs");
        auto info = store.find("dIR/item.BIN");
        check(info && info->mount == first && info->kind == thug2::AssetSource::Archive && info->size == 3,
              "First archive wins");
        auto a = store.open("DIR/ITEM.BIN"), b = store.open("dir\\item.bin");
        check(a && b && content(*a) == "abc", "Compressed duplicate first-wins");
        std::array<std::uint8_t, 8> buffer{};
        check(a->read(std::span(buffer).first(2)) == 2 && buffer[0] == 'a' && buffer[1] == 'b', "Partial read");
        check(a->tell() == 2 && b->tell() == 0, "Independent cursors");
        check(a->read(buffer) == 1 && buffer[0] == 'c' && a->eof(), "Short read at EOF");
        check(a->read(buffer) == 0 && a->tell() == 3, "EOF read");
        a->seek(1); check(a->read({}) == 0 && a->tell() == 1, "Empty read");
        rejects([&] { a->seek(4); }); check(a->tell() == 1, "Failed seek changed cursor");
        a->seek(3); check(a->eof(), "Seek to EOF");
        check(store.open("empty")->eof() && store.open("zero")->eof(), "Empty assets");
        check(store.open("SECOND-ONLY")->info().mount == second, "Second archive fallback");
        check(content(*store.open("looseonly.txt")) == "loose", "Loose fallback/case folding");
        check(store.find("LOOSEONLY.TXT")->kind == thug2::AssetSource::Directory, "Loose provenance");
        check(store.unmount(first) && !store.unmount(first), "Unmount behavior");
        check(content(*store.open("Dir/Item.bin")) == "second", "Unmount reveals second archive");
        check(content(*a) == "abc", "Open file invalidated by unmount");
        a->seek(2);
        thug2::AssetFile moved(std::move(*a));
        check(moved.tell() == 2 && moved.read(buffer) == 1 && buffer[0] == 'c', "Moved file lost cursor");
        check(a->tell() == 0 && a->read({}) == 0, "Moved-from cursor invalid");
        *b = std::move(moved);
        check(b->tell() == 3 && content(*b) == "abc" && moved.tell() == 0, "Move assignment lost file");
        check(store.unmount(second) && content(*store.open("DIR\\item.bin")) == "disk", "Unmount reveals loose file");
        auto remounted = store.mount_archive(root / "one.prx");
        check(remounted > disk && content(*store.open("dir/item.bin")) == "disk", "Remount appends priority");
        for (auto name : {"", "../x", "a/../x", "/x", "C:\\x", "a//b", "a/"}) {
            rejects([&] { store.find(name); }); rejects([&] { store.open(name); });
        }
        auto count = store.mount_count();
        rejects([&] { store.mount_archive(root / "absent.prx"); });
        rejects([&] { store.mount_directory(root / "one.prx"); });
        check(store.mount_count() == count, "Failed mount changed table");
        text(root / "loose" / "LooseOnly.TXT", "different size");
        rejects([&] { store.open("looseonly.txt"); });
        text(root / "loose" / "new", "new");
        check(!store.find("new"), "Directory index is not a snapshot");
        store.unmount(disk);
        store.mount_directory(root / "loose");
        check(content(*store.open("new")) == "new", "Remount does not refresh directory");
        // A broken higher-priority asset must not silently select lower data.
        archive(root / "bad.prx", {{"dir/item.bin", std::string("\0\0", 2), 3, true}});
        thug2::AssetStore bad;
        bad.mount_archive(root / "bad.prx"); bad.mount_archive(root / "two.prx");
        rejects([&] { bad.open("dir/item.bin"); });
#ifndef _WIN32
        text(root / "outside", "outside");
        std::filesystem::create_symlink(root / "outside", root / "loose" / "linked");
        thug2::AssetStore links;
        links.mount_directory(root / "loose");
        check(!links.find("linked"), "Symlink imported by directory scan");
        std::filesystem::remove(root / "loose" / "new");
        std::filesystem::create_symlink(root / "outside", root / "loose" / "new");
        rejects([&] { links.open("new"); });
#endif
        std::cout << "Asset priority, fallback, snapshots, cursors, errors and lifetimes passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
