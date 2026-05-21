// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — reflect_impl.cpp — Reflect API: apply, construct, defineProperty, etc.

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace Zepra::Runtime {

struct ReflectPropertyDescriptor {
    uint64_t valueBits;
    uint64_t getBits;
    uint64_t setBits;
    bool hasValue;
    bool hasGet;
    bool hasSet;
    bool writable;
    bool enumerable;
    bool configurable;
    bool hasWritable;
    bool hasEnumerable;
    bool hasConfigurable;

    ReflectPropertyDescriptor() : valueBits(0), getBits(0), setBits(0)
        , hasValue(false), hasGet(false), hasSet(false)
        , writable(true), enumerable(true), configurable(true)
        , hasWritable(false), hasEnumerable(false), hasConfigurable(false) {}
};

class ReflectImpl {
public:
    struct Callbacks {
        // Object operations.
        std::function<uint64_t(void* fn, void* thisObj, uint64_t* args, size_t argc)> apply;
        std::function<void*(void* constructor, uint64_t* args, size_t argc, void* newTarget)> construct;
        std::function<bool(void* obj, const std::string& name, const ReflectPropertyDescriptor& desc)> defineProperty;
        std::function<bool(void* obj, const std::string& name)> deleteProperty;
        std::function<uint64_t(void* obj, const std::string& name, void* receiver)> get;
        std::function<bool(void* obj, const std::string& name, uint64_t value, void* receiver)> set;
        std::function<bool(void* obj, const std::string& name)> has;
        std::function<void*(void* obj)> getPrototypeOf;
        std::function<bool(void* obj, void* proto)> setPrototypeOf;
        std::function<bool(void* obj)> isExtensible;
        std::function<bool(void* obj)> preventExtensions;
        std::function<std::vector<std::string>(void* obj)> ownKeys;
        std::function<bool(void* obj, const std::string& name, ReflectPropertyDescriptor& desc)> getOwnPropertyDescriptor;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Reflect.apply(target, thisArg, argumentsList)
    uint64_t apply(void* target, void* thisArg, uint64_t* args, size_t argc) {
        if (!cb_.apply) return 0;
        stats_.applyCount++;
        return cb_.apply(target, thisArg, args, argc);
    }

    // Reflect.construct(target, argumentsList [, newTarget])
    void* construct(void* target, uint64_t* args, size_t argc, void* newTarget = nullptr) {
        if (!cb_.construct) return nullptr;
        stats_.constructCount++;
        return cb_.construct(target, args, argc, newTarget ? newTarget : target);
    }

    // Reflect.defineProperty(target, propertyKey, attributes)
    bool defineProperty(void* target, const std::string& name,
                        const ReflectPropertyDescriptor& desc) {
        if (!cb_.defineProperty) return false;
        return cb_.defineProperty(target, name, desc);
    }

    // Reflect.deleteProperty(target, propertyKey)
    bool deleteProperty(void* target, const std::string& name) {
        if (!cb_.deleteProperty) return false;
        return cb_.deleteProperty(target, name);
    }

    // Reflect.get(target, propertyKey [, receiver])
    uint64_t get(void* target, const std::string& name, void* receiver = nullptr) {
        if (!cb_.get) return 0;
        stats_.getCount++;
        return cb_.get(target, name, receiver ? receiver : target);
    }

    // Reflect.set(target, propertyKey, value [, receiver])
    bool set(void* target, const std::string& name, uint64_t value,
             void* receiver = nullptr) {
        if (!cb_.set) return false;
        stats_.setCount++;
        return cb_.set(target, name, value, receiver ? receiver : target);
    }

    // Reflect.has(target, propertyKey)
    bool has(void* target, const std::string& name) {
        if (!cb_.has) return false;
        return cb_.has(target, name);
    }

    // Reflect.getPrototypeOf(target)
    void* getPrototypeOf(void* target) {
        if (!cb_.getPrototypeOf) return nullptr;
        return cb_.getPrototypeOf(target);
    }

    // Reflect.setPrototypeOf(target, proto)
    bool setPrototypeOf(void* target, void* proto) {
        if (!cb_.setPrototypeOf) return false;
        return cb_.setPrototypeOf(target, proto);
    }

    // Reflect.isExtensible(target)
    bool isExtensible(void* target) {
        if (!cb_.isExtensible) return true;
        return cb_.isExtensible(target);
    }

    // Reflect.preventExtensions(target)
    bool preventExtensions(void* target) {
        if (!cb_.preventExtensions) return false;
        return cb_.preventExtensions(target);
    }

    // Reflect.ownKeys(target)
    std::vector<std::string> ownKeys(void* target) {
        if (!cb_.ownKeys) return {};
        return cb_.ownKeys(target);
    }

    // Reflect.getOwnPropertyDescriptor(target, propertyKey)
    bool getOwnPropertyDescriptor(void* target, const std::string& name,
                                   ReflectPropertyDescriptor& desc) {
        if (!cb_.getOwnPropertyDescriptor) return false;
        return cb_.getOwnPropertyDescriptor(target, name, desc);
    }

    struct Stats {
        uint64_t applyCount = 0;
        uint64_t constructCount = 0;
        uint64_t getCount = 0;
        uint64_t setCount = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    Callbacks cb_;
    Stats stats_;
};

} // namespace Zepra::Runtime
