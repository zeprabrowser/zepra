// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — coercion.cpp — ES spec type coercion: ToPrimitive, ToNumber, etc.

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <functional>
#include <limits>

namespace Zepra::Runtime {

enum class CoercionHint : uint8_t {
    Default,
    Number,
    String,
};

union CoercedValue {
    double number;
    bool boolean;
    const char* string;
    void* object;
    uint64_t bits;
};

enum class ValueType : uint8_t {
    Undefined, Null, Boolean, Number, String, Symbol, BigInt, Object,
};

struct TypedValue {
    CoercedValue value;
    ValueType type;

    TypedValue() : value{}, type(ValueType::Undefined) {}
};

class TypeCoercion {
public:
    struct Callbacks {
        // Get [Symbol.toPrimitive] from object.
        std::function<bool(void* obj, CoercionHint hint, TypedValue& result)> toPrimitive;
        // Call valueOf() on object.
        std::function<bool(void* obj, TypedValue& result)> valueOf;
        // Call toString() on object.
        std::function<bool(void* obj, TypedValue& result)> toString;
        // Get typeof string.
        std::function<const char*(void* obj)> typeOf;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // 7.1.1 ToPrimitive.
    TypedValue toPrimitive(const TypedValue& input, CoercionHint hint = CoercionHint::Default) {
        if (input.type != ValueType::Object) return input;

        // Try [Symbol.toPrimitive] first.
        if (cb_.toPrimitive) {
            TypedValue result;
            if (cb_.toPrimitive(input.value.object, hint, result)) {
                if (result.type != ValueType::Object) return result;
            }
        }

        // OrdinaryToPrimitive.
        if (hint == CoercionHint::String || hint == CoercionHint::Default) {
            // Try toString first, then valueOf.
            TypedValue result;
            if (cb_.toString && cb_.toString(input.value.object, result)) {
                if (result.type != ValueType::Object) return result;
            }
            if (cb_.valueOf && cb_.valueOf(input.value.object, result)) {
                if (result.type != ValueType::Object) return result;
            }
        } else {
            // Try valueOf first, then toString.
            TypedValue result;
            if (cb_.valueOf && cb_.valueOf(input.value.object, result)) {
                if (result.type != ValueType::Object) return result;
            }
            if (cb_.toString && cb_.toString(input.value.object, result)) {
                if (result.type != ValueType::Object) return result;
            }
        }

        // TypeError: cannot convert object to primitive.
        TypedValue err;
        err.type = ValueType::Undefined;
        return err;
    }

