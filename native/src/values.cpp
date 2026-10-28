// Static value parsing and expression conventions from script/parse.cpp,
// eval.cpp and struct.cpp (2d82ed2). No runtime game structs or callbacks.
#include "thug2/values.hpp"
#include "extended.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace thug2::qb {
namespace {
[[noreturn]] void fail(EvalFailure kind, const std::string& why) { throw EvalError(kind, why); }
ValuePtr frozen(Value v) { return std::make_shared<const Value>(std::move(v)); }
ValuePtr scalar(ValueType type, std::uint32_t bits) { Value v;v.type=type;v.bits=bits;return frozen(std::move(v)); }
bool number(ValuePtr v) { return v->type==ValueType::Integer || v->type==ValueType::Float; }
std::int32_t integer(ValuePtr v) { return std::bit_cast<std::int32_t>(v->bits); }
double numeric(ValuePtr v) { return v->type==ValueType::Integer ? double(integer(v)) : double(std::bit_cast<float>(v->bits)); }
std::uint32_t boolean(ValuePtr v) {
    if(v->type==ValueType::Integer)return v->bits;
    return v->type==ValueType::Float && std::bit_cast<float>(v->bits)!=0.0f ? 1:0;
}
bool equal(ValuePtr a,ValuePtr b) {
    if(number(a)&&number(b))return numeric(a)==numeric(b);
    if(a->type!=b->type)return false;
    switch(a->type){
    case ValueType::Name:return a->bits==b->bits;
    case ValueType::String:case ValueType::LocalString:return a->text==b->text;
    case ValueType::Pair:case ValueType::Vector:
        for(unsigned i=0;i<(a->type==ValueType::Pair?2u:3u);++i)
            if(std::bit_cast<float>(a->coordinates[i])!=std::bit_cast<float>(b->coordinates[i]))return false;
        return true;
    case ValueType::Array:case ValueType::Structure:case ValueType::Script:
        fail(EvalFailure::Unsupported,"Runtime identity comparison is not modeled by immutable values");
    default:return false;
    }
}
void add_member(std::vector<Member>& out,Member m) {
    if(!m.value)fail(EvalFailure::Type,"Null structure member");
    std::erase_if(out,[&](const Member& old){
        if(old.name!=m.name)return false;
        if(old.value->type==m.value->type)
            return !(m.name==0 && m.value->type==ValueType::Name) || old.value->bits==m.value->bits;
        return number(old.value)&&number(m.value);
    });
    out.push_back(std::move(m));
}
std::uint32_t word(std::span<const std::uint8_t> b,std::size_t p) {
    if(p>b.size() || b.size()-p<4)fail(EvalFailure::Syntax,"Truncated value word");
    return std::uint32_t(b[p])|std::uint32_t(b[p+1])<<8|std::uint32_t(b[p+2])<<16|std::uint32_t(b[p+3])<<24;
}
} // namespace
std::string_view eval_failure_name(EvalFailure k) {
    switch(k){case EvalFailure::Unsupported:return "unsupported";case EvalFailure::Missing:return "missing";
    case EvalFailure::Cycle:return "cycle";case EvalFailure::Limit:return "limit";case EvalFailure::Type:return "type";
    case EvalFailure::Arithmetic:return "arithmetic";case EvalFailure::Syntax:return "syntax";}return "unknown";
}
ValuePtr integer_value(std::int32_t n){return scalar(ValueType::Integer,std::bit_cast<std::uint32_t>(n));}
ValuePtr float_value(float n){return scalar(ValueType::Float,std::bit_cast<std::uint32_t>(n));}
ValuePtr name_value(std::uint32_t n){return scalar(ValueType::Name,n);}
ValuePtr string_value(std::string text,bool local){
    Value v;v.type=local?ValueType::LocalString:ValueType::String;
    v.text=text.substr(0,text.find('\0'));return frozen(std::move(v));
}
ValuePtr array_value(std::vector<ValuePtr> elements){
    Value v;v.type=ValueType::Array;
    for(const auto& e:elements){
        if(!e)fail(EvalFailure::Type,"Null array element");
        if(v.element_type==ValueType::None)v.element_type=e->type;
        else if(v.element_type!=e->type){
            if((v.element_type==ValueType::Integer&&e->type==ValueType::Float)||
               (v.element_type==ValueType::Float&&e->type==ValueType::Integer))v.element_type=ValueType::Float;
            else fail(EvalFailure::Type,"Heterogeneous array has no safe packed representation");
        }
    }
    if(v.element_type==ValueType::Float)
        for(auto& e:elements)if(e->type==ValueType::Integer)e=float_value(static_cast<float>(integer(e)));
    v.elements=std::move(elements);return frozen(std::move(v));
}
ValuePtr structure_value(std::vector<Member> members){
    Value v;v.type=ValueType::Structure;for(auto& m:members)add_member(v.members,std::move(m));return frozen(std::move(v));
}
int operator_precedence(std::uint8_t op){
    switch(op){case 5:case 8:case 13:return 100;case 12:return 99;case 10:case 11:return 98;
    case 18:return 80;case 20:return 78;case 7:return 76;case 51:return 60;case 50:return 58;default:return -1;}
}
ValuePtr binary_value(std::uint8_t op,ValuePtr a,ValuePtr b){
    if(!a||!b)fail(EvalFailure::Type,"Null value operand");
    if(op==50||op==51)return scalar(ValueType::Integer,op==50?boolean(a)|boolean(b):boolean(a)&boolean(b));
    if(op==7)return integer_value(equal(a,b));
    if(op==18||op==20){
        if(number(a)&&number(b))return integer_value(op==18?numeric(a)<numeric(b):numeric(a)>numeric(b));
        if((a->type==ValueType::String||a->type==ValueType::LocalString)&&a->type==b->type)
            return integer_value(op==18?a->text.size()<b->text.size():a->text.size()>b->text.size());
        if(a->type==ValueType::Structure&&b->type==ValueType::Structure){
            auto small=op==18?a:b,large=op==18?b:a;
            for(const auto& m:small->members)
                if(std::none_of(large->members.begin(),large->members.end(),[&](const Member& n){return m.name==n.name&&equal(m.value,n.value);}))
                    return integer_value(0);
            return integer_value(1);
        }
        return integer_value(0);
    }
    if(op==5){
        if(a->type!=ValueType::Array||b->type!=ValueType::Integer)fail(EvalFailure::Type,"[] requires array and integer");
        if(b->bits>=a->elements.size())fail(EvalFailure::Arithmetic,"Array index out of range");
        if(a->element_type==ValueType::LocalString)fail(EvalFailure::Unsupported,"Original [] does not support local-string arrays");
        return a->elements[b->bits];
    }
    if(op==11&&a->type==ValueType::Name&&(b->type==ValueType::Integer||b->type==ValueType::Name))return name_value(a->bits+b->bits);
    if(number(a)&&number(b)){
        if(op==12){
            if((b->type==ValueType::Integer&&b->bits==0)||
               (b->type==ValueType::Float&&std::fabs(numeric(b))<double(1e-6f)))fail(EvalFailure::Arithmetic,"Division by zero or near-zero float");
        }
        if(a->type==ValueType::Integer&&b->type==ValueType::Integer){
            if(op==11)return scalar(ValueType::Integer,a->bits+b->bits);
            if(op==10)return scalar(ValueType::Integer,a->bits-b->bits);
            if(op==13)return scalar(ValueType::Integer,a->bits*b->bits);
            if(op==12){
                if(integer(a)==std::numeric_limits<std::int32_t>::min()&&integer(b)==-1)fail(EvalFailure::Arithmetic,"Integer division overflow");
                return integer_value(integer(a)/integer(b));
            }
        }
        double x=numeric(a),y=numeric(b);
        if(op==11||op==10||op==13||op==12)return float_value(detail::arithmetic(op,x,y));
    }
    if(op==11&&(a->type==ValueType::String||a->type==ValueType::LocalString)&&a->type==b->type){
        if(a->text.size()+b->text.size()>=200)fail(EvalFailure::Arithmetic,"String addition exceeds 199 bytes");
        return string_value(a->text+b->text,a->type==ValueType::LocalString);
    }
    if(a->type==ValueType::Array&&b->type==ValueType::Array&&op==11){
        if(a->elements.empty())return b;if(b->elements.empty())return a;
        if(a->element_type!=b->element_type)fail(EvalFailure::Type,"Array addition with different element types is unsafe");
        auto e=a->elements;e.insert(e.end(),b->elements.begin(),b->elements.end());return array_value(std::move(e));
    }
    if(a->type==ValueType::Structure){
        if(op==11&&b->type==ValueType::Structure){auto m=a->members;m.insert(m.end(),b->members.begin(),b->members.end());return structure_value(std::move(m));}
        if(op==10){
            auto m=a->members;
            auto remove=[&](std::uint32_t name,std::optional<ValueType> type={}){
                std::erase_if(m,[&](const Member& e){return (e.name==name&&(!type||e.value->type==*type))||
                    (!type&&e.name==0&&e.value->type==ValueType::Name&&e.value->bits==name);});
            };
            if(b->type==ValueType::Name)remove(b->bits);
            else if(b->type==ValueType::Array&&(b->element_type==ValueType::Name||b->element_type==ValueType::None))
                for(const auto& e:b->elements)remove(e->bits);
            else if(b->type==ValueType::Structure){for(const auto& e:b->members){if(e.name)remove(e.name,e.value->type);else if(e.value->type==ValueType::Name)remove(e.value->bits);}}
            else fail(EvalFailure::Type,"Invalid structure subtraction");
            return structure_value(std::move(m));
        }
    }
    auto coordinate=[](ValuePtr v,unsigned i){return double(std::bit_cast<float>(v->coordinates[i]));};
    bool vector=a->type==ValueType::Vector||a->type==ValueType::Pair;
    if(vector&&a->type==b->type&&(op==11||op==10||op==8||(op==13&&a->type==ValueType::Vector))){
        unsigned n=a->type==ValueType::Vector?3:2;
        if(op==8){auto sum=detail::multiply(coordinate(a,n-1),coordinate(b,n-1));
            for(unsigned i=n-1;i>0;--i)sum=detail::add(sum,detail::multiply(coordinate(a,i-1),coordinate(b,i-1)));
            return float_value(detail::single(sum));}
        Value v;v.type=a->type;
        for(unsigned i=0;i<n;++i){
            float z;
            if(op==11||op==10)z=detail::arithmetic(op,coordinate(a,i),coordinate(b,i));
            else{
                auto x=detail::multiply(coordinate(b,(i+2)%3),coordinate(a,(i+1)%3));
                auto y=detail::multiply(coordinate(b,(i+1)%3),coordinate(a,(i+2)%3));
                z=detail::single(detail::add(x,{-y.high,-y.low}));
            }
            v.coordinates[i]=std::bit_cast<std::uint32_t>(z);
        }
        return frozen(std::move(v));
    }
    if((vector&&number(b)&&(op==13||op==12)) ||
       (number(a)&&(b->type==ValueType::Vector||b->type==ValueType::Pair)&&op==13)){
        auto vec=vector?a:b,num=vector?b:a;
        if(op==12&&(numeric(num)==0 || (num->type==ValueType::Float&&std::fabs(numeric(num))<double(1e-6f))))fail(EvalFailure::Arithmetic,"Vector division by zero");
        Value v;v.type=vec->type;
        for(unsigned i=0;i<(v.type==ValueType::Vector?3u:2u);++i){
            double x=coordinate(vec,i),y=numeric(num);
            v.coordinates[i]=std::bit_cast<std::uint32_t>(detail::arithmetic(op==12?12:13,x,y));
        }
        return frozen(std::move(v));
    }
    fail(EvalFailure::Type,"Unsupported operands for QB operator "+std::to_string(op));
}

