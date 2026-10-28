#include "thug2/qb.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void rejects(const std::function<void()>& fn) {
    bool caught = false;
    try { fn(); } catch (const std::exception&) { caught = true; }
    check(caught, "Expected parse failure");
}
void word(thug2::Bytes& b, std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) b.push_back(n >> (8*i)); }
void name(thug2::Bytes& b, std::uint32_t n) { b.push_back(0x16); word(b, n); }
thug2::Bytes script(bool numbered) {
    thug2::Bytes b{0x23}; name(b, thug2::name_crc("Example"));
    if (numbered) { b.push_back(2); word(b, 100); } else b.push_back(1);
    b.push_back(0x29); // return 42
    b.push_back(0x17); word(b, 42); b.insert(b.end(), {1, 0x24, 0});
    return b;
}
}
int main() {
    try {
        thug2::qb::Document empty(thug2::Bytes{0});
        check(empty.tokens().size() == 1, "Empty file EOF");
        auto raw = script(false);
        thug2::qb::Document d(raw), numbered(script(true));
        check(d.scripts().size() == 1 && d.scripts()[0].body_offset == 6, "Script header");
        check(d.scripts()[0].checksum == numbered.scripts()[0].checksum, "Line-independent checksum");
        thug2::Bytes body{0x29, 0x17}; word(body, 42);
        check(d.scripts()[0].checksum == thug2::extend_crc(0xffffffff, body), "Script checksum range");
        thug2::Bytes embedded{3}; embedded.insert(embedded.end(), raw.begin(), raw.end()-1);
        embedded.insert(embedded.end(), {4, 0});
        thug2::qb::Document component(embedded);
        check(component.scripts().size() == 1 && component.scripts()[0].container_depth == 1,
              "Embedded script component");
        thug2::Bytes globals;
        auto crc = thug2::name_crc("Settings");
        name(globals, crc); globals.insert(globals.end(), {7, 1, 3});
        name(globals, thug2::name_crc("items")); globals.insert(globals.end(), {7, 5, 0x17});
        word(globals, 3); globals.insert(globals.end(), {6, 4, 1, 0x2b}); word(globals, crc);
        const std::string text = "Settings";
        globals.insert(globals.end(), text.begin(), text.end()); globals.insert(globals.end(), {0, 0});
        thug2::qb::Document g(globals);
        check(g.symbols().size() == 1 && g.symbols()[0].name == crc, "Top-level symbol indexing");
        check(g.names().size() == 1 && g.label(crc) == "\"Settings\"", "Checksum names");
        check(g.label(0x12345678) == "0x12345678", "Unknown name fallback");
        thug2::Bytes adjacent; name(adjacent, 1); adjacent.insert(adjacent.end(), {7, 0x17}); word(adjacent, 2);
        name(adjacent, 3); adjacent.insert(adjacent.end(), {7, 0x17}); word(adjacent, 4);
        adjacent.push_back(0x2b); word(adjacent, 3); adjacent.insert(adjacent.end(), {'x', 0, 0});
        thug2::qb::Document packed_globals(adjacent);
        check(packed_globals.symbols().size() == 2 && packed_globals.symbols()[0].end_offset == 11 &&
              packed_globals.symbols()[1].end_offset == 22, "Globals without EOL and trailing checksum dictionary");
        thug2::qb::Document jump(thug2::Bytes{0x49, 2, 0, 0});
        check(jump.jumps().size() == 1 && jump.jumps()[0].target == 3, "Short jump base");
        check(jump.describe(0, 1) == "short_jump -> 0x00000002", "Relative dump addresses");
        thug2::Bytes weighted{0x2f}; word(weighted, 2);
        weighted.insert(weighted.end(), {1, 0, 2, 0}); word(weighted, 4); word(weighted, 0); weighted.push_back(0);
        thug2::qb::Document random(weighted);
        check(random.jumps().size() == 2 && random.jumps()[0].target == 17 && random.jumps()[1].target == 17,
              "Random table is weights followed by offsets");
        thug2::qb::Document back(thug2::Bytes{1, 0x2e, 0xfa, 0xff, 0xff, 0xff, 0});
        check(back.jumps()[0].target == 0, "Signed/wrapped long jump");
        thug2::qb::Document literals(thug2::Bytes{0x17, 0xff, 0xff, 0xff, 0xff,
            0x1a, 0, 0, 0x80, 0x3f, 0x1b, 4, 0, 0, 0, 'a', '\n', '"', 0, 0});
        check(literals.describe(0) == "integer -1", "Signed integer");
        check(literals.describe(1) == "float 1", "IEEE float");
        check(literals.describe(2) == "string \"a\\x0a\\\"\"", "Escaped text output");
        for (std::size_t n = 0; n < raw.size(); ++n) {
            auto truncated = thug2::Bytes(raw.begin(), raw.begin()+n);
            rejects([&] { thug2::qb::Document bad(truncated); });
        }
        for (auto bad : {thug2::Bytes{0x10, 0}, {0, 0}, {3, 6, 0}, {3, 0}, {0x24, 0},
                         {0x23, 0}, {0x49, 1, 0, 0}, {0x49, 255, 255, 0},
                         {0x1b, 0xff, 0xff, 0xff, 0xff, 0}, {0x1b, 1, 0, 0, 0, 'x', 0},
                         {0x2b, 1, 0, 0, 0, 'x'}, {0x2f, 0, 0, 0, 0, 0},
                         {0x2f, 0xff, 0xff, 0xff, 0xff, 0}, {0x6f, 0xef, 0x75, 0xb0}})
            rejects([&] { thug2::qb::Document doc(bad); });
        // Each payload form must reject every truncation within the token.
        for (auto data : {thug2::Bytes{2, 1, 0, 0, 0}, {0x1e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                         {0x1f, 0, 0, 0, 0, 0, 0, 0, 0}, {0x47, 2, 0}, {0x1b, 2, 0, 0, 0, 'x', 0},
                         {0x2b, 1, 0, 0, 0, 'x', 0}}) {
            check(thug2::qb::read_token(data, 0).size == data.size(), "Token width");
            for (std::size_t n = 0; n < data.size(); ++n)
                rejects([&] { thug2::qb::read_token(std::span(data).first(n), 0); });
        }
        std::cout << "QB token bounds, scripts, globals, names, jumps, CRC and literals passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
