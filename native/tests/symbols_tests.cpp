#include "thug2/symbols.hpp"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using thug2::Bytes;
using thug2::qb::ResolveStatus;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void rejects(const std::function<void()>& fn) {
    bool caught=false; try { fn(); } catch (const std::exception&) { caught=true; }
    check(caught,"Expected failure");
}
void word(Bytes& b,std::uint32_t n) { for(unsigned i=0;i<4;++i)b.push_back(n>>(i*8)); }
Bytes value(std::uint32_t name, std::uint32_t raw, bool alias=false) {
    Bytes b{0x16};word(b,name);b.insert(b.end(),{7,static_cast<std::uint8_t>(alias?0x16:0x17)});
    word(b,raw);b.insert(b.end(),{1,0});return b;
}
Bytes image(std::uint32_t name,std::uint32_t raw,bool alias=false) {
    Bytes b{0x6f,0xef,0x75,0xb0,48,0,0,0,0x1c,8,2,4,0x10,4,8,0xc,0xc,8,2,4,0x14,2,4,0xc,0x10,0x10,0xc,0};
    word(b,(alias?13:1)<<16);word(b,name);word(b,0);word(b,raw);word(b,0);return b;
}
}
int main() {
    try {
        thug2::qb::SymbolTable table;
        auto a=thug2::name_crc("A"),b=thug2::name_crc("B"),c=thug2::name_crc("C");
        check(table.resolve(a).status==ResolveStatus::Missing,"Missing root");
        auto one=table.load_bytes("one",value(a,b,true));
        check(table.resolve(a).path==std::vector<std::uint32_t>{a,b},"Missing target trace");
        check(table.resolve(a).last_alias->name==a,"Original missing-target alias fallback lost");
        auto two=table.load_bytes("two",image(b,c,true));
        auto three=table.load_bytes("three",value(c,42));
        auto result=table.resolve(a);
        check(result.status==ResolveStatus::Resolved && result.target->name==c && result.path.size()==3,"Mixed-format chain");
        check(table.resolve(c,0).status==ResolveStatus::Resolved && table.resolve(a,1).status==ResolveStatus::Limit,"Hop limit");
        auto snapshot=result.target;
        auto replacement=table.load_bytes("replacement",image(c,99));
        check(table.find(c)->module->id==replacement && snapshot->module->id==three,"Replacement/snapshot ownership");
        check(table.unload(three) && table.find(c)->module->id==replacement,"Old unload removed new definition");
        check(table.unload(replacement) && !table.find(c),"Unload restored overwritten definition");
        check(snapshot->module->name=="three","Snapshot lost module lifetime");
        auto cycle=table.load_bytes("cycle",value(c,a,true));
        check(table.resolve(a).status==ResolveStatus::Cycle && table.resolve(a).path.back()==a,"Cycle detection");
        table.unload(cycle);
        auto self=table.load_bytes("self",value(c,c,true));
        check(table.resolve(c).status==ResolveStatus::Cycle,"Self alias");
        table.unload(self);
        auto duplicate=value(c,1);duplicate.pop_back();auto later=value(c,2);
        duplicate.insert(duplicate.end(),later.begin(),later.end());
        auto dup=table.load_bytes("duplicate",duplicate);
        check(table.find(c)->module->id==dup && table.find(c)->index==1,"Within-file source order");
        // A multi-token expression starting with a name must not become an alias.
        auto expression=value(c,a,true);expression.resize(expression.size()-2);
        expression.push_back(0x0b);expression.push_back(0x17);word(expression,1);expression.insert(expression.end(),{1,0});
        table.load_bytes("expression",expression);
        check(!table.find(c)->alias && table.find(c)->type==thug2::qb::ValueType::None,"Expression misclassified as alias");
        // A named function inside a structure is not a global function.
        Bytes embedded{3,0x23,0x16};word(embedded,123);embedded.insert(embedded.end(),{1,0x24,4,0});
        table.load_bytes("embedded",embedded);check(!table.find(123),"Embedded function exported globally");
        Bytes script{0x23,0x16};word(script,456);script.insert(script.end(),{1,0x24,0});
        auto script_owner=table.load_bytes("script-original",script);
        auto script_reload=table.load_bytes("script-reload",script);
        check(table.find(456)->module->id==script_owner,"Unchanged token script lost original ownership");
        table.unload(script_reload);check(table.find(456)->module->id==script_owner,"Reload unload removed reused script");
        table.unload(script_owner);check(!table.find(456),"Original script unload failed");
        auto count=table.modules().size();rejects([&]{table.load_bytes("bad",Bytes{0x17});});
        check(table.modules().size()==count,"Bad load mutated modules");
        auto root=std::filesystem::temp_directory_path()/
            ("thug2-symbols-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
        struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}} cleanup{root};
        for(const auto& [name,bytes]:std::vector<std::pair<std::string,Bytes>>{{"good.qb",value(777,0)},{"bad.qb",{0xff}}}) {
            std::ofstream out(root/name,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        }
        thug2::AssetStore assets;assets.mount_directory(root);
        std::vector<std::string> batch{"good.qb","bad.qb"};
        rejects([&]{table.load_batch(assets,batch);});
        check(!table.find(777) && table.modules().size()==count,"Batch was partially published");
        auto loaded=table.load(assets,"good.qb");
        check(table.find(777)->module->origin.has_value(),"Asset provenance missing");
        check(loaded==table.modules().back()->id && loaded>two && two>one,"Stable load IDs");
        check(!table.unload(999999),"Unknown unload");
        table.clear();check(table.definitions().empty() && table.modules().empty(),"Clear");
        check(snapshot->name==c && snapshot->module->name=="three","Snapshot invalid after clear");
        check(table.load_bytes("again",value(a,0))>loaded,"IDs reused after clear");
        std::cout<<"Symbol order, aliases, cycles, unload, snapshots and atomic loads passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