struct ValueEvaluator::Frame {
    ValueEvaluator& e;
    explicit Frame(ValueEvaluator& e):e(e){e.step();if(e.depth_>=e.limits_.depth)fail(EvalFailure::Limit,"Value recursion limit");++e.depth_;}
    ~Frame(){--e.depth_;}
};
ValueEvaluator::ValueEvaluator(const SymbolTable& table,EvalLimits limits):limits_(limits){
    for(auto d:table.definitions())definitions_[d->name]=std::move(d);
}
void ValueEvaluator::step(){if(++steps_>limits_.steps)fail(EvalFailure::Limit,"Value step limit");}
ValuePtr ValueEvaluator::track(ValuePtr v){if(++nodes_>limits_.nodes)fail(EvalFailure::Limit,"Value allocation limit");return v;}
ValuePtr ValueEvaluator::global(std::uint32_t name){
    if(!depth_){nodes_=0;steps_=0;}return get(name);
}
ValuePtr ValueEvaluator::image(const PrecompiledImage& source,std::size_t index){
    Frame frame(*this);const auto& input=source.values().at(index);
    Value v;v.type=input.type;v.bits=input.raw;v.text=input.text;v.element_type=input.element_type;
    for(std::size_t i=0;i<input.coordinates.size();++i)v.coordinates[i]=input.coordinates[i];
    if(input.script)v.code=source.scripts()[*input.script].code;
    for(auto child:input.children){
        auto e=image(source,child);
        if(v.type==ValueType::Array)v.elements.push_back(std::move(e));
        else v.members.push_back({source.values()[child].name,std::move(e)});
    }
    return track(frozen(std::move(v)));
}
ValuePtr ValueEvaluator::resolve_name(ValuePtr v){
    if(v->type!=ValueType::Name||!definitions_.count(v->bits))return v;
    auto value=get(v->bits);
    if(value->type==ValueType::Script||value->type==ValueType::CFunction||value->type==ValueType::MemberFunction)
        fail(EvalFailure::Unsupported,"Expression requires a script/engine call");
    return value;
}
ValuePtr ValueEvaluator::find_member(ValuePtr v,std::uint32_t name){
    Frame frame(*this);if(!v||v->type!=ValueType::Structure)fail(EvalFailure::Type,"Member lookup requires a structure");
    ValuePtr found;
    for(const auto& m:v->members){
        step();if(m.name==name)found=m.value;
        else if(m.name==0&&m.value->type==ValueType::Name&&definitions_.count(m.value->bits)){
            auto included=get(m.value->bits); // Non-structure flags, including script names, are not inclusions.
            if(included->type==ValueType::Structure){auto match=find_member(included,name);if(match)found=std::move(match);}
        }
    }
    return found;
}
ValuePtr ValueEvaluator::member(ValuePtr v,std::uint32_t name){
    if(!depth_){nodes_=0;steps_=0;}return find_member(std::move(v),name);
}
ValuePtr ValueEvaluator::apply(std::uint8_t op,ValuePtr a,ValuePtr b){
    step();
    if(op==11&&a->type==b->type&&(a->type==ValueType::Array||a->type==ValueType::Structure)){
        auto left=a->type==ValueType::Array?a->elements.size():a->members.size();
        auto right=b->type==ValueType::Array?b->elements.size():b->members.size();
        if(left>limits_.nodes||right>limits_.nodes-left)fail(EvalFailure::Limit,"Container expansion exceeds value budget");
    }
    if(op==7||op==18||op==20){a=resolve_name(std::move(a));b=resolve_name(std::move(b));}
    if((op==18||op==20)&&a->type==ValueType::Structure&&b->type==ValueType::Structure){
        auto small=op==18?a:b,large=op==18?b:a;
        for(const auto& m:small->members){
            bool found=false;
            for(const auto& n:large->members){
                step();if(m.name==n.name&&apply(7,m.value,n.value)->bits){found=true;break;}
            }
            if(!found)return track(integer_value(0));
        }
        return track(integer_value(1));
    }
    if(op==8&&a->type==ValueType::Structure){
        if(b->type!=ValueType::Name)fail(EvalFailure::Type,"Structure member selector must be a name");
        auto v=find_member(a,b->bits);return v?v:track(frozen(Value{}));
    }
    return track(binary_value(op,std::move(a),std::move(b)));
}

