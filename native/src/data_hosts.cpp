// Data-only script commands: array_cfuncs.cpp (GetArraySize) and
// script_cfuncs.cpp (StructureContains), script_util_cfuncs.cpp (GotParam),
// upstream 2d82ed2. Native bounds new.
#include "thug2/runtime.hpp"
#include <bit>
#include <limits>
#include <unordered_set>

namespace thug2::qb {
namespace {
struct Search {
    ValueEvaluator& values;std::size_t work=0;
    std::unordered_set<const Value*> path;
    void charge(std::size_t n=1){
        if(n>250000-work)throw EvalError(EvalFailure::Limit,"Data host search work limit");work+=n;
    }
    struct Guard {
        Search& owner;const Value* value;
        Guard(Search& owner,ValuePtr v):owner(owner),value(v.get()){
            if(!v||v->type!=ValueType::Structure)throw EvalError(EvalFailure::Type,"Data host requires a structure");
            if(owner.path.size()>=128)throw EvalError(EvalFailure::Limit,"Data host search depth limit");
            if(!owner.path.insert(value).second)throw EvalError(EvalFailure::Cycle,"Cyclic included structure");
        }
        ~Guard(){owner.path.erase(value);}
    };
    ValuePtr global(std::uint32_t name){
        ValuePtr v;
        try{v=values.global(name);}catch(const EvalError& e){if(e.kind!=EvalFailure::Missing)throw;}
        charge(values.last_work());return v;
    }
    // CStruct::Search keeps the last correctly typed match. Bare name flags
    // must not erase a prior match of another type. Name queries use the
    // specialized GetChecksum exception below.
    ValuePtr typed(ValuePtr structure,std::uint32_t name,ValueType kind){
        Guard guard(*this,structure);ValuePtr found;
        for(const auto& m:structure->members){
            charge();
            if(m.name==name){
                auto value=m.value;
                if(value->type==kind){
                    // GetChecksum has a special unnamed-name rule absent
                    // from generic Search: global structure inclusions are
                    // not checksum values (and are not recursively expanded).
                    if(kind==ValueType::Name&&name==0){auto symbol=global(value->bits);if(symbol&&symbol->type==ValueType::Structure)continue;}
                    found=value;continue;
                }
                if(value->type==ValueType::Name)value=global(value->bits);
                if(value&&value->type==kind)found=std::move(value);
            }else if(m.name==0&&m.value->type==ValueType::Name){
                auto included=global(m.value->bits);
                if(included&&included->type==ValueType::Structure){auto inner=typed(included,name,kind);if(inner)found=std::move(inner);}
            }
        }
        return found;
    }
    bool contains(ValuePtr structure,std::uint32_t name){
        Guard guard(*this,structure);
        for(const auto& m:structure->members){
            charge();if(m.name==name)return true;
            if(m.name==0&&m.value->type==ValueType::Name){
                if(m.value->bits==name)return true;
                auto included=global(m.value->bits);
                if(included&&included->type==ValueType::Structure&&contains(included,name))return true;
            }
        }
        return false;
    }
};
}
void Interpreter::install_data_hosts(){
    bind(name_crc("GotParam"),[this](const HostCall& call){
        Search search{values_,0,{}};auto name=search.typed(call.arguments,0,ValueType::Name);
        bool found=search.contains(call.parameters,name?name->bits:0);
        return HostResult{found,{},{},search.work};
    });
    bind(name_crc("GetArraySize"),[this](const HostCall& call){
        Search search{values_,0,{}};auto array=search.typed(call.arguments,0,ValueType::Array);
        if(!array)throw EvalError(EvalFailure::Type,"GetArraySize requires an array");
        for(auto name:{"Index1","index2","index3"}){
            auto index=search.typed(call.arguments,name_crc(name),ValueType::Integer);if(!index)break;
            if(array->element_type!=ValueType::Array)throw EvalError(EvalFailure::Type,"GetArraySize index requires nested arrays");
            if(index->bits>=array->elements.size())throw EvalError(EvalFailure::Arithmetic,"GetArraySize index out of bounds");
            array=array->elements[index->bits];search.charge();
            if(!array||array->type!=ValueType::Array)throw EvalError(EvalFailure::Type,"GetArraySize index did not select an array");
        }
        if(array->elements.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw EvalError(EvalFailure::Limit,"Array size exceeds int32");
        return HostResult{true,structure_value({{name_crc("array_size"),integer_value(static_cast<std::int32_t>(array->elements.size()))}}),{},search.work};
    });
    bind(name_crc("StructureContains"),[this](const HostCall& call){
        Search search{values_,0,{}};auto structure=search.typed(call.arguments,name_crc("Structure"),ValueType::Structure);
        if(!structure)return HostResult{false,{},{},search.work};
        auto name=search.typed(call.arguments,0,ValueType::Name);
        bool found=search.contains(structure,name?name->bits:0);
        return HostResult{found,{},{},search.work};
    });
}
} // namespace thug2::qb
