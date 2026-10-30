// NOT ORIGINAL - private QuickJS codec for the rich by-value QB model.
// Phase 2, issue #21. This header is internal to the scripting library and
// may include QuickJS; the public model (js_value.hpp) must not.
#pragma once
#include "thug2/js_value.hpp"
#include <quickjs.h>
#include <cstddef>
#include <cstdint>

namespace thug2::js {
// Hard limits shared by every codec call (issue #21 contract).
struct RichLimits {
    static constexpr std::size_t max_depth = 128;
    static constexpr std::size_t max_nodes = 250000;
    static constexpr std::size_t max_fields = RichValue::max_fields;      // 4096
    static constexpr std::size_t max_elements = RichValue::max_elements;  // 250000
    static constexpr std::size_t max_string_bytes = RichValue::max_string_bytes; // 250000
    static constexpr std::size_t max_payload_bytes = 4 * 1024 * 1024;     // 4 MiB
};

// Converts a rich model into a QuickJS value object shaped like the values.js
// representation ({type,bits} / {type,bytes} / {type,coordinates} /
// {type,members} / {type,elementType,elements}).
//
// Ownership and error policy (both directions):
// - Validation/limit failures happen before or without any QuickJS state
//   change and surface as std::runtime_error; the QuickJS context is left
//   with no pending exception.
// - If a QuickJS allocation or property access fails and leaves a pending
//   exception, encode returns JS_EXCEPTION (the pending exception stays for
//   the caller) and decode converts the pending exception into
//   std::runtime_error after clearing it, so the context stays reusable.
// - Successfully encoded JSValues are owned by the caller (single reference,
//   free with JS_FreeValue); on any error path nothing leaks.
// Reference tags 20/21 are not representable in the model and are rejected
// explicitly at any depth.
class RichCodec {
public:
    explicit RichCodec(JSContext* context);
    ~RichCodec();
    RichCodec(const RichCodec&) = delete;
    RichCodec& operator=(const RichCodec&) = delete;

    // Encode a model value; shape/limit validation happens before any
    // QuickJS allocation.
    JSValue encode(const RichValue& value); // caller frees via JS_FreeValue
    // Encode an ordered parameter list as [[name,value],...].
    JSValue encode_parameters(const RichParameters& parameters);
    // Decode a QuickJS value object into an owning model. Validates real
    // types/lengths/tags, rejects getter/proxy exceptions (a pending QuickJS
    // exception is never ignored) and rejects cycles. Tags 20/21 and any
    // unsupported descriptor fail explicitly.
    RichValue decode(JSValueConst value);
    // Decode [[name,value],...] into an ordered model parameter list.
    RichParameters decode_parameters(JSValueConst array);
private:
    struct Impl;
    Impl& impl_;
};
} // namespace thug2::js