class ValueEvaluator::Parser {
public:
    Parser(ValueEvaluator& e,const Document& doc,std::size_t begin,std::size_t end,ValuePtr params={}):e(e),doc(doc),b(doc.bytes()),p(begin),end(end),params(std::move(params)){
        if(begin>end||end>b.size())fail(EvalFailure::Syntax,"Invalid value source range");
    }
    ValuePtr complete(){auto v=value();skip();if(p!=end)error(EvalFailure::Unsupported,"Unconsumed value tokens");return v;}
    ParsedValue one(){auto v=value();return {std::move(v),p,e.steps_};}
    ParsedValue parameters(){
        std::vector<Member> members;
        while(p<end){
            skip();if(p==end)break;auto initial=peek();std::uint32_t named=0;bool assigned=false;
            if(initial==0x16){auto saved=p;auto n=word(b,p+1);p+=token().size;skip();
                if(p<end&&b[p]==7){named=n;assigned=true;++p;}else p=saved;}
            skip();auto source=peek();auto v=value();
            if(v->type==ValueType::None)continue;
            bool spread=!assigned&&(source==3||source==0x0e||source==0x2c||source==0x2d)&&v->type==ValueType::Structure;
            if(spread){for(const auto& m:v->members)add_member(members,m);}else add_member(members,{named,std::move(v)});
        }
        return {e.track(structure_value(std::move(members))),p,e.steps_};
    }
private:
    [[noreturn]] void error(EvalFailure k,const std::string& message){std::ostringstream s;s<<"QB value offset 0x"<<std::hex<<p<<": "<<message;fail(k,s.str());}
    std::uint8_t peek(){if(p>=end)error(EvalFailure::Syntax,"Unexpected end of value");return b[p];}
    Token token(){e.step();auto t=read_token(b,p);if(t.size>end-p)error(EvalFailure::Syntax,"Value exceeds its source range");return t;}
    void skip(){while(p<end&&(b[p]==1||b[p]==2||b[p]==9)){p+=token().size;}}
    ValuePtr value(bool expression_name=false,bool member_name=false){
        Frame frame(e);skip();auto t=token();auto start=p;p+=t.size;
        switch(t.opcode){
        case 0x16:{auto v=e.track(name_value(word(b,start+1)));return expression_name&&!member_name?e.resolve_name(v):v;}
        case 0x17:case 0x18:return e.track(scalar(ValueType::Integer,word(b,start+1)));
        case 0x1a:return e.track(scalar(ValueType::Float,word(b,start+1)));
        case 0x1b:case 0x1c:return e.track(string_value(std::string(b.begin()+start+5,b.begin()+start+t.size-1),t.opcode==0x1c));
        case 0x1e:case 0x1f:{Value v;v.type=t.opcode==0x1e?ValueType::Vector:ValueType::Pair;
            for(std::size_t i=0;i<(t.size-1)/4;++i)v.coordinates[i]=word(b,start+1+i*4);return e.track(frozen(std::move(v)));}
        case 3:{std::vector<Member> members;
            for(;;){skip();if(peek()==4){++p;break;}
                if(peek()==3){auto sub=value();for(const auto& m:sub->members)add_member(members,m);continue;}
                std::uint32_t name=0;bool assigned=false;
                if(peek()==0x16){auto saved=p;auto n=word(b,p+1);p+=token().size;skip();
                    if(p<end&&b[p]==7){++p;name=n;assigned=true;}else p=saved;}
                skip();auto source=peek();bool expr=source==0x0e;
                if(peek()==0x23){auto i=std::find_if(doc.scripts().begin(),doc.scripts().end(),[&](const Script& s){return s.offset==p;});
                    if(i==doc.scripts().end())error(EvalFailure::Syntax,"Missing embedded script metadata");name=i->name;}
                auto v=value();
                if(v->type==ValueType::None)continue;
                if(!assigned&&(expr||source==0x2c||source==0x2d)&&v->type==ValueType::Structure){for(const auto& m:v->members)add_member(members,m);}
                else add_member(members,{name,std::move(v)});
            }
            return e.track(structure_value(std::move(members)));}
        case 5:{std::vector<ValuePtr> elements;
            if(expression_name)error(EvalFailure::Unsupported,"Array literal in expression context");
            for(;;){skip();auto t=peek();if(t==6){++p;break;}
                if(t!=3&&t!=5&&t!=0x16&&t!=0x17&&t!=0x18&&t!=0x1a&&t!=0x1b&&t!=0x1c&&t!=0x1e&&t!=0x1f)
                    error(EvalFailure::Unsupported,"Runtime-dependent array element");
                elements.push_back(value());}
            return e.track(array_value(std::move(elements)));}
        case 0x0e:{auto v=expression(0);skip_expression();if(peek()!=0x0f)error(EvalFailure::Syntax,"Missing expression close");++p;return v;}
        case 0x23:{auto i=std::find_if(doc.scripts().begin(),doc.scripts().end(),[&](const Script& s){return s.offset==start;});
            if(i==doc.scripts().end()||i->end_offset>end)error(EvalFailure::Syntax,"Invalid script value");
            Value v;v.type=ValueType::Script;v.code.assign(b.begin()+i->body_offset,b.begin()+i->end_offset);p=i->end_offset;return e.track(frozen(std::move(v)));}
        case 0x2c:
            if(params)return params;
            if(expression_name)error(EvalFailure::Missing,"Caller parameters are absent");
            return e.track(frozen(Value{}));
        case 0x2d:{
            if(p>=end||b[p]!=0x16||end-p<5)error(EvalFailure::Syntax,"Parameter reference lacks name token");
            auto n=word(b,p+1);p+=5;auto v=params?e.find_member(params,n):ValuePtr{};
            if(!v){if(expression_name)error(EvalFailure::Missing,"Caller parameter is absent");return e.track(frozen(Value{}));}
            return expression_name?e.resolve_name(v):v;
        }
        default:p=start;error(EvalFailure::Unsupported,"Runtime-dependent or unsupported value token "+std::to_string(t.opcode));
        }
    }
    void skip_expression(){while(p<end&&(b[p]==1||b[p]==2||b[p]==6)){p+=token().size;}}
    ValuePtr expression(int minimum,bool member_name=false){
        Frame frame(e);skip_expression();auto left=value(true,member_name);
        for(;;){skip_expression();if(p>=end||b[p]==0x0f)break;
            auto op=b[p];bool implicit=false;
            if((op==0x17||op==0x1a)&&p+5<=end){
                auto bits=word(b,p+1);implicit=op==0x17?std::bit_cast<std::int32_t>(bits)<0:std::bit_cast<float>(bits)<0;
                if(implicit)op=10;
            }
            int precedence=operator_precedence(op);
            if(precedence<0)error(EvalFailure::Unsupported,"Unsupported expression operator");
            if(precedence<minimum)break;
            ValuePtr right;
            if(implicit){auto t=token();auto bits=word(b,p+1);p+=t.size;
                right=e.track(t.opcode==0x17?scalar(ValueType::Integer,0u-bits):float_value(-std::bit_cast<float>(bits)));
                right=tail(std::move(right),precedence+1);
            }else{++p;right=expression(precedence+1,op==8);}
            left=e.apply(op,std::move(left),std::move(right));
        }
        return left;
    }
    ValuePtr tail(ValuePtr left,int minimum){
        // An implicit negative literal has already supplied the right operand.
        for(;;){skip_expression();if(p>=end||b[p]==0x0f)break;
            auto op=b[p];auto precedence=operator_precedence(op);if(precedence<minimum)break;
            ++p;auto right=expression(precedence+1,op==8);left=e.apply(op,std::move(left),std::move(right));}
        return left;
    }
    ValueEvaluator& e;const Document& doc;std::span<const std::uint8_t> b;std::size_t p,end;ValuePtr params;
};
ParsedValue ValueEvaluator::read(const Document& doc,std::size_t begin,std::size_t end,ValuePtr params){
    if(!depth_){nodes_=0;steps_=0;}return Parser(*this,doc,begin,end,std::move(params)).one();
}
ParsedValue ValueEvaluator::arguments(const Document& doc,std::size_t begin,std::size_t end,ValuePtr params){
    if(!depth_){nodes_=0;steps_=0;}return Parser(*this,doc,begin,end,std::move(params)).parameters();
}
ValuePtr ValueEvaluator::resolved(ValuePtr value){
    if(!depth_){nodes_=0;steps_=0;}return resolve_name(std::move(value));
}
ValuePtr ValueEvaluator::get(std::uint32_t name){
    Frame frame(*this);
    if(auto cached=cache_.find(name);cached!=cache_.end())return cached->second;
    auto found=definitions_.find(name);if(found==definitions_.end())fail(EvalFailure::Missing,"Global symbol is absent");
    if(!active_.insert(name).second)fail(EvalFailure::Cycle,"Cyclic value dependency");
    struct Leave{std::unordered_set<std::uint32_t>& s;std::uint32_t n;~Leave(){s.erase(n);}} leave{active_,name};
    const auto& d=*found->second;ValuePtr result;
    if(d.alias){result=definitions_.count(*d.alias)?get(*d.alias):track(name_value(*d.alias));}
    else if(d.storage==DefinitionStorage::ImageValue){const auto& source=std::get<PrecompiledImage>(d.module->data);result=image(source,source.symbols()[d.index].value);}
    else if(d.storage==DefinitionStorage::TokenScript){const auto& source=std::get<Document>(d.module->data);const auto& s=source.scripts()[d.index];
        Value v;v.type=ValueType::Script;v.code.assign(source.bytes().begin()+s.body_offset,source.bytes().begin()+s.end_offset);result=track(frozen(std::move(v)));}
    else{const auto& source=std::get<Document>(d.module->data);const auto& s=source.symbols()[d.index];result=Parser(*this,source,s.value_offset,s.end_offset).complete();}
    cache_[name]=result;return result;
}