    // 7.1.3 ToNumber.
    double toNumber(const TypedValue& input) {
        switch (input.type) {
            case ValueType::Undefined: return std::numeric_limits<double>::quiet_NaN();
            case ValueType::Null: return 0.0;
            case ValueType::Boolean: return input.value.boolean ? 1.0 : 0.0;
            case ValueType::Number: return input.value.number;
            case ValueType::String: return stringToNumber(input.value.string);
            case ValueType::Symbol: return std::numeric_limits<double>::quiet_NaN();
            case ValueType::BigInt: return std::numeric_limits<double>::quiet_NaN();
            case ValueType::Object: {
                TypedValue prim = toPrimitive(input, CoercionHint::Number);
                return toNumber(prim);
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // 7.1.17 ToString.
    std::string toString(const TypedValue& input) {
        switch (input.type) {
            case ValueType::Undefined: return "undefined";
            case ValueType::Null: return "null";
            case ValueType::Boolean: return input.value.boolean ? "true" : "false";
            case ValueType::Number: return numberToString(input.value.number);
            case ValueType::String: return input.value.string ? input.value.string : "";
            case ValueType::Symbol: return "";  // TypeError in strict mode
            case ValueType::BigInt: return "0";  // Simplified
            case ValueType::Object: {
                TypedValue prim = toPrimitive(input, CoercionHint::String);
                return toString(prim);
            }
        }
        return "";
    }

    // 7.1.2 ToBoolean.
    bool toBoolean(const TypedValue& input) {
        switch (input.type) {
            case ValueType::Undefined: return false;
            case ValueType::Null: return false;
            case ValueType::Boolean: return input.value.boolean;
            case ValueType::Number:
                return input.value.number != 0.0 &&
                       !std::isnan(input.value.number);
            case ValueType::String:
                return input.value.string && input.value.string[0] != '\0';
            case ValueType::Symbol: return true;
            case ValueType::BigInt: return false;  // Simplified
            case ValueType::Object: return true;
        }
        return false;
    }

    // 7.1.4 ToInteger.
    int64_t toInteger(const TypedValue& input) {
        double n = toNumber(input);
        if (std::isnan(n) || n == 0.0) return 0;
        if (std::isinf(n)) return n > 0 ? INT64_MAX : INT64_MIN;
        return static_cast<int64_t>(n);
    }

    // 7.1.5 ToInt32.
    int32_t toInt32(const TypedValue& input) {
        double n = toNumber(input);
        if (std::isnan(n) || std::isinf(n) || n == 0.0) return 0;
        double d = std::fmod(std::trunc(n), 4294967296.0);
        if (d >= 2147483648.0) d -= 4294967296.0;
        return static_cast<int32_t>(d);
    }

    // 7.1.6 ToUint32.
    uint32_t toUint32(const TypedValue& input) {
        double n = toNumber(input);
        if (std::isnan(n) || std::isinf(n) || n == 0.0) return 0;
        double d = std::fmod(std::trunc(n), 4294967296.0);
        if (d < 0) d += 4294967296.0;
        return static_cast<uint32_t>(d);
    }

    // 7.1.14 ToObject.
    TypedValue toObject(const TypedValue& input) {
        // Undefined/null → TypeError (not representable here).
        // Primitives → wrapper objects (handled by runtime).
        return input;
    }

    // 7.2.13 Abstract equality (==).
    bool abstractEqual(const TypedValue& x, const TypedValue& y) {
        if (x.type == y.type) return strictEqual(x, y);

        // null == undefined (and vice versa).
        if ((x.type == ValueType::Null && y.type == ValueType::Undefined) ||
            (x.type == ValueType::Undefined && y.type == ValueType::Null)) {
            return true;
        }

        // Number comparisons.
        if (x.type == ValueType::Number && y.type == ValueType::String) {
            return x.value.number == stringToNumber(y.value.string);
        }
        if (x.type == ValueType::String && y.type == ValueType::Number) {
            return stringToNumber(x.value.string) == y.value.number;
        }

        // Boolean → Number.
        if (x.type == ValueType::Boolean) {
            TypedValue xn;
            xn.type = ValueType::Number;
            xn.value.number = x.value.boolean ? 1.0 : 0.0;
            return abstractEqual(xn, y);
        }
        if (y.type == ValueType::Boolean) {
            TypedValue yn;
            yn.type = ValueType::Number;
            yn.value.number = y.value.boolean ? 1.0 : 0.0;
            return abstractEqual(x, yn);
        }

        return false;
    }

    // 7.2.14 Strict equality (===).
    bool strictEqual(const TypedValue& x, const TypedValue& y) {
        if (x.type != y.type) return false;
        switch (x.type) {
            case ValueType::Undefined:
            case ValueType::Null: return true;
            case ValueType::Number:
                if (std::isnan(x.value.number) || std::isnan(y.value.number)) return false;
                return x.value.number == y.value.number;
            case ValueType::Boolean: return x.value.boolean == y.value.boolean;
            case ValueType::String:
                if (!x.value.string || !y.value.string) return x.value.string == y.value.string;
                return strcmp(x.value.string, y.value.string) == 0;
            case ValueType::Object: return x.value.object == y.value.object;
            case ValueType::Symbol: return x.value.bits == y.value.bits;
            default: return false;
        }
    }

private:
    double stringToNumber(const char* str) const {
        if (!str || !str[0]) return 0.0;
        // Skip whitespace.
        while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
        if (!*str) return 0.0;

        // Handle special cases.
        if (strcmp(str, "Infinity") == 0 || strcmp(str, "+Infinity") == 0)
            return std::numeric_limits<double>::infinity();
        if (strcmp(str, "-Infinity") == 0)
            return -std::numeric_limits<double>::infinity();

        char* end = nullptr;
        double result = strtod(str, &end);
        // If entire string wasn't consumed, it's NaN.
        while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        if (end && *end != '\0') return std::numeric_limits<double>::quiet_NaN();
        return result;
    }

    std::string numberToString(double n) const {
        if (std::isnan(n)) return "NaN";
        if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
        if (n == 0.0) return "0";

        // Check if integer.
        if (n == std::floor(n) && std::abs(n) < 1e15) {
            int64_t i = static_cast<int64_t>(n);
            return std::to_string(i);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", n);
        return buf;
    }

    Callbacks cb_;
};

} // namespace Zepra::Runtime
