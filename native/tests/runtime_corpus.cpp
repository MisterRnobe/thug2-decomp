#include "thug2/runtime.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

int main(int argc,char** argv){
 try{
    using namespace thug2;using namespace thug2::qb;
    if(argc<2||argc>4)throw std::runtime_error("Usage: runtime-corpus DATA_ROOT [REPORT.json [--wait|--data|--data-wait]]");
    std::string_view mode=argc==4?argv[3]:"";
    if(argc==4&&mode!="--wait"&&mode!="--data"&&mode!="--data-wait")throw std::runtime_error("Unknown audit mode");
    bool with_wait=mode=="--wait"||mode=="--data-wait",with_data=mode=="--data"||mode=="--data-wait";
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
    RunLimits limits;limits.fuel=500;limits.trace=4;if(with_data)interpreter.install_data_hosts();if(with_wait)interpreter.install_wait();
    std::uint64_t frame=0;
    std::size_t scripts=0,finished=0,limit=0,waiting=0;std::map<std::string,std::size_t> faults;
    std::vector<std::uint32_t> completed;
    for(const auto& d:table.definitions())if(d->type==ValueType::Script){
        ++scripts;RunResult r{};
        if(with_wait){
            auto task=interpreter.start(d->name,{},limits);
            for(unsigned i=0;i<4;++i){
                r=interpreter.update(task,{frame,1.0/60,static_cast<std::uint32_t>(frame*1000/60)});++frame;
                if(r.status!=RunStatus::Waiting)break;
            }
            interpreter.cancel(task);
        }else r=interpreter.run(d->name,{},limits);
        if(r.status==RunStatus::Finished){++finished;completed.push_back(d->name);}
        else if(r.status==RunStatus::Limit)++limit;
        else if(r.status==RunStatus::Waiting)++waiting;
        else ++faults[r.failure?std::string(eval_failure_name(*r.failure)):"exception"];
    }
    std::cout<<(with_data?(with_wait?"Data+Wait execution audit: ":"Data-only execution audit: "):(with_wait?"Wait-only execution audit: ":"No-host execution audit: "))<<scripts<<" entries, "<<finished<<" finished, "<<limit<<" limited, "<<waiting<<" waiting\n";
    for(const auto& [kind,n]:faults)std::cout<<kind<<": "<<n<<'\n';
    if(argc>=3){std::ofstream out(argv[2]);out<<"{\n  \"modules\": "<<files.size()<<",\n  \"entries\": "<<scripts
        <<",\n  \"fuel_per_entry\": "<<limits.fuel<<",\n  \"registered_hosts\": "<<((with_wait?1:0)+(with_data?3:0))<<",\n  \"max_frames_per_entry\": "<<(with_wait?4:1)<<",\n  \"waiting\": "<<waiting<<",\n  \"finished\": "<<finished
        <<",\n  \"limited\": "<<limit<<",\n  \"faults\": {";
        bool comma=false;for(const auto& [kind,n]:faults){if(comma)out<<',';comma=true;out<<'"'<<kind<<"\": "<<n;}
        out<<"},\n  \"completed_entries\": [";comma=false;for(auto name:completed){if(comma)out<<',';comma=true;out<<name;}
        out<<"]\n}\n";out.close();if(!out)throw std::runtime_error("Cannot write report");}
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
