#include "thug2/values.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

int main(int argc,char** argv){
 try{
    using namespace thug2;using namespace thug2::qb;
    if(argc!=2&&argc!=3)throw std::runtime_error("Usage: values-corpus DATA_ROOT [REPORT.json]");
    auto root=std::filesystem::canonical(argv[1]);AssetStore assets;std::map<std::string,std::string> catalog;
    auto add=[&](std::string n){auto key=n;for(char& c:key){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';}
        if(key.ends_with(".qb")||key.ends_with(".qb.xbx"))catalog.emplace(key,n);};
    std::vector<std::filesystem::path> pres;
    for(const auto& e:std::filesystem::directory_iterator(root/"pre"))if(e.path().extension()==".prx")pres.push_back(e.path());
    std::sort(pres.begin(),pres.end());
    for(const auto& path:pres){assets.mount_archive(path);auto a=Archive::load(path);for(const auto& e:a.entries())add(e.name);}
    assets.mount_directory(root);
    for(const auto& e:std::filesystem::recursive_directory_iterator(root))if(!e.is_symlink()&&e.is_regular_file())add(e.path().lexically_relative(root).generic_string());
    std::vector<std::string> files;for(const auto& [key,n]:catalog){(void)key;files.push_back(n);}
    SymbolTable table;table.load_batch(assets,files);ValueEvaluator evaluator(table);
    std::map<std::string,std::size_t> types,failures;std::size_t success=0;
    std::vector<std::pair<std::uint32_t,std::string>> errors;
    for(const auto& d:table.definitions()){
        try{auto v=evaluator.global(d->name);++success;++types[std::string(value_type_name(v->type))];}
        catch(const EvalError& e){++failures[std::string(eval_failure_name(e.kind))];
            errors.emplace_back(d->name,std::string(eval_failure_name(e.kind))+": "+e.what());}
    }
    std::cout<<"Value catalog: "<<table.modules().size()<<" modules, "<<success<<" materialized, "<<errors.size()<<" diagnosed\n";
    for(const auto& [kind,n]:failures)std::cout<<kind<<": "<<n<<'\n';
    if(argc==3){
        std::ofstream out(argv[2]);out<<"{\n  \"modules\": "<<table.modules().size()<<",\n  \"definitions\": "<<table.definitions().size()
            <<",\n  \"materialized\": "<<success<<",\n  \"diagnosed\": "<<errors.size()<<",\n  \"types\": {";
        bool comma=false;for(const auto& [kind,n]:types){if(comma)out<<',';comma=true;out<<'"'<<kind<<"\": "<<n;}
        out<<"},\n  \"failures\": {";comma=false;for(const auto& [kind,n]:failures){if(comma)out<<',';comma=true;out<<'"'<<kind<<"\": "<<n;}
        out<<"},\n  \"diagnostics\": [";comma=false;
        // Messages contain numeric offsets and controlled text; escape robustly for JSON.
        auto quote=[&](const std::string& s){out<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')out<<'\\'<<char(c);else if(c>=32&&c<127)out<<char(c);else out<<'?';}out<<'"';};
        for(const auto& [crc,msg]:errors){if(comma)out<<',';comma=true;out<<"\n    {\"name\": "<<crc<<", \"message\": ";quote(msg);out<<'}';}
        out<<"\n  ]\n}\n";out.close();if(!out)throw std::runtime_error("Cannot write report");
    }
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
