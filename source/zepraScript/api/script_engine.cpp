// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file script_engine.cpp
 * @brief ScriptEngine — high-level embedding interface
 *
 * Owns an Isolate + Context and exposes a simple API
 * for executing JS, registering native functions, and
 * accessing globals.
 */

#include "script_engine.hpp"
#include <algorithm>
#include "runtime/objects/function.hpp"
#include <fstream>
#include <sstream>

namespace Zepra {

class ScriptEngineImpl : public ScriptEngine {
public:
    explicit ScriptEngineImpl(const IsolateOptions& options)
        : isolate_(Isolate::create(options))
    {
        context_ = isolate_->createContext();
    }

    ~ScriptEngineImpl() override = default;

    Result<Value> execute(std::string_view source,
                          std::string_view filename) override {
        if (!context_) {
            return Result<Value>(std::string("InternalError: no context"));
        }
        return context_->evaluate(source, filename);
    }

    Result<Value> executeFile(std::string_view filepath) override {
        std::string path{filepath};
        std::ifstream file{path};
        if (!file.is_open()) {
            return Result<Value>(std::string("Error: cannot open file: ") + std::string(filepath));
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        return execute(ss.str(), filepath);
    }

    void registerFunction(std::string_view name, NativeCallback callback) override {
        if (!context_) return;
        // Wrap NativeCallback → NativeFn (Context* is unused in embedding API)
        Runtime::NativeFn nativeFn = [cb = std::move(callback)](
            Runtime::Context* ctx, const std::vector<Runtime::Value>& args) -> Runtime::Value {
            return cb(nullptr, args);
        };
        auto* fn = new Runtime::Function(std::string(name), std::move(nativeFn), 0);
        context_->globalObject()->set(std::string(name), Value::object(fn));
    }

    void setGlobal(std::string_view name, Value value) override {
        if (context_) {
            context_->globalObject()->set(std::string(name), value);
        }
    }

    Value getGlobal(std::string_view name) override {
        if (context_) {
            return context_->globalObject()->get(std::string(name));
        }
        return Value::undefined();
    }

    Isolate* isolate() override { return isolate_.get(); }
    Context* context() override { return context_.get(); }

private:
    std::unique_ptr<Isolate> isolate_;
    std::unique_ptr<Context> context_;
};

std::unique_ptr<ScriptEngine> ScriptEngine::create(const IsolateOptions& options) {
    return std::make_unique<ScriptEngineImpl>(options);
}

} // namespace Zepra
