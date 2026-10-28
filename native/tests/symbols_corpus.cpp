#include "thug2/symbols.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Expected {
    thug2::qb::ModuleId module;std::size_t offset;std::optional<std::uint32_t> alias;
    std::optional<std::uint32_t> checksum;bool token_script=false;
};
}
int main(int argc,char** argv){
    try{
        using namespace thug2::qb;
        if(argc<2 || argc>4)throw std::runtime_error("Usage: symbols-corpus DATA_ROOT [REPORT.json [TOPOLOGY.json]]");
        auto root=std::filesystem::canonical(argv[1]);
        std::map<std::string,std::string> catalog;
        auto add=[&](std::string name){
            auto normalized=name;
            for(char& c:normalized){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';}
            if(normalized.ends_with(".qb")||normalized.ends_with(".qb.xbx"))catalog.emplace(normalized,name);
        };
        std::vector<std::filesystem::path> pres;
        for(const auto& e:std::filesystem::directory_iterator(root/"pre"))if(e.path().extension()==".prx")pres.push_back(e.path());
        std::sort(pres.begin(),pres.end());
        thug2::AssetStore assets;
        for(const auto& p:pres){assets.mount_archive(p);auto archive=thug2::Archive::load(p);for(const auto& e:archive.entries())add(e.name);}
        assets.mount_directory(root);
        for(const auto& e:std::filesystem::recursive_directory_iterator(root))
            if(!e.is_symlink()&&e.is_regular_file())add(e.path().lexically_relative(root).generic_string());
        std::vector<std::string> files;for(const auto& [key,name]:catalog){(void)key;files.push_back(name);}
        check(!files.empty(),"No QB modules");
        SymbolTable table;auto ids=table.load_batch(assets,files);
        std::map<std::uint32_t,Expected> expected;
        std::size_t events=0,tokens=0,images=0,reused=0;
        for(const auto& module:table.modules()){
            std::vector<std::pair<std::uint32_t,Expected>> ordered;
            if(auto doc=std::get_if<Document>(&module->data)){
                ++tokens;
                for(const auto& s:doc->symbols()){
                    std::optional<std::uint32_t> alias;
                    auto b=doc->bytes();
                    if(s.end_offset-s.value_offset==5 && b[s.value_offset]==0x16){
                        auto p=s.value_offset+1;
                        alias=std::uint32_t(b[p])|std::uint32_t(b[p+1])<<8|std::uint32_t(b[p+2])<<16|std::uint32_t(b[p+3])<<24;
                    }
                    ordered.push_back({s.name,{module->id,s.offset,alias,{}}});
                }
                for(const auto& s:doc->scripts())if(!s.container_depth)ordered.push_back({s.name,{module->id,s.offset,{},s.checksum,true}});
            }else{
                ++images;const auto& image=std::get<PrecompiledImage>(module->data);
                for(const auto& s:image.symbols()){
                    const auto& v=image.values()[s.value];
                    ordered.push_back({s.name,{module->id,s.offset,v.type==ValueType::Name?std::optional(v.raw):std::nullopt,
                                              v.script?std::optional(image.scripts()[*v.script].checksum):std::nullopt}});
                }
            }
            std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.second.offset<b.second.offset;});
            for(const auto& [name,e]:ordered){
                ++events;auto old=expected.find(name);
                if(e.token_script && old!=expected.end() && old->second.checksum && old->second.checksum==e.checksum){++reused;continue;}
                expected[name]=e;
            }
        }
        check(table.definitions().size()==expected.size(),"Definition count differs from source-order model");
        std::size_t aliases=0,resolved=0,missing=0,cycles=0,limits=0;
        for(const auto& [name,e]:expected){
            auto actual=table.find(name);
            check(actual&&actual->module->id==e.module&&actual->offset==e.offset&&actual->alias==e.alias,"Winner/provenance mismatch");
            if(!e.alias)continue;
            ++aliases;
            std::set<std::uint32_t> seen;std::vector<std::uint32_t> path;
            auto n=name;std::size_t hops=0;ResolveStatus status;
            for(;;){
                path.push_back(n);
                if(!seen.insert(n).second){status=ResolveStatus::Cycle;break;}
                auto it=expected.find(n);
                if(it==expected.end()){status=ResolveStatus::Missing;break;}
                if(!it->second.alias){status=ResolveStatus::Resolved;break;}
                if(hops==256){status=ResolveStatus::Limit;break;}
                ++hops;n=*it->second.alias;
            }
            auto result=table.resolve(name);
            check(result.status==status && result.path==path,"Alias trace/status mismatch");
            switch(status){case ResolveStatus::Resolved:++resolved;break;case ResolveStatus::Missing:++missing;break;
                case ResolveStatus::Cycle:++cycles;break;case ResolveStatus::Limit:++limits;break;}
        }
        auto definitions=expected.size();
        if(argc==4){
            std::ofstream out(argv[3]);out<<"{\n  \"nodes\": [\n";bool comma=false;
            for(const auto& d:table.definitions()){
                auto result=table.resolve(d->name);
                if(comma)out<<",\n";comma=true;
                out<<"    {\"name\": "<<d->name<<", \"type\": "<<static_cast<unsigned>(d->type)<<", \"alias\": ";
                if(d->alias)out<<*d->alias;else out<<"null";
                out<<", \"status\": \""<<resolve_status_name(result.status)<<"\", \"target\": ";
                if(result.target)out<<result.target->name;else out<<"null";
                out<<", \"last_alias\": ";if(result.last_alias)out<<result.last_alias->name;else out<<"null";
                out<<", \"steps\": "<<result.path.size()<<"}";
            }
            out<<"\n  ]\n}\n";out.close();check(bool(out),"Cannot write topology");
        }
        for(auto id:{ids.front(),ids.back()}){
            table.unload(id);std::erase_if(expected,[=](const auto& item){return item.second.module==id;});
            check(table.definitions().size()==expected.size(),"Unload count mismatch");
            for(const auto& [name,e]:expected)check(table.find(name)->module->id==e.module,"Unload revealed overwritten symbol");
        }
        std::cout<<"PASS: "<<ids.size()<<" modules ("<<tokens<<" token, "<<images<<" image), "<<definitions
                 <<" active definitions, "<<events-definitions-reused<<" replacements, "<<reused<<" unchanged scripts; aliases: "<<resolved<<" resolved, "<<missing
                 <<" missing, "<<cycles<<" cycles, "<<limits<<" limited\n";
        if(argc>=3){
            std::ofstream out(argv[2]);out<<"{\n  \"modules\": "<<ids.size()<<",\n  \"token_modules\": "<<tokens
             <<",\n  \"image_modules\": "<<images<<",\n  \"definition_events\": "<<events<<",\n  \"active_definitions\": "<<definitions
             <<",\n  \"replacements\": "<<events-definitions-reused<<",\n  \"reused_scripts\": "<<reused<<",\n  \"aliases\": "<<aliases<<",\n  \"resolved\": "<<resolved
             <<",\n  \"missing\": "<<missing<<",\n  \"cycles\": "<<cycles<<",\n  \"limits\": "<<limits<<"\n}\n";
            out.close();check(bool(out),"Cannot write report");
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
