#include "thug2/runtime.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <functional>
#include <unordered_set>

int main(int argc,char** argv){
 try{
    using namespace thug2;using namespace thug2::qb;
    if(argc!=2&&argc!=3)throw std::runtime_error("Usage: menu-callback-corpus DATA_ROOT [REPORT.json]");
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
    auto profiles=values.global(name_crc("master_skater_list"));if(profiles->type!=ValueType::Array||profiles->elements.empty())throw std::runtime_error("Nonempty profile array required");
    auto entry=name_crc("generic_menu_pad_choose"),callback=name_crc("callback"),output=name_crc("num_visible_profiles");
    auto absent=interpreter.run(entry,{},limits);
    if(absent.status!=RunStatus::Finished||values.member(absent.parameters,output)||values.member(absent.parameters,name_crc("skater_index")))throw std::runtime_error("Absent callback must finish without callback output");
    auto direct=interpreter.run(name_crc("get_num_visible_profiles"),{},limits);auto expected=values.member(direct.parameters,output);
    if(direct.status!=RunStatus::Finished||!expected)throw std::runtime_error("Reference profile count must finish");
    auto count=interpreter.run(entry,structure_value({{callback,name_value(name_crc("get_num_visible_profiles"))}}),limits);auto actual=values.member(count.parameters,output);
    if(count.status!=RunStatus::Finished||!actual||actual->bits!=expected->bits)throw std::runtime_error("Menu callback profile count differs from direct QB call");
    std::size_t checks=2,lookups=0;constexpr std::uint32_t marker=0x01234567;
    for(std::size_t i=0;i<profiles->elements.size();++i){
        auto name=values.member(profiles->elements[i],name_crc("name"));if(!name||name->type!=ValueType::Name)throw std::runtime_error("Profile name required");
        auto result=interpreter.run(entry,structure_value({{callback,name_value(name_crc("get_skater_index_from_name"))},{name_crc("name"),name},{marker,integer_value(123)}}),limits);
        auto index=values.member(result.parameters,name_crc("skater_index"));auto kept=values.member(result.parameters,marker);
        if(result.status!=RunStatus::Finished||!index||index->bits!=i||!kept||kept->bits!=123)throw std::runtime_error("Menu callback did not forward/retain parameters or return profile index");
        ++checks;++lookups;
    }
    std::cout<<"Original QB menu callback: "<<checks<<" cases, "<<lookups<<" profile lookups, visible count "<<expected->bits<<'\n';
    if(argc==3){std::ofstream out(argv[2]);out<<"{\n  \"modules\": "<<files.size()<<",\n  \"checks\": "<<checks<<",\n  \"forwarded_profile_lookups\": "<<lookups<<",\n  \"visible_profiles\": "<<expected->bits<<",\n  \"absent_callback_checked\": true,\n  \"parameter_retention_checked\": true,\n  \"fuel_per_call\": "<<limits.fuel<<",\n  \"registered_hosts\": [\"GetArraySize\", \"StructureContains\", \"GotParam\"],\n  \"scope\": \"unmodified generic_menu_pad_choose and profile QB callbacks; no rendered UI\"\n}\n";out.close();if(!out)throw std::runtime_error("Cannot write report");}
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
