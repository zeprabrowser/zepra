// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file weakref.cpp
 * @brief JS WeakRef + FinalizationRegistry builtins
 *
 * WeakRef: holds a weak reference to a target object. deref() returns
 * the target if it hasn't been GC'd, undefined otherwise.
 *
 * FinalizationRegistry: registers cleanup callbacks that fire after
 * registered targets are GC'd.
 */

#include "runtime/objects/object.hpp"
#include <algorithm>
#include "runtime/objects/function.hpp"
#include "runtime/objects/value.hpp"

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;
using Runtime::ObjectType;
using Runtime::Function;
using Runtime::FunctionCallInfo;
using Runtime::String;

// =============================================================================
// WeakRefObject — wraps a raw Object* with GC weak-ref semantics
// =============================================================================

class WeakRefObject : public Object {
public:
    explicit WeakRefObject(Object* target)
        : Object(ObjectType::Ordinary), target_(target), alive_(true) {}

    Value deref() const {
        if (!alive_ || !target_) return Value::undefined();
        return Value::object(target_);
    }

    void clearTarget() {
        target_ = nullptr;
        alive_ = false;
    }

    Object* target() const { return target_; }
    bool alive() const { return alive_; }

private:
    Object* target_;
    bool alive_;
};

// =============================================================================
// FinalizationRegistryObject
// =============================================================================

class FinalizationRegistryObject : public Object {
public:
    explicit FinalizationRegistryObject(Function* callback)
        : Object(ObjectType::Ordinary), callback_(callback) {}

    struct Registration {
        Object* target;
        Value heldValue;
        Object* unregisterToken; // nullable
    };

    void registerTarget(Object* target, const Value& heldValue, Object* token) {
        registrations_.push_back({target, heldValue, token});
    }

    bool unregister(Object* token) {
        if (!token) return false;
        bool removed = false;
        registrations_.erase(
            std::remove_if(registrations_.begin(), registrations_.end(),
                [token, &removed](const Registration& r) {
                    if (r.unregisterToken == token) {
                        removed = true;
                        return true;
                    }
                    return false;
                }),
            registrations_.end());
        return removed;
    }

    void cleanupSome() {
        // In a real GC integration, this would check liveness.
        // For now, registrations are cleaned up when the GC notifies us.
        (void)callback_;
    }

    Function* callback() const { return callback_; }
    const std::vector<Registration>& registrations() const { return registrations_; }

private:
    Function* callback_;
    std::vector<Registration> registrations_;
};

// =============================================================================
// WeakRef constructor + prototype
// =============================================================================

namespace WeakRefBuiltin {

Value constructor(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        // TypeError: WeakRef target must be an object
        return Value::undefined();
    }
    return Value::object(new WeakRefObject(args[0].asObject()));
}

Object* createPrototype() {
    Object* proto = new Object();

    proto->set("deref", Value::object(
        new Function("deref", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            auto* wr = dynamic_cast<WeakRefObject*>(info.thisValue().asObject());
            if (!wr) return Value::undefined();
            return wr->deref();
        }, 0)));

    return proto;
}

} // namespace WeakRefBuiltin

// =============================================================================
// FinalizationRegistry constructor + prototype
// =============================================================================

namespace FinalizationRegistryBuiltin {

Value constructor(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value::undefined();
    }
    auto* fn = dynamic_cast<Function*>(args[0].asObject());
    if (!fn) return Value::undefined();
    return Value::object(new FinalizationRegistryObject(fn));
}

Object* createPrototype() {
    Object* proto = new Object();

    proto->set("register", Value::object(
        new Function("register", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            auto* fr = dynamic_cast<FinalizationRegistryObject*>(info.thisValue().asObject());
            if (!fr || info.argumentCount() < 2) return Value::undefined();

            if (!info.argument(0).isObject()) return Value::undefined();

            Object* target = info.argument(0).asObject();
            Value heldValue = info.argument(1);
            Object* token = nullptr;
            if (info.argumentCount() > 2 && info.argument(2).isObject()) {
                token = info.argument(2).asObject();
            }

            fr->registerTarget(target, heldValue, token);
            return Value::undefined();
        }, 3)));

    proto->set("unregister", Value::object(
        new Function("unregister", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::boolean(false);
            auto* fr = dynamic_cast<FinalizationRegistryObject*>(info.thisValue().asObject());
            if (!fr || info.argumentCount() < 1 || !info.argument(0).isObject()) {
                return Value::boolean(false);
            }
            return Value::boolean(fr->unregister(info.argument(0).asObject()));
        }, 1)));

    proto->set("cleanupSome", Value::object(
        new Function("cleanupSome", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            auto* fr = dynamic_cast<FinalizationRegistryObject*>(info.thisValue().asObject());
            if (fr) fr->cleanupSome();
            return Value::undefined();
        }, 0)));

    return proto;
}

} // namespace FinalizationRegistryBuiltin

} // namespace Zepra::Builtins
