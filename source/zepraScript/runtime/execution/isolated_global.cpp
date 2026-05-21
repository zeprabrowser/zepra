// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — isolated_global.cpp — Per-realm isolated global, frozen intrinsics

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>

namespace Zepra::Runtime {

struct GlobalProperty {
    uint64_t valueBits;
    bool writable;
    bool enumerable;
    bool configurable;
    bool frozen;            // Part of frozen intrinsics
    const char* name;

    GlobalProperty() : valueBits(0), writable(true), enumerable(true)
        , configurable(true), frozen(false), name("") {}
};

enum class IntrinsicId : uint16_t {
    Object,
    ObjectPrototype,
    Function,
    FunctionPrototype,
    Array,
    ArrayPrototype,
    String,
    StringPrototype,
    Number,
    NumberPrototype,
    Boolean,
    BooleanPrototype,
    Symbol,
    SymbolPrototype,
    Error,
    TypeError,
    RangeError,
    SyntaxError,
    ReferenceError,
    URIError,
    EvalError,
    Promise,
    PromisePrototype,
    Map,
    MapPrototype,
    Set,
    SetPrototype,
    WeakMap,
    WeakSet,
    WeakRef,
    FinalizationRegistry,
    RegExp,
    RegExpPrototype,
    Date,
    DatePrototype,
    JSON,
    Math,
    Reflect,
    Proxy,
    ArrayBuffer,
    SharedArrayBuffer,
    DataView,
    TypedArrayBase,
    Int8Array,
    Uint8Array,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
    BigInt64Array,
    BigUint64Array,
    BigInt,
    Atomics,
    Iterator,
    AsyncIterator,
    GeneratorFunction,
    AsyncGeneratorFunction,
    AsyncFunction,
    Count,
};

class IsolatedGlobal {
public:
    explicit IsolatedGlobal(uint32_t realmId) : realmId_(realmId), frozen_(false) {}

    // Install a global property.
    void defineProperty(const std::string& name, uint64_t valueBits,
                        bool writable = true, bool enumerable = true,
                        bool configurable = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frozen_) return;

        GlobalProperty prop;
        prop.valueBits = valueBits;
        prop.writable = writable;
        prop.enumerable = enumerable;
        prop.configurable = configurable;
        prop.name = "";  // Stored by key in map
        properties_[name] = prop;
    }

    bool getProperty(const std::string& name, uint64_t& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = properties_.find(name);
        if (it == properties_.end()) return false;
        out = it->second.valueBits;
        return true;
    }

    bool setProperty(const std::string& name, uint64_t valueBits) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = properties_.find(name);
        if (it == properties_.end()) return false;
        if (!it->second.writable) return false;
        if (it->second.frozen) return false;
        it->second.valueBits = valueBits;
        return true;
    }

    bool deleteProperty(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = properties_.find(name);
        if (it == properties_.end()) return false;
        if (!it->second.configurable) return false;
        properties_.erase(it);
        return true;
    }

    bool hasProperty(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return properties_.count(name) > 0;
    }

    // Intrinsic registry — per-realm frozen copies.
    void registerIntrinsic(IntrinsicId id, uint64_t valueBits) {
        intrinsics_[static_cast<size_t>(id)] = valueBits;
    }

    uint64_t getIntrinsic(IntrinsicId id) const {
        size_t idx = static_cast<size_t>(id);
        return idx < static_cast<size_t>(IntrinsicId::Count) ? intrinsics_[idx] : 0;
    }

    // Freeze all intrinsics (ShadowRealm requirement).
    void freezeIntrinsics() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, prop] : properties_) {
            if (isIntrinsicProperty(name)) {
                prop.writable = false;
                prop.configurable = false;
                prop.frozen = true;
            }
        }
    }

    // Freeze entire global (for lockdown).
    void freeze() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, prop] : properties_) {
            prop.writable = false;
            prop.configurable = false;
            prop.frozen = true;
        }
        frozen_ = true;
    }

    bool isFrozen() const { return frozen_; }
    uint32_t realmId() const { return realmId_; }

    // Enumerate own enumerable property names.
    std::vector<std::string> ownPropertyNames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (auto& [name, prop] : properties_) {
            if (prop.enumerable) names.push_back(name);
        }
        return names;
    }

    size_t propertyCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return properties_.size();
    }

    // Host hook for ShadowRealm communication.
    using HostCallbackFn = std::function<uint64_t(const std::string& name,
                                                   const std::vector<uint64_t>& args)>;
    void setHostCallback(HostCallbackFn fn) { hostCallback_ = std::move(fn); }

    uint64_t callHostHook(const std::string& name, const std::vector<uint64_t>& args) {
        if (hostCallback_) return hostCallback_(name, args);
        return 0;
    }

private:
    bool isIntrinsicProperty(const std::string& name) const {
        static const std::unordered_set<std::string> intrinsicNames = {
            "Object", "Function", "Array", "String", "Number", "Boolean",
            "Symbol", "Error", "TypeError", "RangeError", "SyntaxError",
            "ReferenceError", "Promise", "Map", "Set", "WeakMap", "WeakSet",
            "RegExp", "Date", "JSON", "Math", "Reflect", "Proxy",
            "ArrayBuffer", "DataView", "BigInt", "Atomics",
        };
        return intrinsicNames.count(name) > 0;
    }

    uint32_t realmId_;
    bool frozen_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, GlobalProperty> properties_;
    uint64_t intrinsics_[static_cast<size_t>(IntrinsicId::Count)] = {};
    HostCallbackFn hostCallback_;
};

} // namespace Zepra::Runtime
