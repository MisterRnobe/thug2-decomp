#include "thug2/assets.hpp"
#include "thug2/precompiled.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv){
    try{
        if(argc!=2 && argc!=3)throw std::runtime_error("Usage: precompiled-corpus DATA_ROOT [REPORT.json]");
        auto root=std::filesystem::canonical(argv[1]);
        std::vector<std::filesystem::path> paths;
        for(const auto& e:std::filesystem::recursive_directory_iterator(root))
            if(!e.is_symlink() && e.is_regular_file() && e.path().generic_string().ends_with(".qb.xbx")) paths.push_back(e.path());
        std::sort(paths.begin(),paths.end());
        if(paths.empty())throw std::runtime_error("No precompiled images found");
        thug2::AssetStore assets;assets.mount_directory(root);
        std::size_t bytes=0,symbols=0,values=0,refs=0,scripts=0,compressed=0,code=0;
        for(const auto& path:paths){
            auto name=path.lexically_relative(root).generic_string();
            try{
                auto file=assets.open(name);if(!file)throw std::runtime_error("Missing asset");
                thug2::qb::PrecompiledImage image(thug2::Bytes(file->bytes().begin(),file->bytes().end()));
                bytes+=image.bytes().size();symbols+=image.symbols().size();values+=image.values().size();
                refs+=image.references().size();scripts+=image.scripts().size();
                for(const auto& s:image.scripts()){compressed+=s.compressed;code+=s.code.size();}
            }catch(const std::exception& e){throw std::runtime_error(name+": "+e.what());}
        }
        std::cout<<"PASS: "<<paths.size()<<" images, "<<symbols<<" symbols, "<<values<<" values, "<<refs
                 <<" references, "<<scripts<<" scripts ("<<compressed<<" compressed), "<<code<<" decoded script bytes\n";
        if(argc==3){
            std::ofstream out(argv[2]);out<<"{\n  \"files\": "<<paths.size()<<",\n  \"bytes\": "<<bytes<<",\n  \"symbols\": "<<symbols
             <<",\n  \"values\": "<<values<<",\n  \"references\": "<<refs<<",\n  \"scripts\": "<<scripts<<",\n  \"compressed_scripts\": "
             <<compressed<<",\n  \"decoded_script_bytes\": "<<code<<"\n}\n";out.close();if(!out)throw std::runtime_error("Cannot write report");
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
