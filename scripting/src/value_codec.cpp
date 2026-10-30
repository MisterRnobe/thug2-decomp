// NOT ORIGINAL - private QuickJS codec for the rich by-value QB model.
// Phase 2, issue #21. See value_codec.hpp for the ownership contract.
#include "value_codec.hpp"
#include <quickjs.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace thug2::js {
namespace {
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("rich codec: " + message); }

bool is_scalar_type(std::uint32_t type) { return type == 1 || type == 2 || type == 13; }
bool is_string_type(std::uint32_t type) { return type == 3 || type == 4; }
bool is_coord_type(std::uint32_t type) { return type == 5 || type == 6; }
bool is_element_type(std::uint32_t type) {
    // 0 is the canonical element type of an empty array (see RichValue::array).
    return type == 0 || type == 1 || type == 2 || type == 3 || type == 4 || type == 5 ||
           type == 6 || type == 10 || type == 12 || type == 13;
}

// Shared per-call budget. Every encode/decode call starts with a fresh
// budget; limits are enforced before any QuickJS allocation on encode.
struct Budget {
    std::size_t depth = 0;
    std::size_t nodes = 0;
    std::size_t payload = 0;
    void node() {
        if (nodes >= RichLimits::max_nodes) fail("node limit");
        ++nodes;
    }
    void enter() {
        if (depth >= RichLimits::max_depth) fail("depth limit");
        ++depth;
    }
    void exit() { --depth; }
    void payload_bytes(std::size_t bytes) {
        if (bytes > RichLimits::max_payload_bytes || payload > RichLimits::max_payload_bytes - bytes)
            fail("payload byte limit");
        payload += bytes;
    }
};

// RAII guard that frees a JSValue on scope exit unless released.
class JSGuard {
public:
    explicit JSGuard(JSContext* ctx) : ctx_(ctx) {}
    JSGuard(const JSGuard&) = delete;
    JSGuard& operator=(const JSGuard&) = delete;
    ~JSGuard() { JS_FreeValue(ctx_, value_); }
    JSValue release() { return std::exchange(value_, JS_UNDEFINED); }
    JSValueConst slot() const { return value_; }
    void hold(JSValue v) { value_ = v; }
private:
    JSContext* ctx_;
    JSValue value_ = JS_UNDEFINED;
};

// Scoped recursion depth guard.
class DepthGuard {
public:
    explicit DepthGuard(Budget& b) : b_(b) { b_.enter(); }
    ~DepthGuard() { b_.exit(); }
private:
    Budget& b_;
};
} // namespace

struct RichCodec::Impl {
    JSContext* context;
    Budget budget;

    explicit Impl(JSContext* ctx) : context(ctx) {}

    // --- helpers -----------------------------------------------------------
    // Reads a property; returns JS_UNDEFINED-equivalent sentinel on missing.
    // Throws (as a QuickJS failure marker) only when the read itself throws.
    bool property(JSValueConst object, const char* key, JSValue& out) {
        JSValue v = JS_GetPropertyStr(context, object, key);
        if (JS_IsException(v)) return false; // pending exception stays set
        out = v;
        return true;
    }
    // Converts a JS number to uint32 without float64 round-trip surprises:
    // only exact integers in [0, 2^32) are accepted. Missing properties
    // (undefined) and fractional values are rejected instead of coercing.
    bool to_uint32(JSValueConst v, std::uint32_t& out) {
        if (!JS_IsNumber(v)) return false;
        double d = 0.0;
        if (JS_ToFloat64(context, &d, v) < 0) return false; // pending exception stays set
        if (!(d >= 0.0 && d <= 4294967295.0) || std::floor(d) != d) return false;
        out = static_cast<std::uint32_t>(d);
        return true;
    }
    std::uint32_t array_length(JSValueConst array) {
        JSValue len = JS_GetPropertyStr(context, array, "length");
        if (JS_IsException(len)) fail("length read threw");
        JSGuard g(context);
        g.hold(len);
        std::uint32_t out = 0;
        if (!to_uint32(len, out)) fail("expected uint32 length");
        return out;
    }
    JSValue element(JSValueConst array, std::uint32_t index) {
        JSValue v = JS_GetPropertyUint32(context, array, index);
        if (JS_IsException(v)) fail("element read threw");
        return v;
    }
    // Clears a pending QuickJS exception and converts it to std::runtime_error.
    [[noreturn]] void rethrow_pending(const char* what) {
        JSValue exc = JS_GetException(context); // clears the pending exception
        JSGuard g(context);
        g.hold(exc);
        std::string message(what);
        if (JS_IsError(context, exc)) {
            JSValue msg = JS_GetPropertyStr(context, exc, "message");
            if (!JS_IsException(msg)) {
                JSGuard gm(context);
                gm.hold(msg);
                const char* utf8 = JS_ToCString(context, msg);
                if (utf8) {
                    message += ": ";
                    message += utf8;
                    JS_FreeCString(context, utf8);
                }
            }
        }
        fail(message);
    }
    void check_no_pending() {
        if (JS_HasException(context)) rethrow_pending("unexpected pending QuickJS exception");
    }

