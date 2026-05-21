// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — native_binding.cpp — C++ ↔ JS binding, argument marshalling

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Zepra::Runtime {

union NativeValue {
    double number;
    int64_t integer;
    bool boolean;
    const char* string;
    void* object;
    uint64_t bits;
};

enum class NativeType : uint8_t {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Object,
    Function,
    Symbol,
    BigInt,
};

struct NativeArg {
    NativeValue value;
    NativeType type;

    NativeArg() : value{}, type(NativeType::Undefined) {}

    bool isNumber() const { return type == NativeType::Number; }
    bool isString() const { return type == NativeType::String; }
    bool isBool() const { return type == NativeType::Boolean; }
    bool isObject() const { return type == NativeType::Object; }
    bool isNull() const { return type == NativeType::Null; }
    bool isUndefined() const { return type == NativeType::Undefined; }

    double asNumber() const { return value.number; }
    int64_t asInteger() const { return value.integer; }
    bool asBool() const { return value.boolean; }
    const char* asString() const { return value.string; }
    void* asObject() const { return value.object; }
};

using NativeFn = std::function<NativeArg(NativeArg* args, uint8_t argc, void* thisObj)>;

struct BindingInfo {
    std::string name;
    NativeFn function;
    uint8_t minArgs;
    uint8_t maxArgs;
    bool isConstructor;
    bool isGetter;
    bool isSetter;
    const char* className;    // For method bindings

    BindingInfo() : minArgs(0), maxArgs(255), isConstructor(false)
        , isGetter(false), isSetter(false), className("") {}
};

class NativeBindingRegistry {
public:
    // Register a native function binding.
    void registerFunction(const std::string& name, NativeFn fn, uint8_t minArgs = 0,
                          uint8_t maxArgs = 255) {
        BindingInfo info;
        info.name = name;
        info.function = std::move(fn);
        info.minArgs = minArgs;
        info.maxArgs = maxArgs;
        bindings_[name] = info;
    }

    // Register a method on a class.
    void registerMethod(const char* className, const std::string& methodName,
                        NativeFn fn, uint8_t minArgs = 0) {
        std::string key = std::string(className) + "." + methodName;
        BindingInfo info;
        info.name = methodName;
        info.function = std::move(fn);
        info.minArgs = minArgs;
        info.className = className;
        bindings_[key] = info;
    }

    // Register getter/setter.
    void registerGetter(const char* className, const std::string& propName, NativeFn fn) {
        std::string key = std::string(className) + ".get_" + propName;
        BindingInfo info;
        info.name = propName;
        info.function = std::move(fn);
        info.isGetter = true;
        info.className = className;
        bindings_[key] = info;
    }

    void registerSetter(const char* className, const std::string& propName, NativeFn fn) {
        std::string key = std::string(className) + ".set_" + propName;
        BindingInfo info;
        info.name = propName;
        info.function = std::move(fn);
        info.isSetter = true;
        info.className = className;
        bindings_[key] = info;
    }

    // Invoke a native function.
    NativeArg invoke(const std::string& name, NativeArg* args, uint8_t argc,
                     void* thisObj = nullptr) {
        auto it = bindings_.find(name);
        if (it == bindings_.end()) {
            NativeArg err;
            err.type = NativeType::Undefined;
            return err;
        }

        const BindingInfo& info = it->second;

        // Argument count validation.
        if (argc < info.minArgs) {
            NativeArg err;
            err.type = NativeType::Undefined;
            return err;
        }

        return info.function(args, argc, thisObj);
    }

    bool hasBinding(const std::string& name) const {
        return bindings_.count(name) > 0;
    }

    const BindingInfo* getBinding(const std::string& name) const {
        auto it = bindings_.find(name);
        return it != bindings_.end() ? &it->second : nullptr;
    }

    size_t bindingCount() const { return bindings_.size(); }

    // Enumerate all bindings for a class.
    std::vector<std::string> methodsForClass(const char* className) const {
        std::vector<std::string> methods;
        std::string prefix = std::string(className) + ".";
        for (auto& [key, info] : bindings_) {
            if (key.compare(0, prefix.size(), prefix) == 0) {
                methods.push_back(info.name);
            }
        }
        return methods;
    }

private:
    std::unordered_map<std::string, BindingInfo> bindings_;
};

// Argument marshalling helpers.
namespace Marshal {
    inline NativeArg fromNumber(double d) {
        NativeArg a; a.type = NativeType::Number; a.value.number = d; return a;
    }
    inline NativeArg fromBool(bool b) {
        NativeArg a; a.type = NativeType::Boolean; a.value.boolean = b; return a;
    }
    inline NativeArg fromString(const char* s) {
        NativeArg a; a.type = NativeType::String; a.value.string = s; return a;
    }
    inline NativeArg fromObject(void* o) {
        NativeArg a; a.type = NativeType::Object; a.value.object = o; return a;
    }
    inline NativeArg undefined() {
        NativeArg a; a.type = NativeType::Undefined; return a;
    }
    inline NativeArg null() {
        NativeArg a; a.type = NativeType::Null; return a;
    }
}

} // namespace Zepra::Runtime
