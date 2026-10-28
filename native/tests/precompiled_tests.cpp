#include "thug2/precompiled.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using thug2::Bytes;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void rejects(const std::function<void()>& fn) {
    bool caught = false; try { fn(); } catch (const std::exception&) { caught = true; }
    check(caught, "Expected image rejection");
}
void put(Bytes& b, std::uint32_t n) { for (unsigned i=0;i<4;++i) b.push_back(n>>(i*8)); }
void set(Bytes& b, std::size_t p, std::uint32_t n) { for (unsigned i=0;i<4;++i) b[p+i]=n>>(i*8); }
Bytes header() { return {0x6f,0xef,0x75,0xb0, 0,0,0,0, 0x1c,8,2,4,0x10,4,8,0xc,0xc,8,2,4,0x14,2,4,0xc,0x10,0x10,0xc,0}; }
Bytes finish(Bytes b) { set(b,4,static_cast<std::uint32_t>(b.size())); return b; }
std::size_t symbol(Bytes& b, unsigned type, std::uint32_t raw) {
    auto start=b.size(); put(b,type<<16);put(b,0x12345678);put(b,0);put(b,raw);put(b,0);return start;
}
void pad(Bytes& b) { while (b.size()%4) b.push_back(0); }
}
int main() {
    try {
        thug2::qb::PrecompiledImage empty(finish(header()));
        check(empty.symbols().empty(), "Empty image");
        auto b=header();symbol(b,1,0xffffffff);symbol(b,2,0x3f800000);symbol(b,13,0xaabbccdd);
        thug2::qb::PrecompiledImage scalars(finish(b));
        check(scalars.describe(0)=="integer -1" && scalars.describe(1)=="float 1", "Inline scalars");
        b=header();symbol(b,3,48);b.insert(b.end(),{'a',0,0,0});
        thug2::qb::PrecompiledImage string(finish(b));
        check(string.values()[0].text=="a" && string.references().size()==1, "String reference");
        b=header();symbol(b,5,48);put(b,0);put(b,0x3f800000);put(b,0x40000000);
        thug2::qb::PrecompiledImage pair(finish(b));check(pair.describe(0)=="pair 1 2", "Pair");
        b=header();symbol(b,12,48);put(b,3<<16);put(b,2);put(b,60);put(b,68);put(b,70);
        b.insert(b.end(),{'a',0,'b','b',0});pad(b);
        thug2::qb::PrecompiledImage strings(finish(b));
        check(strings.values()[0].children.size()==2 && strings.values()[2].text=="bb", "Unaligned array strings");
        b=header();symbol(b,10,48);put(b,0);put(b,56);
        put(b,1<<9);put(b,55);put(b,42);put(b,0);
        auto structure=finish(b);
        thug2::qb::PrecompiledImage object(structure);
        check(object.values()[0].children.size()==1 && object.values()[1].name==55 && object.values()[1].raw==42,
              "Component field layout");
        auto bad=structure;set(bad,68,56);rejects([&]{thug2::qb::PrecompiledImage x(bad);}); // Linked-list cycle.
        bad=structure;set(bad,52,1000);rejects([&]{thug2::qb::PrecompiledImage x(bad);});
        for(std::size_t n=0;n<structure.size();++n) {
            auto cut=Bytes(structure.begin(),structure.begin()+n);
            rejects([&]{thug2::qb::PrecompiledImage x(cut);});
            // Repair the outer size too, so aligned cuts reach deep bounds checks.
            if (n>28 && n%4==0) rejects([&]{thug2::qb::PrecompiledImage x(finish(cut));});
        }
        bad=structure;bad[21]=1;rejects([&]{thug2::qb::PrecompiledImage x(bad);}); // Other ABI.
        bad=structure;set(bad,40,0xfffffff0);rejects([&]{thug2::qb::PrecompiledImage x(bad);});
        // Empty and singleton arrays use the inline slot, not an external table.
        b=header();symbol(b,12,48);put(b,1<<16);put(b,1);put(b,99);
        thug2::qb::PrecompiledImage singleton(finish(b));check(singleton.values()[1].raw==99,"Singleton array");
        set(b,52,0xffffffff);rejects([&]{thug2::qb::PrecompiledImage x(finish(b));});
        b=header();symbol(b,12,48);put(b,0);put(b,0);put(b,0);
        thug2::qb::PrecompiledImage zero(finish(b));check(zero.values()[0].children.empty(),"Empty array");
        // Script symbol with raw body. CRC excludes EOL and endscript.
        b=header();symbol(b,7,48);put(b,0xffffffff);put(b,2);put(b,2);b.insert(b.end(),{1,0x24});pad(b);
        thug2::qb::PrecompiledImage code(finish(b));
        check(code.scripts().size()==1 && !code.scripts()[0].compressed && code.scripts()[0].token_count==2,"Raw script");
        bad=finish(b);set(bad,48,0);rejects([&]{thug2::qb::PrecompiledImage x(bad);});
        bad=finish(b);bad[61]=0;rejects([&]{thug2::qb::PrecompiledImage x(bad);});
        // Reject a displacement that lands on the inspector's synthetic header.
        Bytes escaped{0x2e,0xf5,0xff,0xff,0xff};
        b=header();symbol(b,7,48);put(b,thug2::extend_crc(0xffffffff,escaped));put(b,6);put(b,6);
        b.insert(b.end(),escaped.begin(),escaped.end());b.push_back(0x24);pad(b);
        rejects([&]{thug2::qb::PrecompiledImage x(finish(b));});
        // Script components have no 12-byte block header.
        b=header();symbol(b,10,48);put(b,0);put(b,56);
        put(b,7<<9);put(b,55);put(b,72);put(b,0);b.insert(b.end(),{1,0x24});pad(b);
        thug2::qb::PrecompiledImage component(finish(b));
        check(component.scripts().size()==1 && component.scripts()[0].component,"Component script layout");
        b=header();symbol(b,10,48);
        for(unsigned n=0;n<131;++n) {
            auto p=b.size();put(b,0);put(b,static_cast<std::uint32_t>(p+8));
            put(b,10<<9);put(b,n);put(b,static_cast<std::uint32_t>(p+24));put(b,0);
        }
        put(b,0);put(b,0);rejects([&]{thug2::qb::PrecompiledImage x(finish(b));});
        std::cout<<"QB image layouts, values, arrays, links, scripts, corruption and depth limits passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