std::string format_value(ValuePtr root,std::size_t max_items){
    if(!root)fail(EvalFailure::Type,"Cannot format a missing value");
    std::ostringstream out;std::size_t remaining=max_items;
    auto print=[&](auto&& self,ValuePtr v,unsigned depth)->void{
        if(!remaining||depth>64){out<<"...";return;}--remaining;
        switch(v->type){
        case ValueType::Integer:out<<integer(v);break;
        case ValueType::Float:out<<std::setprecision(9)<<std::bit_cast<float>(v->bits);break;
        case ValueType::String:case ValueType::LocalString:
            if(v->type==ValueType::LocalString)out<<"local";out<<'"';
            for(unsigned char c:v->text){if(c=='"'||c=='\\')out<<'\\'<<char(c);else if(c>=32&&c<127)out<<char(c);
                else out<<"\\x"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;}out<<'"';break;
        case ValueType::Name:out<<"name(0x"<<std::hex<<v->bits<<std::dec<<')';break;
        case ValueType::Pair:case ValueType::Vector:out<<value_type_name(v->type)<<'(';
            for(unsigned i=0;i<(v->type==ValueType::Pair?2u:3u);++i){if(i)out<<", ";out<<std::setprecision(9)<<std::bit_cast<float>(v->coordinates[i]);}out<<')';break;
        case ValueType::Array:out<<'[';for(std::size_t i=0;i<v->elements.size();++i){if(i)out<<", ";self(self,v->elements[i],depth+1);if(!remaining){if(i+1<v->elements.size())out<<", ...";break;}}out<<']';break;
        case ValueType::Structure:out<<'{';for(std::size_t i=0;i<v->members.size();++i){if(i)out<<", ";out<<"0x"<<std::hex<<v->members[i].name<<std::dec<<" = ";self(self,v->members[i].value,depth+1);if(!remaining){if(i+1<v->members.size())out<<", ...";break;}}out<<'}';break;
        case ValueType::Script:out<<"script("<<v->code.size()<<" bytes)";break;
        default:out<<value_type_name(v->type)<<"(0x"<<std::hex<<v->bits<<std::dec<<')';
        }
    };print(print,std::move(root),0);return out.str();
}
} // namespace thug2::qb
