#include "thug2/runtime.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <functional>
#include <unordered_set>

int main(int argc,char** argv){
 try{
    using namespace thug2;using namespace thug2::qb;
    if(argc!=2&&argc!=3)throw std::runtime_error("Usage: profile-corpus DATA_ROOT [REPORT.json]");
    auto root=std::filesystem::canonical(argv[1]);AssetStore assets;std::map<std::string,std::string> catalog;
    auto add=[&](std::string name){auto key=name;for(char& c:key){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';}
        if(key.ends_with(".qb")||key.ends_with(".qb.xbx"))catalog.emplace(key,name);};
    std::vector<std::filesystem::path> pres;
    for(const auto& e:std::filesystem::directory_iterator(root/"pre"))if(e.path().extension()==".prx")pres.push_back(e.path());
    std::sort(pres.begin(),pres.end());
    for(const auto& p:pres){assets.mount_archive(p);auto archive=Archive::load(p);for(const auto& e:archive.entries())add(e.name);}
    assets.mount_directory(root);
    for(const auto& e:std::filesystem::recursive_directory_iterator(root))if(!e.is_symlink()&&e.is_regular_file())add(e.path().lexically_relative(root).generic_string());
    std::vector<std::string> files;for(const auto& [key,name]:catalog){(void)key;files.push_back(name);}
    SymbolTable table;table.load_batch(assets,files);Interpreter interpreter(table);
    interpreter.install_data_hosts();RunLimits limits;limits.fuel=100000;limits.trace=0;ValueEvaluator values(table);
    auto profiles=values.global(name_crc("master_skater_list"));
    if(profiles->type!=ValueType::Array||profiles->elements.empty())throw std::runtime_error("Nonempty master_skater_list required");
    std::unordered_set<std::uint32_t> names;std::size_t lookups=0,visible=0;
    std::function<bool(ValuePtr,std::size_t)> hidden=[&](ValuePtr v,std::size_t depth){
        if(depth>128||v->type!=ValueType::Structure)throw std::runtime_error("Invalid reference profile structure");
        for(const auto& m:v->members){
            if(m.name==name_crc("not_in_frontend")||(m.name==0&&m.value->type==ValueType::Name&&m.value->bits==name_crc("not_in_frontend")))return true;
            if(m.name==0&&m.value->type==ValueType::Name&&table.find(m.value->bits)){
                auto included=values.global(m.value->bits);if(included->type==ValueType::Structure&&hidden(included,depth+1))return true;
            }
        }
        return false;
    };
    for(std::size_t i=0;i<profiles->elements.size();++i){
        auto profile=profiles->elements[i];auto name=values.member(profile,name_crc("name"));
        if(!name||name->type!=ValueType::Name||!names.insert(name->bits).second)throw std::runtime_error("Unique profile names required");
        if(!hidden(profile,0))++visible;
        auto result=interpreter.run(name_crc("get_skater_index_from_name"),structure_value({{name_crc("name"),name}}),limits);
        if(result.status!=RunStatus::Finished)throw std::runtime_error("Profile lookup: "+result.message);
        auto index=values.member(result.parameters,name_crc("skater_index"));
        if(!index||index->type!=ValueType::Integer||index->bits!=i)throw std::runtime_error("Original QB profile lookup disagrees with reference scan");
        ++lookups;
    }
    auto result=interpreter.run(name_crc("get_num_visible_profiles"),{},limits);
    if(result.status!=RunStatus::Finished)throw std::runtime_error("Visible profiles: "+result.message);
    auto count=values.member(result.parameters,name_crc("num_visible_profiles"));
    if(!count||count->type!=ValueType::Integer||count->bits!=visible)throw std::runtime_error("Original QB profile count disagrees with reference scan");
    std::cout<<"Original QB profile scenario: "<<lookups<<" name lookups, "<<visible<<" visible / "<<profiles->elements.size()<<" total profiles\n";
    if(argc==3){std::ofstream out(argv[2]);out<<"{\n  \"modules\": "<<files.size()<<",\n  \"profiles\": "<<profiles->elements.size()<<",\n  \"lookups\": "<<lookups<<",\n  \"visible_profiles\": "<<visible<<",\n  \"registered_hosts\": [\"ArrayContains\", \"GetArraySize\", \"StructureContains\", \"GotParam\", \"Get2DArrayData\", \"Get3DArrayData\", \"GetNDArrayData\"],\n  \"fuel_per_call\": "<<limits.fuel<<",\n  \"scope\": \"original QB functions on inspection snapshot; no rendering or engine objects\"\n}\n";out.close();if(!out)throw std::runtime_error("Cannot write report");}
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
