// NOT ORIGINAL - QuickJS tests for the rich by-value QB codec (issue #21).
// Independent expected shapes/words: encode results are inspected directly
// against the values.js representation instead of relying on round-trip only.
#include "thug2/js_value.hpp"
#include "../src/value_codec.hpp"
#include <quickjs.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)
#define CHECK_THROWS(expr, needle) do { \
    bool threw = false; \
    try { (void)(expr); } catch (const std::runtime_error& e) { \
        threw = true; \
        if (std::strstr(e.what(), needle) == nullptr) { \
            std::printf("FAIL %s:%d: message '%s' lacks '%s'\n", __FILE__, __LINE__, e.what(), needle); \
            ++failures; \
        } \
    } \
    if (!threw) { std::printf("FAIL %s:%d: no throw: %s\n", __FILE__, __LINE__, #expr); ++failures; } \
} while (0)
// Same as CHECK_THROWS, but evals `code` first and frees the resulting JSValue
// whether the call throws or not.
#define CHECK_THROWS_DECODE(codec, code, needle) do { \
    JSValue owned_ = eval(rt.ctx, (code)); \
    bool threw_ = false; \
    try { (void)(codec).decode(owned_); } catch (const std::runtime_error& e) { \
        threw_ = true; \
        if (std::strstr(e.what(), (needle)) == nullptr) { \
            std::printf("FAIL %s:%d: message '%s' lacks '%s'\n", __FILE__, __LINE__, e.what(), needle); \
            ++failures; \
        } \
    } \
    if (!threw_) { std::printf("FAIL %s:%d: no throw: %s\n", __FILE__, __LINE__, (code)); ++failures; } \
    JS_FreeValue(rt.ctx, owned_); \
} while (0)
#define CHECK_THROWS_PARAMS(codec, code, needle) do { \
    JSValue owned_ = eval(rt.ctx, (code)); \
    bool threw_ = false; \
    try { (void)(codec).decode_parameters(owned_); } catch (const std::runtime_error& e) { \
        threw_ = true; \
        if (std::strstr(e.what(), (needle)) == nullptr) { \
            std::printf("FAIL %s:%d: message '%s' lacks '%s'\n", __FILE__, __LINE__, e.what(), needle); \
            ++failures; \
        } \
    } \
    if (!threw_) { std::printf("FAIL %s:%d: no throw: %s\n", __FILE__, __LINE__, (code)); ++failures; } \
    JS_FreeValue(rt.ctx, owned_); \
} while (0)

using thug2::js::RichCodec;
using thug2::js::RichField;
using thug2::js::RichParameters;
using thug2::js::RichValue;

// QuickJS context fixture.
struct Runtime {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    Runtime() {
        rt = JS_NewRuntime();
        ctx = JS_NewContext(rt);
    }
    ~Runtime() {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
};

// Reads a uint32 property of a JS object.
std::uint32_t num(JSContext* ctx, JSValueConst obj, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(v)) throw std::runtime_error("num read");
    std::uint32_t out = 0;
    if (JS_ToUint32(ctx, &out, v) < 0) {
        JS_FreeValue(ctx, v);
        throw std::runtime_error("num convert");
    }
    JS_FreeValue(ctx, v);
    return out;
}
// Reads element i of a JS array.
JSValue at(JSContext* ctx, JSValueConst arr, std::uint32_t i) {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    if (JS_IsException(v)) throw std::runtime_error("at read");
    return v;
}
std::uint32_t len(JSContext* ctx, JSValueConst arr) {
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(v)) throw std::runtime_error("len read");
    std::uint32_t out = 0;
    if (JS_ToUint32(ctx, &out, v) < 0) {
        JS_FreeValue(ctx, v);
        throw std::runtime_error("len convert");
    }
    JS_FreeValue(ctx, v);
    return out;
}
// Evaluates a JS expression and returns the resulting value.
JSValue eval(JSContext* ctx, const char* code) {
    JSValue v = JS_Eval(ctx, code, std::strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        std::string text = "eval failed";
        JSValue exc = JS_GetException(ctx);
        if (!JS_IsNull(exc) && !JS_IsUndefined(exc)) {
            const char* msg = JS_ToCString(ctx, exc);
            if (msg) {
                text += ": ";
                text += msg;
                JS_FreeCString(ctx, msg);
            }
        }
        JS_FreeValue(ctx, exc);
        throw std::runtime_error(text);
    }
    return v;
}