    // --- encode: model -> QuickJS ------------------------------------------
    JSValue encode(const RichValue& value) {
        budget.node();
        const std::uint32_t type = value.type();
        if (is_scalar_type(type)) {
            JSValue out = JS_NewObject(context);
            if (JS_IsException(out)) return out; // caller sees JS_EXCEPTION
            JSGuard g(context);
            g.hold(out);
            JSValue type_value = JS_NewUint32(context, type);
            JSValue bits_value = JS_NewUint32(context, value.bits());
            if (JS_IsException(type_value) || JS_IsException(bits_value) ||
                JS_SetPropertyStr(context, out, "type", type_value) < 0 ||
                JS_SetPropertyStr(context, out, "bits", bits_value) < 0) {
                JS_FreeValue(context, type_value);
                JS_FreeValue(context, bits_value);
                check_no_pending();
                fail("allocation failed");
            }
            return g.release();
        }
        if (is_string_type(type)) {
            const RichValue::Bytes& bytes = value.bytes();
            budget.payload_bytes(bytes.size());
            JSValue array = JS_NewArray(context);
            if (JS_IsException(array)) return array;
            JSGuard g(context);
            g.hold(array);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                JSValue item = JS_NewUint32(context, bytes[i]);
                if (JS_IsException(item) ||
                    JS_DefinePropertyValueUint32(context, array, static_cast<std::uint32_t>(i),
                                                  item, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(context, item);
                    check_no_pending();
                    fail("allocation failed");
                }
            }
            JSValue out = JS_NewObject(context);
            if (JS_IsException(out)) return out;
            JSGuard go(context);
            go.hold(out);
            JSValue type_value = JS_NewUint32(context, type);
            if (JS_IsException(type_value) ||
                JS_SetPropertyStr(context, out, "type", type_value) < 0 ||
                JS_SetPropertyStr(context, out, "bytes", g.release()) < 0) {
                JS_FreeValue(context, type_value);
                check_no_pending();
                fail("allocation failed");
            }
            return go.release();
        }
        if (is_coord_type(type)) {
            const RichValue::Words& words = value.coordinates();
            budget.payload_bytes(words.size() * sizeof(std::uint32_t));
            JSValue array = JS_NewArray(context);
            if (JS_IsException(array)) return array;
            JSGuard g(context);
            g.hold(array);
            for (std::size_t i = 0; i < words.size(); ++i) {
                JSValue item = JS_NewUint32(context, words[i]);
                if (JS_IsException(item) ||
                    JS_DefinePropertyValueUint32(context, array, static_cast<std::uint32_t>(i),
                                                  item, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(context, item);
                    check_no_pending();
                    fail("allocation failed");
                }
            }
            JSValue out = JS_NewObject(context);
            if (JS_IsException(out)) return out;
            JSGuard go(context);
            go.hold(out);
            JSValue type_value = JS_NewUint32(context, type);
            if (JS_IsException(type_value) ||
                JS_SetPropertyStr(context, out, "type", type_value) < 0 ||
                JS_SetPropertyStr(context, out, "coordinates", g.release()) < 0) {
                JS_FreeValue(context, type_value);
                check_no_pending();
                fail("allocation failed");
            }
            return go.release();
        }
        if (type == 10) {
            DepthGuard guard(budget);
            const auto& names = value.member_names();
            const auto& values = value.member_values();
            if (names.size() > RichLimits::max_fields) fail("field limit");
            JSValue members = JS_NewArray(context);
            if (JS_IsException(members)) return members;
            JSGuard g(context);
            g.hold(members);
            for (std::size_t i = 0; i < names.size(); ++i) {
                JSValue field = encode_field(names[i], values[i]);
                if (JS_IsException(field) ||
                    JS_DefinePropertyValueUint32(context, members, static_cast<std::uint32_t>(i),
                                                  field, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(context, field);
                    check_no_pending();
                    fail("allocation failed");
                }
            }
            JSValue out = JS_NewObject(context);
            if (JS_IsException(out)) return out;
            JSGuard go(context);
            go.hold(out);
            JSValue type_value = JS_NewUint32(context, type);
            if (JS_IsException(type_value) ||
                JS_SetPropertyStr(context, out, "type", type_value) < 0 ||
                JS_SetPropertyStr(context, out, "members", g.release()) < 0) {
                JS_FreeValue(context, type_value);
                check_no_pending();
                fail("allocation failed");
            }
            return go.release();
        }
        // type 12
        DepthGuard guard(budget);
        const std::uint32_t element_type = value.element_type();
        const auto& elements = value.elements();
        if (elements.size() > RichLimits::max_elements) fail("element limit");
        JSValue array = JS_NewArray(context);
        if (JS_IsException(array)) return array;
        JSGuard g(context);
        g.hold(array);
        for (std::size_t i = 0; i < elements.size(); ++i) {
            JSValue item = encode(elements[i]);
            if (JS_IsException(item) ||
                JS_DefinePropertyValueUint32(context, array, static_cast<std::uint32_t>(i),
                                              item, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(context, item);
                check_no_pending();
                fail("allocation failed");
            }
        }
        JSValue out = JS_NewObject(context);
        if (JS_IsException(out)) return out;
        JSGuard go(context);
        go.hold(out);
        JSValue type_value = JS_NewUint32(context, type);
        JSValue et_value = JS_NewUint32(context, element_type);
        if (JS_IsException(type_value) || JS_IsException(et_value) ||
            JS_SetPropertyStr(context, out, "type", type_value) < 0 ||
            JS_SetPropertyStr(context, out, "elementType", et_value) < 0 ||
            JS_SetPropertyStr(context, out, "elements", g.release()) < 0) {
            JS_FreeValue(context, type_value);
            JS_FreeValue(context, et_value);
            check_no_pending();
            fail("allocation failed");
        }
        return go.release();
    }
    JSValue encode_field(std::uint32_t name, const RichValue& value) {
        JSValue field = JS_NewArray(context);
        if (JS_IsException(field)) return field;
        JSGuard g(context);
        g.hold(field);
        JSValue name_value = JS_NewUint32(context, name);
        if (JS_IsException(name_value) ||
            JS_DefinePropertyValueUint32(context, field, 0, name_value, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(context, name_value);
            check_no_pending();
            fail("allocation failed");
        }
        JSValue item = encode(value);
        if (JS_IsException(item) ||
            JS_DefinePropertyValueUint32(context, field, 1, item, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(context, item);
            check_no_pending();
            fail("allocation failed");
        }
        return g.release();
    }

    // --- decode: QuickJS -> model -------------------------------------------
    // All decode helpers throw std::runtime_error; a pending QuickJS
    // exception is always converted and cleared via rethrow_pending.
    std::uint32_t type_tag(JSValueConst object) {
        JSValue raw = JS_GetPropertyStr(context, object, "type");
        if (JS_IsException(raw)) rethrow_pending("type read threw");
        JSGuard g(context);
        g.hold(raw);
        std::uint32_t type = 0;
        if (!to_uint32(raw, type)) rethrow_pending("expected uint32 'type'");
        if (type == 20 || type == 21)
            fail("reference tags 20/21 are not supported by the by-value codec");
        if (!is_scalar_type(type) && !is_string_type(type) && !is_coord_type(type) &&
            type != 10 && type != 12)
            fail("unsupported value type " + std::to_string(type));
        return type;
    }
    std::uint32_t uint32_property(JSValueConst object, const char* key) {
        JSValue raw = JS_GetPropertyStr(context, object, key);
        if (JS_IsException(raw)) rethrow_pending((std::string(key) + " read threw").c_str());
        JSGuard g(context);
        g.hold(raw);
        std::uint32_t out = 0;
        if (!to_uint32(raw, out)) {
            std::string message = std::string("expected uint32 '") + key + "'";
            rethrow_pending(message.c_str());
        }
        return out;
    }
    RichValue decode(JSValueConst value, std::unordered_set<const void*>& path) {
        budget.node();
        if (!JS_IsObject(value)) fail("expected a QB value object");
        const void* identity = JS_VALUE_GET_PTR(value);
        if (!path.insert(identity).second) fail("cyclic value traversal");
        struct Exit {
            std::unordered_set<const void*>& path;
            const void* id;
            ~Exit() { path.erase(id); }
        } exit{path, identity};
        const std::uint32_t type = type_tag(value);
        if (is_scalar_type(type)) {
            const std::uint32_t bits = uint32_property(value, "bits");
            return type == 1 ? RichValue::integer(bits)
                 : type == 2 ? RichValue::floating(bits)
                             : RichValue::name(bits);
        }
        if (is_string_type(type)) {
            JSValue raw = JS_GetPropertyStr(context, value, "bytes");
            if (JS_IsException(raw)) rethrow_pending("bytes read threw");
            JSGuard g(context);
            g.hold(raw);
            if (!JS_IsArray(context, g.slot())) fail("invalid string shape");
            const std::uint32_t length = array_length(g.slot());
            if (length > RichLimits::max_string_bytes) fail("string byte limit");
            RichValue::Bytes bytes(length);
            for (std::uint32_t i = 0; i < length; ++i) {
                JSGuard gi(context);
                gi.hold(element(g.slot(), i));
                std::uint32_t byte = 0;
                if (!to_uint32(gi.slot(), byte)) rethrow_pending("invalid string byte");
                if (byte > 255) fail("invalid string byte");
                bytes[i] = static_cast<std::uint8_t>(byte);
            }
            budget.payload_bytes(bytes.size());
            return RichValue::string(std::move(bytes), type == 4);
        }
        if (is_coord_type(type)) {
            JSValue raw = JS_GetPropertyStr(context, value, "coordinates");
            if (JS_IsException(raw)) rethrow_pending("coordinates read threw");
            JSGuard g(context);
            g.hold(raw);
            if (!JS_IsArray(context, g.slot())) fail("invalid coordinate shape");
            const std::uint32_t length = array_length(g.slot());
            if (length != (type == 5 ? 2u : 3u)) fail("invalid coordinate count");
            RichValue::Words words(length);
            for (std::uint32_t i = 0; i < length; ++i) {
                JSGuard gi(context);
                gi.hold(element(g.slot(), i));
                std::uint32_t word = 0;
                if (!to_uint32(gi.slot(), word)) rethrow_pending("invalid coordinate word");
                words[i] = word;
            }
            budget.payload_bytes(words.size() * sizeof(std::uint32_t));
            return type == 5 ? RichValue::pair(words[0], words[1])
                             : RichValue::vector(words[0], words[1], words[2]);
        }
        DepthGuard guard(budget);
        if (type == 10) {
            JSValue raw = JS_GetPropertyStr(context, value, "members");
            if (JS_IsException(raw)) rethrow_pending("members read threw");
            JSGuard g(context);
            g.hold(raw);
            if (!JS_IsArray(context, g.slot())) fail("expected members array");
            const std::uint32_t length = array_length(g.slot());
            if (length > RichLimits::max_fields) fail("field limit");
            std::vector<std::uint32_t> names(length);
            std::vector<RichValue> values;
            values.reserve(length);
            for (std::uint32_t i = 0; i < length; ++i) {
                JSGuard gi(context);
                gi.hold(element(g.slot(), i));
                if (!JS_IsArray(context, gi.slot())) fail("invalid parameter field");
                const std::uint32_t field_length = array_length(gi.slot());
                if (field_length != 2) fail("invalid parameter field");
                JSGuard gn(context);
                gn.hold(element(gi.slot(), 0));
                std::uint32_t name = 0;
                if (!to_uint32(gn.slot(), name)) rethrow_pending("expected uint32 field name");
                JSGuard gv(context);
                gv.hold(element(gi.slot(), 1));
                names[i] = name;
                values.push_back(decode(gv.slot(), path));
            }
            return RichValue::structure(std::move(names), std::move(values));
        }
        // type 12
        const std::uint32_t element_type = uint32_property(value, "elementType");
        if (!is_element_type(element_type)) fail("invalid array element type");
        JSValue raw = JS_GetPropertyStr(context, value, "elements");
        if (JS_IsException(raw)) rethrow_pending("elements read threw");
        JSGuard g(context);
        g.hold(raw);
        if (!JS_IsArray(context, g.slot())) fail("invalid array shape");
        const std::uint32_t length = array_length(g.slot());
        if (length > RichLimits::max_elements) fail("element limit");
        if ((element_type == 0) != (length == 0)) fail("empty array type mismatch");
        std::vector<RichValue> elements;
        elements.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i) {
            JSGuard gi(context);
            gi.hold(element(g.slot(), i));
            RichValue child = decode(gi.slot(), path);
            if (child.type() != element_type) fail("array element type mismatch");
            elements.push_back(std::move(child));
        }
        return RichValue::array(element_type, std::move(elements));
    }
};

RichCodec::RichCodec(JSContext* context) : impl_(*new Impl(context)) {}
RichCodec::~RichCodec() { delete &impl_; }

JSValue RichCodec::encode(const RichValue& value) {
    impl_.budget = Budget{};
    try {
        JSValue out = impl_.encode(value);
        if (JS_IsException(out)) {
            // Allocation failure: keep the pending exception for the caller.
            return out;
        }
        return out;
    } catch (...) {
        // Model/limit failure: no QuickJS state was touched.
        return JS_EXCEPTION;
    }
}

JSValue RichCodec::encode_parameters(const RichParameters& parameters) {
    impl_.budget = Budget{};
    try {
        if (parameters.size() > RichLimits::max_fields) fail("field limit");
        JSValue array = JS_NewArray(impl_.context);
        if (JS_IsException(array)) return array;
        JSGuard g(impl_.context);
        g.hold(array);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            JSValue field = impl_.encode_field(parameters[i].name, parameters[i].value);
            if (JS_IsException(field) ||
                JS_DefinePropertyValueUint32(impl_.context, array, static_cast<std::uint32_t>(i),
                                              field, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(impl_.context, field);
                impl_.check_no_pending();
                fail("allocation failed");
            }
        }
        return g.release();
    } catch (...) {
        return JS_EXCEPTION;
    }
}

RichValue RichCodec::decode(JSValueConst value) {
    impl_.budget = Budget{};
    std::unordered_set<const void*> path;
    RichValue out = impl_.decode(value, path);
    impl_.check_no_pending();
    return out;
}

RichParameters RichCodec::decode_parameters(JSValueConst array) {
    impl_.budget = Budget{};
    std::unordered_set<const void*> path;
    if (!JS_IsArray(impl_.context, array)) fail("expected parameters array");
    const std::uint32_t length = impl_.array_length(array);
    if (length > RichLimits::max_fields) fail("field limit");
    RichParameters out;
    out.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i) {
        JSGuard g(impl_.context);
        g.hold(impl_.element(array, i));
        if (!JS_IsArray(impl_.context, g.slot())) fail("invalid parameter field");
        const std::uint32_t field_length = impl_.array_length(g.slot());
        if (field_length != 2) fail("invalid parameter field");
        JSGuard gn(impl_.context);
        gn.hold(impl_.element(g.slot(), 0));
        std::uint32_t name = 0;
        if (!impl_.to_uint32(gn.slot(), name)) impl_.rethrow_pending("expected uint32 field name");
        JSGuard gv(impl_.context);
        gv.hold(impl_.element(g.slot(), 1));
        out.push_back(RichField{name, impl_.decode(gv.slot(), path)});
    }
    impl_.check_no_pending();
    return out;
}
} // namespace thug2::js
