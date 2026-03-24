// =============================================================
// variant_bridge.cpp  —  PolyLang v6.6
//
// RETAINED FIXES from v6.5: H-5 (depth cap 64).
//
// ZERO-TRUST AUDIT ROUND 2 FIXES:
//
// FIX VLN-09 [CRITICAL]: from_pl_value() / free_pl_value() trust
//   array.len / dict.len from untrusted adapters.
//   BEFORE: A malicious adapter could return a PLValue with
//     type=PL_TYPE_ARRAY, array.data = small-alloc, array.len = 2^31-1.
//     The loop `for(int32_t i=0; i < in.array.len; ++i)` would walk
//     far past the actual allocation → OOB read / heap corruption.
//     Similarly, dict.len negative = wrap-around in size calculations.
//   AFTER:
//     - PL_VALUE_MAX_ARRAY_LEN (65536) hard cap on array.len and dict.len.
//     - Negative lengths rejected and treated as 0.
//     - Both from_pl_value() and free_pl_value() enforce these bounds.
// =============================================================
#include "variant_bridge.hpp"

#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <algorithm>
#include <string>
#include <cstdlib>
#include <cstring>

namespace polylang {

// Hard cap on array/dict elements from untrusted adapters.
static constexpr int32_t PL_VALUE_MAX_ARRAY_LEN = 65536;

// Clamp incoming len to safe range [0, PL_VALUE_MAX_ARRAY_LEN].
static inline int32_t safe_len(int32_t raw, const char* context) {
    if (raw < 0) {
        ERR_PRINT(("[PolyLang/VariantBridge] Negative array/dict len from adapter (" +
                   std::string(context) + ") — clamped to 0").c_str());
        return 0;
    }
    if (raw > PL_VALUE_MAX_ARRAY_LEN) {
        ERR_PRINT(("[PolyLang/VariantBridge] Oversized array/dict len from adapter (" +
                   std::string(context) + ") — clamped to " +
                   std::to_string(PL_VALUE_MAX_ARRAY_LEN)).c_str());
        return PL_VALUE_MAX_ARRAY_LEN;
    }
    return raw;
}

void VariantBridge::to_pl_value(const godot::Variant& in, PLValue& out, int depth) {
    if (depth > PL_MAX_CONVERSION_DEPTH) {
        ERR_PRINT("[PolyLang/VariantBridge] to_pl_value: max depth exceeded");
        out = PLValue{};
        return;
    }
    out = PLValue{};

    switch (in.get_type()) {
        case godot::Variant::NIL:
            out.type = PL_TYPE_NIL;
            break;
        case godot::Variant::BOOL:
            out.type = PL_TYPE_BOOL;
            out.b    = static_cast<bool>(in);
            break;
        case godot::Variant::INT:
            out.type = PL_TYPE_INT;
            out.i    = static_cast<int64_t>(in);
            break;
        case godot::Variant::FLOAT:
            out.type = PL_TYPE_FLOAT;
            out.f    = static_cast<double>(in);
            break;
        case godot::Variant::STRING: {
            out.type = PL_TYPE_STRING;
            godot::String gs  = in;
            godot::CharString cs = gs.utf8();
            out.s = static_cast<char*>(malloc(cs.length() + 1));
            if (out.s) memcpy(out.s, cs.get_data(), cs.length() + 1);
            break;
        }
        case godot::Variant::VECTOR2: {
            out.type  = PL_TYPE_VEC2;
            godot::Vector2 v = in;
            out.v2[0] = v.x; out.v2[1] = v.y;
            break;
        }
        case godot::Variant::VECTOR3: {
            out.type  = PL_TYPE_VEC3;
            godot::Vector3 v = in;
            out.v3[0] = v.x; out.v3[1] = v.y; out.v3[2] = v.z;
            break;
        }
        case godot::Variant::QUATERNION: {
            out.type  = PL_TYPE_QUAT;
            godot::Quaternion q = in;
            out.q4[0] = q.x; out.q4[1] = q.y; out.q4[2] = q.z; out.q4[3] = q.w;
            break;
        }
        case godot::Variant::ARRAY: {
            out.type = PL_TYPE_ARRAY;
            godot::Array arr = in;
            // Godot Array.size() is always non-negative; still cap it.
            int32_t n = safe_len(arr.size(), "to_pl_value/array");
            out.array.len  = n;
            out.array._cap = n;
            out.array.data = static_cast<PLValue*>(calloc(n, sizeof(PLValue)));
            if (out.array.data) {
                for (int32_t i = 0; i < n; ++i)
                    to_pl_value(arr[i], out.array.data[i], depth + 1);
            }
            break;
        }
        case godot::Variant::DICTIONARY: {
            out.type = PL_TYPE_DICT;
            godot::Dictionary d = in;
            godot::Array keys   = d.keys();
            int32_t n           = safe_len(keys.size(), "to_pl_value/dict");
            out.dict.len    = n;
            out.dict._cap   = n;
            out.dict.keys   = static_cast<PLValue*>(calloc(n, sizeof(PLValue)));
            out.dict.values = static_cast<PLValue*>(calloc(n, sizeof(PLValue)));
            if (out.dict.keys && out.dict.values) {
                for (int32_t i = 0; i < n; ++i) {
                    to_pl_value(keys[i],    out.dict.keys[i],   depth + 1);
                    to_pl_value(d[keys[i]], out.dict.values[i], depth + 1);
                }
            }
            break;
        }
        default:
            out.type = PL_TYPE_NIL;
            break;
    }
}

godot::Variant VariantBridge::from_pl_value(const PLValue& in, int depth) {
    if (depth > PL_MAX_CONVERSION_DEPTH) {
        ERR_PRINT("[PolyLang/VariantBridge] from_pl_value: max depth exceeded");
        return godot::Variant();
    }

    switch (in.type) {
        case PL_TYPE_NIL:    return godot::Variant();
        case PL_TYPE_BOOL:   return godot::Variant(in.b);
        case PL_TYPE_INT:    return godot::Variant(in.i);
        case PL_TYPE_FLOAT:  return godot::Variant(in.f);
        case PL_TYPE_STRING:
            return in.s ? godot::String::utf8(in.s) : godot::String();
        case PL_TYPE_VEC2:
            return godot::Vector2(in.v2[0], in.v2[1]);
        case PL_TYPE_VEC3:
            return godot::Vector3(in.v3[0], in.v3[1], in.v3[2]);
        case PL_TYPE_QUAT:
            return godot::Quaternion(in.q4[0], in.q4[1], in.q4[2], in.q4[3]);

        case PL_TYPE_ARRAY: {
            godot::Array arr;
            if (in.array.data) {
                // FIX VLN-09: clamp len from untrusted adapter.
                int32_t n = safe_len(in.array.len, "from_pl_value/array");
                for (int32_t i = 0; i < n; ++i)
                    arr.push_back(from_pl_value(in.array.data[i], depth + 1));
            }
            return arr;
        }

        case PL_TYPE_DICT: {
            godot::Dictionary d;
            if (in.dict.keys && in.dict.values) {
                // FIX VLN-09: clamp len from untrusted adapter.
                int32_t n = safe_len(in.dict.len, "from_pl_value/dict");
                for (int32_t i = 0; i < n; ++i)
                    d[from_pl_value(in.dict.keys[i], depth + 1)] =
                        from_pl_value(in.dict.values[i], depth + 1);
            }
            return d;
        }

        default:
            return godot::Variant();
    }
}

void VariantBridge::free_pl_value(PLValue& v, int depth) {
    if (depth > PL_MAX_CONVERSION_DEPTH) {
        ERR_PRINT("[PolyLang/VariantBridge] free_pl_value: max depth exceeded");
        v.type = PL_TYPE_NIL;
        return;
    }

    switch (v.type) {
        case PL_TYPE_STRING:
            free(v.s);
            v.s = nullptr;
            break;
        case PL_TYPE_ARRAY:
            if (v.array.data) {
                // FIX VLN-09: clamp before freeing elements.
                int32_t n = safe_len(v.array.len, "free_pl_value/array");
                for (int32_t i = 0; i < n; ++i)
                    free_pl_value(v.array.data[i], depth + 1);
                free(v.array.data);
                v.array.data = nullptr;
            }
            break;
        case PL_TYPE_DICT:
            if (v.dict.keys) {
                int32_t n = safe_len(v.dict.len, "free_pl_value/dict");
                for (int32_t i = 0; i < n; ++i) {
                    free_pl_value(v.dict.keys[i], depth + 1);
                    if (v.dict.values) free_pl_value(v.dict.values[i], depth + 1);
                }
                free(v.dict.keys);
                if (v.dict.values) free(v.dict.values);
                v.dict.keys   = nullptr;
                v.dict.values = nullptr;
            }
            break;
        default:
            break;
    }
    v.type = PL_TYPE_NIL;
}

} // namespace polylang