// --- encode shape checks against the values.js representation ------------
void test_encode_scalar_shapes() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // Integer: raw bits preserved, no float64 round-trip.
    {
        JSValue v = codec.encode(RichValue::integer(0xDEADBEEFu));
        CHECK(JS_IsObject(v));
        CHECK(num(rt.ctx, v, "type") == 1);
        CHECK(num(rt.ctx, v, "bits") == 0xDEADBEEFu);
        JS_FreeValue(rt.ctx, v);
    }
    // Float: raw -0 and NaN payload bits preserved.
    {
        std::uint32_t neg_zero = 0x80000000u;
        std::uint32_t nan_bits = 0x7F800042u;
        JSValue v = codec.encode(RichValue::floating(neg_zero));
        CHECK(num(rt.ctx, v, "type") == 2);
        CHECK(num(rt.ctx, v, "bits") == neg_zero);
        JS_FreeValue(rt.ctx, v);
        v = codec.encode(RichValue::floating(nan_bits));
        CHECK(num(rt.ctx, v, "bits") == nan_bits);
        JS_FreeValue(rt.ctx, v);
    }
    // Name: distinct from Integer even with equal bits.
    {
        JSValue v = codec.encode(RichValue::name(0x12345678u));
        CHECK(num(rt.ctx, v, "type") == 13);
        CHECK(num(rt.ctx, v, "bits") == 0x12345678u);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_encode_string_shapes() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // String with bytes >= 128, canonical NUL-prefix model without a NUL.
    {
        RichValue::Bytes bytes = {0x00, 0x41, 0x80, 0xFF, 0xC3};
        // canonical payload never contains an embedded NUL; use >=128 bytes
        RichValue::Bytes canonical = {0x41, 0x80, 0xFF, 0xC3};
        JSValue v = codec.encode(RichValue::string(canonical));
        CHECK(num(rt.ctx, v, "type") == 3);
        JSValue arr = JS_GetPropertyStr(rt.ctx, v, "bytes");
        CHECK(JS_IsArray(rt.ctx, arr));
        CHECK(len(rt.ctx, arr) == 4);
        JSValue b0 = at(rt.ctx, arr, 0);
        std::uint32_t x = 0;
        JS_ToUint32(rt.ctx, &x, b0);
        CHECK(x == 0x41);
        JS_FreeValue(rt.ctx, b0);
        JSValue b2 = at(rt.ctx, arr, 2);
        JS_ToUint32(rt.ctx, &x, b2);
        CHECK(x == 0xFF);
        JS_FreeValue(rt.ctx, b2);
        JS_FreeValue(rt.ctx, arr);
        JS_FreeValue(rt.ctx, v);
    }
    // LocalString keeps its own tag.
    {
        JSValue v = codec.encode(RichValue::string({'h', 'i'}, true));
        CHECK(num(rt.ctx, v, "type") == 4);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_encode_coord_shapes() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    {
        JSValue v = codec.encode(RichValue::pair(0xBF800000u, 0x3F800000u)); // -1.0f, 1.0f
        CHECK(num(rt.ctx, v, "type") == 5);
        JSValue arr = JS_GetPropertyStr(rt.ctx, v, "coordinates");
        CHECK(len(rt.ctx, arr) == 2);
        JSValue w0 = at(rt.ctx, arr, 0);
        std::uint32_t x = 0;
        JS_ToUint32(rt.ctx, &x, w0);
        CHECK(x == 0xBF800000u);
        JS_FreeValue(rt.ctx, w0);
        JS_FreeValue(rt.ctx, arr);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = codec.encode(RichValue::vector(1, 2, 3));
        CHECK(num(rt.ctx, v, "type") == 6);
        JSValue arr = JS_GetPropertyStr(rt.ctx, v, "coordinates");
        CHECK(len(rt.ctx, arr) == 3);
        JS_FreeValue(rt.ctx, arr);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_encode_structure_shapes() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // Ordered duplicate fields preserved as-is, no semantic merge.
    {
        std::vector<std::uint32_t> names = {0xAA, 0xBB, 0xAA};
        std::vector<RichValue> values = {
            RichValue::integer(1), RichValue::integer(2), RichValue::integer(3)};
        JSValue v = codec.encode(RichValue::structure(names, values));
        CHECK(num(rt.ctx, v, "type") == 10);
        JSValue members = JS_GetPropertyStr(rt.ctx, v, "members");
        CHECK(len(rt.ctx, members) == 3);
        JSValue f2 = at(rt.ctx, members, 2);
        JSValue n2 = at(rt.ctx, f2, 0);
        std::uint32_t x = 0;
        JS_ToUint32(rt.ctx, &x, n2);
        CHECK(x == 0xAA);
        JS_FreeValue(rt.ctx, n2);
        JSValue v2 = at(rt.ctx, f2, 1);
        CHECK(num(rt.ctx, v2, "bits") == 3);
        JS_FreeValue(rt.ctx, v2);
        JS_FreeValue(rt.ctx, f2);
        JS_FreeValue(rt.ctx, members);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_encode_array_shapes() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // Empty array: elementType 0.
    {
        JSValue v = codec.encode(RichValue::array(0, {}));
        CHECK(num(rt.ctx, v, "type") == 12);
        CHECK(num(rt.ctx, v, "elementType") == 0);
        JSValue elements = JS_GetPropertyStr(rt.ctx, v, "elements");
        CHECK(len(rt.ctx, elements) == 0);
        JS_FreeValue(rt.ctx, elements);
        JS_FreeValue(rt.ctx, v);
    }
    // Nested arrays/structures.
    {
        RichValue inner = RichValue::array(1, {RichValue::integer(7), RichValue::integer(8)});
        RichValue outer = RichValue::array(12, {inner, RichValue::array(0, {})});
        JSValue v = codec.encode(outer);
        CHECK(num(rt.ctx, v, "type") == 12);
        CHECK(num(rt.ctx, v, "elementType") == 12);
        JSValue elements = JS_GetPropertyStr(rt.ctx, v, "elements");
        CHECK(len(rt.ctx, elements) == 2);
        JSValue first = at(rt.ctx, elements, 0);
        CHECK(num(rt.ctx, first, "elementType") == 1);
        JSValue inner_elements = JS_GetPropertyStr(rt.ctx, first, "elements");
        CHECK(len(rt.ctx, inner_elements) == 2);
        JSValue e1 = at(rt.ctx, inner_elements, 1);
        CHECK(num(rt.ctx, e1, "bits") == 8);
        JS_FreeValue(rt.ctx, e1);
        JS_FreeValue(rt.ctx, inner_elements);
        JS_FreeValue(rt.ctx, first);
        JS_FreeValue(rt.ctx, elements);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_encode_parameters_shape() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    RichParameters params = {
        RichField{0x1111, RichValue::integer(42)},
        RichField{0x2222, RichValue::string({'a', 'b'})},
    };
    JSValue v = codec.encode_parameters(params);
    CHECK(JS_IsArray(rt.ctx, v));
    CHECK(len(rt.ctx, v) == 2);
    JSValue f0 = at(rt.ctx, v, 0);
    JSValue n0 = at(rt.ctx, f0, 0);
    std::uint32_t x = 0;
    JS_ToUint32(rt.ctx, &x, n0);
    CHECK(x == 0x1111);
    JS_FreeValue(rt.ctx, n0);
    JSValue val0 = at(rt.ctx, f0, 1);
    CHECK(num(rt.ctx, val0, "bits") == 42);
    JS_FreeValue(rt.ctx, val0);
    JS_FreeValue(rt.ctx, f0);
    JS_FreeValue(rt.ctx, v);
}

// --- decode: JS -> model ---------------------------------------------------
void test_decode_scalars() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    {
        JSValue v = eval(rt.ctx, "({type: 1, bits: 4294967295})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 1);
        CHECK(out.bits() == 4294967295u);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx, "({type: 2, bits: 2147483648})"); // -0.0f bits
        RichValue out = codec.decode(v);
        CHECK(out.type() == 2);
        CHECK(out.bits() == 0x80000000u);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx, "({type: 13, bits: 7})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 13);
        CHECK(out.bits() == 7);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_decode_strings() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    {
        JSValue v = eval(rt.ctx, "({type: 3, bytes: [65, 128, 255, 0x7F]})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 3);
        const RichValue::Bytes& bytes = out.bytes();
        CHECK(bytes.size() == 4);
        CHECK(bytes[0] == 65 && bytes[1] == 128 && bytes[2] == 255 && bytes[3] == 0x7F);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx, "({type: 4, bytes: []})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 4);
        CHECK(out.bytes().empty());
        JS_FreeValue(rt.ctx, v);
    }
}

void test_decode_coords() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    {
        JSValue v = eval(rt.ctx, "({type: 5, coordinates: [3212836864, 1065353216]})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 5);
        CHECK(out.coordinates()[0] == 0xBF800000u);
        CHECK(out.coordinates()[1] == 0x3F800000u);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx, "({type: 6, coordinates: [1, 2, 3]})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 6);
        CHECK(out.coordinates().size() == 3);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_decode_structures_and_arrays() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    {
        JSValue v = eval(rt.ctx,
            "({type: 10, members: [[2718, {type: 1, bits: 5}], [2718, {type: 2, bits: 6}]]})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 10);
        CHECK(out.member_names().size() == 2);
        CHECK(out.member_names()[0] == 2718 && out.member_names()[1] == 2718);
        CHECK(out.member_values()[0].bits() == 5);
        CHECK(out.member_values()[1].bits() == 6);
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx, "({type: 12, elementType: 0, elements: []})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 12);
        CHECK(out.element_type() == 0);
        CHECK(out.elements().empty());
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx,
            "({type: 12, elementType: 10, elements: "
            "[{type: 10, members: [[1, {type: 1, bits: 9}]]}]})");
        RichValue out = codec.decode(v);
        CHECK(out.type() == 12);
        CHECK(out.element_type() == 10);
        CHECK(out.elements()[0].member_values()[0].bits() == 9);
        JS_FreeValue(rt.ctx, v);
    }
}

void test_decode_parameters() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    JSValue v = eval(rt.ctx,
        "[[43690, {type: 1, bits: 1}], [43690, {type: 3, bytes: [120]}]]");
    RichParameters out = codec.decode_parameters(v);
    CHECK(out.size() == 2);
    CHECK(out[0].name == 43690 && out[0].value.bits() == 1);
    CHECK(out[1].name == 43690 && out[1].value.bytes().size() == 1);
    JS_FreeValue(rt.ctx, v);
}

// --- round-trip -------------------------------------------------------------
void test_round_trip() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    RichValue original = RichValue::structure(
        {0x0102, 0x0304},
        {RichValue::array(2, {RichValue::floating(0x7FC00000u), RichValue::floating(0x80000000u)}),
         RichValue::string({'Q', 'B', 0xFE}, true)});
    JSValue encoded = codec.encode(original);
    CHECK(!JS_IsException(encoded));
    RichValue back = codec.decode(encoded);
    CHECK(back == original);
    JS_FreeValue(rt.ctx, encoded);

    RichParameters params = {
        RichField{0x0A, RichValue::pair(0x40000000u, 0xC0000000u)},
        RichField{0x0B, RichValue::name(0)},
    };
    JSValue p = codec.encode_parameters(params);
    RichParameters back_p = codec.decode_parameters(p);
    CHECK(back_p.size() == 2);
    CHECK(back_p == params);
    JS_FreeValue(rt.ctx, p);
}

// --- negative: model-side ---------------------------------------------------
void test_model_negatives() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // Embedded NUL rejected explicitly (non-canonical payload).
    CHECK_THROWS(RichValue::string({'a', 0, 'b'}), "NUL");
    // Structure member mismatch.
    CHECK_THROWS(RichValue::structure({1}, {}), "mismatch");
    // Array element type mismatch.
    CHECK_THROWS(RichValue::array(1, {RichValue::floating(0)}), "mismatch");
    // Empty array must have elementType 0.
    CHECK_THROWS(RichValue::array(1, {}), "mismatch");
    // Invalid element type.
    CHECK_THROWS(RichValue::array(99, {RichValue::integer(1)}), "element type");
    // Wrong accessor family.
    {
        RichValue v = RichValue::integer(5);
        CHECK_THROWS(v.bytes(), "accessor");
    }
    // Encode-side depth limit: 129 nested arrays.
    {
        RichValue deep = RichValue::array(0, {});
        bool threw = false;
        try {
            for (int i = 0; i < 200; ++i)
                deep = RichValue::array(12, {deep});
            JSValue out = codec.encode(deep);
            if (JS_IsException(out)) threw = true;
            else JS_FreeValue(rt.ctx, out);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
}

// --- negative: JS-side --------------------------------------------------------
void test_decode_negatives() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // Reference tags 20/21 rejected at top level and nested.
    CHECK_THROWS_DECODE(codec, "({type: 20, bits: 1})", "20/21");
    CHECK_THROWS_DECODE(codec, "({type: 21, bits: 1})", "20/21");
    CHECK_THROWS_DECODE(codec,
        "({type: 10, members: [[1, {type: 21, bits: 1}]]})", "20/21");
    // Unsupported type tag.
    CHECK_THROWS_DECODE(codec, "({type: 7})", "unsupported");
    // Missing type.
    CHECK_THROWS_DECODE(codec, "({})", "uint32 'type'");
    // Non-integer bits.
    CHECK_THROWS_DECODE(codec, "({type: 1, bits: 1.5})", "uint32");
    // Missing bits.
    CHECK_THROWS_DECODE(codec, "({type: 1})", "uint32 'bits'");
    // Non-object input.
    CHECK_THROWS_DECODE(codec, "42", "QB value object");
    // String shape: bytes not an array.
    CHECK_THROWS_DECODE(codec, "({type: 3, bytes: 'abc'})", "string shape");
    // String byte out of range.
    CHECK_THROWS_DECODE(codec, "({type: 3, bytes: [256]})", "string byte");
    // Coordinate count mismatch.
    CHECK_THROWS_DECODE(codec, "({type: 5, coordinates: [1]})", "coordinate count");
    CHECK_THROWS_DECODE(codec, "({type: 6, coordinates: [1,2,3,4]})", "coordinate count");
    // Members not an array.
    CHECK_THROWS_DECODE(codec, "({type: 10, members: 5})", "members array");
    // Field not a [name, value] pair.
    CHECK_THROWS_DECODE(codec, "({type: 10, members: [5]})", "parameter field");
    CHECK_THROWS_DECODE(codec, "({type: 10, members: [[1,2,3]]})", "parameter field");
    // elementType mismatch with elements.
    CHECK_THROWS_DECODE(codec,
        "({type: 12, elementType: 1, elements: [{type: 2, bits: 0}]})", "mismatch");
    // Empty array with nonzero elementType.
    CHECK_THROWS_DECODE(codec, "({type: 12, elementType: 1, elements: []})", "mismatch");
    // Non-empty array with elementType 0.
    CHECK_THROWS_DECODE(codec,
        "({type: 12, elementType: 0, elements: [{type: 1, bits: 0}]})", "mismatch");
    // Invalid elementType tag.
    CHECK_THROWS_DECODE(codec,
        "({type: 12, elementType: 20, elements: []})", "element type");
    // Cyclic JS graph.
    CHECK_THROWS_DECODE(codec,
        "globalThis.cyc = {type: 10, members: []}; cyc.members.push([1, cyc]); cyc", "cyclic");
    // Getter that throws: pending exception must surface, not be ignored.
    CHECK_THROWS_DECODE(codec,
        "({type: 1, get bits() { throw new Error('boom'); }})", "boom");
    // Proxy trap throwing.
    CHECK_THROWS_DECODE(codec,
        "new Proxy({type: 1, bits: 1}, {get(t,k){ if(k==='bits') throw new Error('proxy boom'); return t[k]; }})",
        "proxy boom");
    // decode_parameters: not an array.
    CHECK_THROWS_PARAMS(codec, "({})", "parameters array");
    // decode_parameters: bad field.
    CHECK_THROWS_PARAMS(codec, "[[1]]", "parameter field");
    // decode_parameters: cyclic value (a parameter value contains itself).
    CHECK_THROWS_PARAMS(codec,
        "globalThis.v = {type: 10, members: []}; v.members.push([2, v]); [[1, v]]", "cyclic");
}

// --- error recovery -----------------------------------------------------------
void test_recovery_and_reuse() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // After a decode failure the context has no pending exception and works.
    bool threw = false;
    {
        JSValue v = eval(rt.ctx, "({type: 99})");
        try {
            codec.decode(v);
        } catch (const std::runtime_error&) { threw = true; }
        JS_FreeValue(rt.ctx, v);
    }
    CHECK(threw);
    CHECK(!JS_HasException(rt.ctx));
    JSValue ok = codec.encode(RichValue::integer(3));
    CHECK(!JS_IsException(ok));
    CHECK(num(rt.ctx, ok, "bits") == 3);
    JS_FreeValue(rt.ctx, ok);
    // After a getter-throws failure the pending exception is cleared.
    threw = false;
    {
        JSValue v = eval(rt.ctx, "({type: 1, get bits() { throw new Error('x'); }})");
        try {
            codec.decode(v);
        } catch (const std::runtime_error&) { threw = true; }
        JS_FreeValue(rt.ctx, v);
    }
    CHECK(threw);
    CHECK(!JS_HasException(rt.ctx));
    // Encode failure (model limit) leaves no pending exception.
    threw = false;
    try {
        RichValue::Bytes big(RichValue::max_string_bytes + 1, 'x');
        RichValue::string(big); // factory rejects oversized strings
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(!JS_HasException(rt.ctx));
    // Repeated successful use after failures.
    for (int i = 0; i < 3; ++i) {
        JSValue v = codec.encode(RichValue::name(77));
        CHECK(!JS_IsException(v));
        RichValue back = codec.decode(v);
        CHECK(back.type() == 13 && back.bits() == 77);
        JS_FreeValue(rt.ctx, v);
    }
}

// --- limits --------------------------------------------------------------------
void test_limits() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // String byte limit (decode side).
    CHECK_THROWS_DECODE(codec, "({type: 3, bytes: new Array(250001).fill(65)})", "string byte limit");
    // Field limit (decode side).
    CHECK_THROWS_DECODE(codec,
        "({type: 10, members: new Array(4097).fill(0).map((_, i) => [i, {type: 1, bits: 1}])})",
        "field limit");
    // Node limit: a wide structure of arrays exceeds 250000 nodes.
    {
        bool threw = false;
        try {
            std::vector<RichValue> elements;
            elements.reserve(300000);
            for (int i = 0; i < 300000; ++i)
                elements.push_back(RichValue::integer(i));
            // 300001 nodes > 250000
            RichValue big = RichValue::array(1, std::move(elements));
            JSValue out = codec.encode(big);
            if (JS_IsException(out)) threw = true;
            else JS_FreeValue(rt.ctx, out);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    // Payload limit: many strings together exceed 4 MiB.
    {
        bool threw = false;
        try {
            std::vector<RichValue> elements;
            RichValue::Bytes chunk(60000, 'a'); // 60 KB each, valid alone
            for (int i = 0; i < 80; ++i)
                elements.push_back(RichValue::string(chunk));
            RichValue big = RichValue::array(3, std::move(elements)); // 4.8 MB total
            JSValue out = codec.encode(big);
            if (JS_IsException(out)) threw = true;
            else JS_FreeValue(rt.ctx, out);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    // Depth limit (decode side): 129 nested arrays.
    {
        bool threw = false;
        try {
            std::string code = "var d = {type: 12, elementType: 0, elements: []};";
            for (int i = 0; i < 200; ++i)
                code += "d = {type: 12, elementType: 12, elements: [d]};";
            code += "d";
            JSValue v = eval(rt.ctx, code.c_str());
            try {
                codec.decode(v);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            JS_FreeValue(rt.ctx, v);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
}

// --- review regressions (issue #21 review round 1) ---------------------------
// Every failure must leave a reusable context: no stale pending exception.
#define CHECK_DECODE_FAILS(code, needle) do { \
    JSValue owned_ = eval(rt.ctx, (code)); \
    bool threw_ = false; \
    try { (void)codec.decode(owned_); } catch (const std::runtime_error& e) { \
        threw_ = true; \
        if (std::strstr(e.what(), (needle)) == nullptr) { \
            std::printf("FAIL %s:%d: message '%s' lacks '%s'\n", __FILE__, __LINE__, e.what(), needle); \
            ++failures; \
        } \
    } \
    if (!threw_) { std::printf("FAIL %s:%d: no throw: %s\n", __FILE__, __LINE__, (code)); ++failures; } \
    if (JS_HasException(rt.ctx)) { \
        std::printf("FAIL %s:%d: stale pending exception after: %s\n", __FILE__, __LINE__, (code)); \
        ++failures; \
        JSValue exc_ = JS_GetException(rt.ctx); \
        JS_FreeValue(rt.ctx, exc_); \
    } \
    JS_FreeValue(rt.ctx, owned_); \
} while (0)

void test_review_double_free_and_sentinel() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // P1-1: a consuming property op failing under memory pressure must not
    // double free; the failure must surface (JS_EXCEPTION + pending, or a
    // C++ throw), and the context must stay usable.
    {
        RichValue v = RichValue::array(10, std::vector<RichValue>(100,
            RichValue::structure({1}, {RichValue::string({'A', 'B', 'C'})})));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(rt.rt, &usage);
        // Sweep a range of budgets: allocator thresholds vary by platform,
        // and different budgets fail at different allocation sites.
        int oom_count = 0;
        int ok_count = 0;
        for (int extra = 0; extra <= 65536; extra += 4096) {
            JS_SetMemoryLimit(rt.rt, usage.malloc_size + extra);
            JSValue out = codec.encode(v);
            if (JS_IsException(out)) {
                ++oom_count;
                CHECK(JS_HasException(rt.ctx)); // never a bare sentinel
                JSValue exc = JS_GetException(rt.ctx);
                JS_FreeValue(rt.ctx, exc);
            } else {
                ++ok_count;
                JS_FreeValue(rt.ctx, out);
            }
        }
        // The sweep must actually exercise the failure path somewhere.
        if (oom_count == 0) {
            std::printf("FAIL %s:%d: OOM sweep never failed\n", __FILE__, __LINE__);
            ++failures;
        }
        if (ok_count == 0) {
            std::printf("FAIL %s:%d: OOM sweep never succeeded\n", __FILE__, __LINE__);
            ++failures;
        }
        JS_SetMemoryLimit(rt.rt, static_cast<std::size_t>(-1));
        // Context still usable after the OOM sweep.
        JSValue ok = codec.encode(RichValue::integer(7));
        CHECK(!JS_IsException(ok));
        CHECK(num(rt.ctx, ok, "bits") == 7);
        JS_FreeValue(rt.ctx, ok);
    }
    // P1-2: depth-limit encode must throw a C++ runtime_error (model
    // preflight), not return a bare JS_EXCEPTION.
    {
        RichValue deep = RichValue::array(0, {});
        for (int i = 0; i < 129; ++i) deep = RichValue::array(12, {deep});
        bool threw = false;
        try {
            JSValue out = codec.encode(deep);
            if (JS_IsException(out)) {
                std::printf("FAIL %s:%d: bare sentinel for model limit\n", __FILE__, __LINE__);
                ++failures;
                JSValue exc = JS_GetException(rt.ctx);
                JS_FreeValue(rt.ctx, exc);
            }
            JS_FreeValue(rt.ctx, out);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        CHECK(!JS_HasException(rt.ctx));
    }
    // P1-2: node-limit and payload-limit encode throw before any JS work.
    {
        bool threw = false;
        try {
            std::vector<RichValue> elements;
            elements.reserve(300000);
            for (int i = 0; i < 300000; ++i)
                elements.push_back(RichValue::integer(i));
            RichValue big = RichValue::array(1, std::move(elements));
            JSValue out = codec.encode(big);
            if (JS_IsException(out)) {
                std::printf("FAIL %s:%d: bare sentinel for node limit\n", __FILE__, __LINE__);
                ++failures;
                JSValue exc = JS_GetException(rt.ctx);
                JS_FreeValue(rt.ctx, exc);
            }
            JS_FreeValue(rt.ctx, out);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        CHECK(!JS_HasException(rt.ctx));
    }
}

void test_review_decode_context_poisoning() {
    Runtime rt;
    RichCodec codec(rt.ctx);
    // P1-3: element getter throwing inside bytes; must throw C++ AND clear.
    CHECK_DECODE_FAILS(
        "(()=>{ const b=[65]; Object.defineProperty(b,'0',{"
        "get(){throw Error('element boom')}}); return {type:3,bytes:b}; })()",
        "element boom");
    // P1-3: exception whose own message getter throws; diagnostics must not
    // run user getters, and the context must end clean.
    CHECK_DECODE_FAILS(
        "({type:1,get bits(){ const e=Error('outer');"
        "Object.defineProperty(e,'message',{get(){throw Error('inner')}}); throw e; }})",
        "bits");
    // P1-3: revoked proxy passed as bytes; JS_IsArray returns -1, not 0.
    CHECK_DECODE_FAILS(
        "(()=>{ const p=Proxy.revocable([],{}); p.revoke();"
        "return {type:3,bytes:p.proxy}; })()",
        "array check");
    // decode_parameters variants of the same failure classes.
    {
        JSValue v = eval(rt.ctx,
            "[[1, (()=>{ const b=[65]; Object.defineProperty(b,'0',{"
            "get(){throw Error('param element boom')}}); return {type:3,bytes:b}; })()]]");
        bool threw = false;
        try { (void)codec.decode_parameters(v); }
        catch (const std::runtime_error& e) {
            threw = true;
            if (std::strstr(e.what(), "param element boom") == nullptr) {
                std::printf("FAIL %s:%d: message '%s' lacks 'param element boom'\n",
                            __FILE__, __LINE__, e.what());
                ++failures;
            }
        }
        if (!threw) { std::printf("FAIL %s:%d: no throw\n", __FILE__, __LINE__); ++failures; }
        CHECK(!JS_HasException(rt.ctx));
        JS_FreeValue(rt.ctx, v);
    }
    {
        JSValue v = eval(rt.ctx,
            "[[1, (()=>{ const p=Proxy.revocable([],{}); p.revoke();"
            "return {type:3,bytes:p.proxy}; })()]]");
        bool threw = false;
        try { (void)codec.decode_parameters(v); }
        catch (const std::runtime_error&) { threw = true; }
        if (!threw) { std::printf("FAIL %s:%d: no throw\n", __FILE__, __LINE__); ++failures; }
        CHECK(!JS_HasException(rt.ctx));
        JS_FreeValue(rt.ctx, v);
    }
    // Context still fully usable after all the failures above.
    for (int i = 0; i < 3; ++i) {
        JSValue ok = codec.encode(RichValue::name(77));
        CHECK(!JS_IsException(ok));
        RichValue back = codec.decode(ok);
        CHECK(back.type() == 13 && back.bits() == 77);
        JS_FreeValue(rt.ctx, ok);
    }
}
} // namespace

int main() {
    test_encode_scalar_shapes();
    test_encode_string_shapes();
    test_encode_coord_shapes();
    test_encode_structure_shapes();
    test_encode_array_shapes();
    test_encode_parameters_shape();
    test_decode_scalars();
    test_decode_strings();
    test_decode_coords();
    test_decode_structures_and_arrays();
    test_decode_parameters();
    test_round_trip();
    test_model_negatives();
    test_decode_negatives();
    test_review_double_free_and_sentinel();
    test_review_decode_context_poisoning();
    test_recovery_and_reuse();
    test_limits();
    if (failures == 0) {
        std::printf("value_codec_tests: all checks passed\n");
        return 0;
    }
    std::printf("value_codec_tests: %d failure(s)\n", failures);
    return 1;
}
