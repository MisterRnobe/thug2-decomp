#include "thug2/runtime.hpp"
#include <charconv>
#include <iostream>

namespace {
std::uint32_t checksum(std::string_view name){
    if(!name.starts_with("0x"))return thug2::name_crc(name);
    std::uint32_t n;auto [end,error]=std::from_chars(name.data()+2,name.data()+name.size(),n,16);
    if(error!=std::errc{}||end!=name.data()+name.size())throw std::invalid_argument("Invalid CRC");return n;
}
std::size_t count(std::string_view text){
    std::size_t n;auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),n);
    if(error!=std::errc{}||end!=text.data()+text.size())throw std::invalid_argument("Invalid count");return n;
}
}
int main(int argc,char** argv){
 try{
    if(argc<6||(argc-2)%2)throw std::invalid_argument("Usage: thug2-run SCRIPT --load QB... [--prx ARCHIVE|--dir ROOT] [--int PARAM=VALUE] [--fuel N] [--trace N] [--frames N] [--fps N] [--seed N]");
    thug2::AssetStore assets;std::vector<std::string> files;std::vector<thug2::qb::Member> params;thug2::qb::RunLimits limits;std::size_t frames=0,fps=60;std::uint32_t seed=0;
    for(int i=2;i<argc;i+=2){std::string option=argv[i];std::string_view argument=argv[i+1];
        if(option=="--load")files.emplace_back(argument);
        else if(option=="--prx")assets.mount_archive(argv[i+1]);
        else if(option=="--dir")assets.mount_directory(argv[i+1]);
        else if(option=="--fuel")limits.fuel=count(argument);
        else if(option=="--trace")limits.trace=count(argument);
        else if(option=="--frames"){frames=count(argument);if(!frames||frames>100000)throw std::invalid_argument("Frames must be 1..100000");}
        else if(option=="--fps"){fps=count(argument);if(!fps||fps>1000)throw std::invalid_argument("FPS must be 1..1000");}
        else if(option=="--seed"){auto n=count(argument);if(n>0xffffffffu)throw std::invalid_argument("Seed must fit uint32");seed=static_cast<std::uint32_t>(n);}
        else if(option=="--int"){
            auto split=argument.find('=');if(split==std::string_view::npos||!split)throw std::invalid_argument("Expected PARAM=VALUE");
            auto value=argument.substr(split+1);std::int32_t n;
            auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
            if(error!=std::errc{}||end!=value.data()+value.size())throw std::invalid_argument("Invalid int32 parameter");
            params.push_back({checksum(argument.substr(0,split)),thug2::qb::integer_value(n)});
        }else throw std::invalid_argument("Unknown option: "+option);
    }
    if(files.empty())throw std::invalid_argument("At least one --load is required");
    thug2::qb::SymbolTable symbols;symbols.load_batch(assets,files);thug2::qb::Interpreter interpreter(symbols);
    interpreter.seed_random(seed);
    auto parameters=thug2::qb::structure_value(std::move(params));
    thug2::qb::RunResult result{};
    if(frames){
        interpreter.install_wait();auto task=interpreter.start(checksum(argv[1]),parameters,limits);
        for(std::size_t i=0;i<frames;++i){
            result=interpreter.update(task,{i,1.0/double(fps),static_cast<std::uint32_t>(i*1000/fps)});
            if(result.status!=thug2::qb::RunStatus::Waiting)break;
        }
    }else result=interpreter.run(checksum(argv[1]),parameters,limits);
    std::cout<<"status\t"<<thug2::qb::run_status_name(result.status)<<"\nwork\t"<<result.work
             <<"\nparameters\t"<<thug2::qb::format_value(result.parameters)<<'\n';
    if(result.failure)std::cout<<"failure\t"<<thug2::qb::eval_failure_name(*result.failure)<<'\n';
    if(!result.message.empty())std::cout<<"message\t"<<result.message<<'\n';
    for(const auto& entry:result.trace)std::cout<<"trace\t"<<symbols.label(entry.script)<<"+0x"<<std::hex<<entry.offset
        <<std::dec<<"\tdepth="<<entry.depth<<'\t'<<thug2::qb::token_name(entry.token)<<'\n';
    if(result.trace_truncated)std::cout<<"trace_truncated\ttrue\n";
    return result.status==thug2::qb::RunStatus::Finished?0:result.status==thug2::qb::RunStatus::Limit?2:result.status==thug2::qb::RunStatus::Waiting?3:1;
 }catch(const std::exception& e){std::cerr<<"thug2-run: "<<e.what()<<'\n';return 1;}
}
