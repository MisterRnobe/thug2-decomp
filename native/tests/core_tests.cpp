#include "thug2/core.hpp"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok) { if (!ok) throw std::runtime_error("Check failed"); }
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
thug2::Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }
void put(thug2::Bytes& b, std::uint32_t n, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) b.push_back(static_cast<std::uint8_t>(n >> (i*8)));
}
thug2::Bytes fixture() {
    thug2::Bytes b;
    put(b, 60); put(b, 0xabcd0003); put(b, 2);
    for (auto name : {"A", "a"}) {
        put(b, 3); put(b, 0); put(b, 4, 2); put(b, 0, 2); put(b, 123);
        b.insert(b.end(), {static_cast<std::uint8_t>(*name), 0, 99, 99});
        b.insert(b.end(), {'a', 'b', 'c', 99});
    }
    return b;
}
}
int main() {
    try {
        check(thug2::buffer_crc(bytes("123456789")) == 0x340bc6d9);
        check(thug2::buffer_crc({}) == 0);
        check(thug2::name_crc("") == 0xffffffff);
        check(thug2::name_crc("Models/A.XBX") == thug2::name_crc("models\\a.xbx"));
        check(thug2::extend_crc(thug2::extend_crc(0xffffffff, bytes("1234")), bytes("56789")) == 0x340bc6d9);
        thug2::PreDecoder d;
        check(d.decode({thug2::Bytes{7, 'a', 'b', 'c'}}, 3) == bytes("abc"));
        // Initial space window, then overlapping copy starting at position 4078.
        check(d.decode(thug2::Bytes{0, 0, 0}, 3) == bytes("   "));
        check(d.decode(thug2::Bytes{1, 'x', 0xee, 0xff}, 19) == bytes(std::string(19, 'x')));
        // A new call resets the prefix but retains the final 18 window bytes.
        check(d.decode(thug2::Bytes{0, 0xee, 0xf0}, 3) == bytes("xxx"));
        thug2::PreDecoder fresh;
        check(fresh.decode(thug2::Bytes{0, 0xee, 0xf0}, 3) == thug2::Bytes(3, 0));
        // Enough literals to wrap the ring, then a reference crossing its end.
        thug2::Bytes encoded, expected;
        for (unsigned i = 0; i < 512; ++i) {
            encoded.push_back(255);
            for (unsigned j = 0; j < 8; ++j) {
                auto c = static_cast<std::uint8_t>(i*8+j);
                encoded.push_back(c); expected.push_back(c);
            }
        }
        encoded.insert(encoded.end(), {0, 0xfe, 0xf0});
        expected.insert(expected.end(), {16, 17, 18});
        check(fresh.decode(encoded, expected.size()) == expected);
        rejects([&] { d.decode(thug2::Bytes{0, 0}, 3); });
        rejects([&] { d.decode(thug2::Bytes{1, 'a'}, 0); });
        rejects([&] { d.decode(thug2::Bytes{1, 'a'}, 2); });
        auto b = fixture();
        thug2::Archive archive(b);
        check(archive.entries().size() == 2);
        check(archive.entries()[0].active && !archive.entries()[1].active);
        check(archive.entries()[0].stored_checksum == 123); // Not a name CRC.
        check(archive.read(0, d) == bytes("abc"));
        for (std::size_t n = 0; n < b.size(); ++n) {
            auto truncated = thug2::Bytes(b.begin(), b.begin()+n);
            rejects([&] { thug2::Archive bad(truncated); });
        }
        auto bad = b; bad[4] = 0; rejects([&] { thug2::Archive a(bad); });
        bad = b; bad[30] = 1; bad[29] = 1; bad[28] = 1; bad[31] = 1;
        rejects([&] { thug2::Archive a(bad); }); // No terminating NUL.
        check(thug2::relative_path("levels\\AP\\AP.col.xbx").generic_string() == "levels/AP/AP.col.xbx");
        for (auto name : {"../x", "/tmp/x", "C:\\x", "\\\\host\\x", "a/../x", "a//x", "a/", "./x", ""})
            rejects([&] { thug2::relative_path(name); });
        auto temp = std::filesystem::temp_directory_path() /
            ("thug2-core-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(temp);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        } cleanup{temp};
        thug2::extract(archive, temp / "out");
        std::ifstream extracted(temp / "out" / "A", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(extracted)), {});
        check(content == "abc");
        rejects([&] { thug2::extract(archive, temp / "out"); });
#ifndef _WIN32
        std::filesystem::create_directory_symlink(temp / "out", temp / "link");
        rejects([&] { thug2::extract(archive, temp / "link"); });
        check(!std::filesystem::exists(temp / "out" / "nested"));
#endif
        std::cout << "CRC, window state/overlap/wrap, malformed streams, PRX bounds, duplicates and paths passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
