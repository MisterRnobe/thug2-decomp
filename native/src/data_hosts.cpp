// Data-only script commands: array_cfuncs.cpp (size and element access) and
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
    ValuePtr named_array(const HostCall& call,std::uint32_t name){
        auto array=typed(call.parameters,name,ValueType::Array);if(!array)array=global(name);
        if(!array||array->type!=ValueType::Array)throw EvalError(EvalFailure::Type,"Array data command requires a named array");
        return array;
    }
    ValuePtr at(ValuePtr array,std::uint32_t index){
        charge();if(!array||array->type!=ValueType::Array)throw EvalError(EvalFailure::Type,"Array data path requires an array");
        if(index>=array->elements.size())throw EvalError(EvalFailure::Arithmetic,"Array data index out of bounds");
        auto value=array->elements[index];if(!value||value->type!=array->element_type)throw EvalError(EvalFailure::Type,"Inconsistent array element type");return value;
    }
    ValuePtr child(ValuePtr array,std::uint32_t index){
        auto value=at(array,index);if(value->type!=ValueType::Array)throw EvalError(EvalFailure::Type,"Array data path requires nested arrays");return value;
    }
    ValuePtr output(ValuePtr array,std::uint32_t index){
        if(!array||array->type!=ValueType::Array)throw EvalError(EvalFailure::Type,"Array data leaf requires an array");
        switch(array->element_type){
        case ValueType::Integer:case ValueType::Float:case ValueType::String:case ValueType::LocalString:
        case ValueType::Pair:case ValueType::Vector:case ValueType::Structure:case ValueType::Array:case ValueType::Name:break;
        default:return {}; // Original AddElement has no output for unsupported types, even an empty array.
        }
        auto value=at(array,index);
        if(value->type==ValueType::LocalString){charge(value->text.size());value=string_value(value->text);}
        return structure_value({{name_crc("val"),std::move(value)}});
    }
    bool string_equal(std::string_view a,std::string_view b){
        // _stricmp in the original default C locale: ASCII case folding,
        // unsigned bytes otherwise, and C-string termination at embedded NUL.
        auto lower=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};
        for(std::size_t i=0;;++i){
            charge();unsigned char x=i<a.size()?static_cast<unsigned char>(a[i]):0;
            unsigned char y=i<b.size()?static_cast<unsigned char>(b[i]):0;
            if(lower(x)!=lower(y))return false;if(x==0)return true;
        }
    }

};
}
void Interpreter::install_data_hosts(){
    bind(name_crc("ArrayContains"),[this](const HostCall& call){
        Search search{values_,0,{}};auto array=search.typed(call.arguments,name_crc("array"),ValueType::Array);
        if(!array)throw EvalError(EvalFailure::Type,"ArrayContains requires array");
        if(array->elements.empty())return HostResult{false,{},{},search.work};
        const auto kind=array->element_type;ValuePtr target;
        if(kind==ValueType::Integer||kind==ValueType::Name)target=search.typed(call.arguments,name_crc("contains"),kind);
        else if(kind==ValueType::String){
            target=search.typed(call.arguments,name_crc("contains"),ValueType::String);
            if(!target)target=search.typed(call.arguments,name_crc("contains"),ValueType::LocalString);
        }else return HostResult{false,{},{},search.work};
        for(std::size_t i=0;i<array->elements.size();++i){
            auto value=search.at(array,static_cast<std::uint32_t>(i));
            const bool equal=kind==ValueType::String?search.string_equal(target?target->text:std::string_view{},value->text):value->bits==(target?target->bits:0);
            if(equal)return HostResult{true,{},{},search.work};
        }
        return HostResult{false,{},{},search.work};
    });
    for(unsigned dimensions:{2u,3u})bind(name_crc(dimensions==2?"Get2DArrayData":"Get3DArrayData"),[this,dimensions](const HostCall& call){
        Search search{values_,0,{}};auto name=search.typed(call.arguments,name_crc("arrayName"),ValueType::Name);
        auto array=search.named_array(call,name?name->bits:0);
        for(unsigned i=1;i<=dimensions;++i){
            auto index=search.typed(call.arguments,name_crc("index"+std::to_string(i)),ValueType::Integer);
            auto bits=index?index->bits:0;
            if(i<dimensions)array=search.child(array,bits);
            else return HostResult{true,search.output(array,bits),{},search.work};
        }
        throw std::logic_error("Invalid array data dimensions");
    });
    bind(name_crc("GetNDArrayData"),[this](const HostCall& call){
        Search search{values_,0,{}};auto name=search.typed(call.arguments,name_crc("arrayName"),ValueType::Name);
        auto array=search.named_array(call,name?name->bits:0);
        auto indices=search.typed(call.parameters,name_crc("indices"),ValueType::Array);
        if(!indices||indices->elements.empty())throw EvalError(EvalFailure::Type,"GetNDArrayData requires nonempty script indices");
        if(indices->element_type!=ValueType::Integer&&indices->element_type!=ValueType::Float&&indices->element_type!=ValueType::Name)
            throw EvalError(EvalFailure::Type,"GetNDArrayData indices require scalar raw words");
        for(std::size_t i=0;i<indices->elements.size();++i){
            search.charge();auto index=indices->elements[i];
            if(!index||index->type!=indices->element_type)throw EvalError(EvalFailure::Type,"Inconsistent index array type");
            if(i+1<indices->elements.size())array=search.child(array,index->bits);
            else return HostResult{true,search.output(array,index->bits),{},search.work};
        }
        throw std::logic_error("Invalid empty indices");
    });
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
