// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file service_worker.cpp
 * @brief Service Worker API implementation (skeleton)
 */

#include "browser/ServiceWorker.h"
#include <algorithm>
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"

namespace Zepra::Browser {

// Array wrapper for ServiceWorker results
namespace {
class ArrayObject : public Runtime::Object {
public:
    ArrayObject() : Object(Runtime::ObjectType::Array) {}
    
    void push(const Value& val) {
        elements_.push_back(val);
    }
};
}

// =============================================================================
// ServiceWorker Implementation
// =============================================================================

ServiceWorker::ServiceWorker(const std::string& scriptURL)
    : Object(Runtime::ObjectType::Ordinary)
    , scriptURL_(scriptURL) {}

void ServiceWorker::postMessage(const Value& message, 
                                 const std::vector<Object*>& transfer) {
    MessageEvent event;
    event.data = message;
    event.origin = scriptURL_;

    // Transfer ownership of transferable objects.
    for (auto* obj : transfer) {
        event.transferList.push_back(obj);
    }

    // Enqueue to worker's message queue.
    std::lock_guard<std::mutex> lock(messageMutex_);
    messageQueue_.push(std::move(event));
}

void ServiceWorker::setState(State state) {
    if (state_ != state) {
        state_ = state;
        if (onstatechange) {
            onstatechange(this);
        }
    }
}

// =============================================================================
// ServiceWorkerRegistration Implementation
// =============================================================================

ServiceWorkerRegistration::ServiceWorkerRegistration(const std::string& scope)
    : Object(Runtime::ObjectType::Ordinary)
    , scope_(scope) {}

Promise* ServiceWorkerRegistration::update() {
    Promise* p = new Promise();

    // Compare current script content with fetched version.
    if (active_) {
        std::string currentHash = active_->scriptHash();
        std::string fetchedSource = active_->fetchScriptSource();

        // Byte-for-byte comparison via hash.
        uint32_t newHash = 0;
        for (char c : fetchedSource) newHash = newHash * 31 + static_cast<uint8_t>(c);
        std::string newHashStr = std::to_string(newHash);

        if (currentHash != newHashStr && !fetchedSource.empty()) {
            ServiceWorker* sw = new ServiceWorker(active_->scriptURL());
            sw->setScriptHash(newHashStr);
            sw->setState(ServiceWorker::State::Installing);
            installing_ = sw;
        }
    }

    p->resolve(Value::object(this));
    return p;
}

Promise* ServiceWorkerRegistration::unregister() {
    Promise* p = new Promise();
    
    // Remove from container's list
    ServiceWorkerContainer* container = ServiceWorkerContainer::instance();
    
    // Mark workers as redundant
    if (active_) active_->setState(ServiceWorker::State::Redundant);
    if (waiting_) waiting_->setState(ServiceWorker::State::Redundant);
    if (installing_) installing_->setState(ServiceWorker::State::Redundant);
    
    p->resolve(Value::boolean(true));
    return p;
}

// =============================================================================
// ServiceWorkerContainer Implementation
// =============================================================================

ServiceWorkerContainer::ServiceWorkerContainer()
    : Object(Runtime::ObjectType::Ordinary) {}

ServiceWorkerContainer* ServiceWorkerContainer::instance() {
    static ServiceWorkerContainer container;
    return &container;
}

Promise* ServiceWorkerContainer::ready() {
    if (!readyPromise_) {
        readyPromise_ = new Promise();
        
        // If we already have an active controller, resolve immediately
        if (controller_) {
            for (auto* reg : registrations_) {
                if (reg->active() == controller_) {
                    readyPromise_->resolve(Value::object(reg));
                    return readyPromise_;
                }
            }
        }
        
        // Otherwise, will resolve when a worker becomes active
    }
    return readyPromise_;
}

Promise* ServiceWorkerContainer::registerWorker(const std::string& scriptURL,
                                                  const std::string& scope) {
    Promise* p = new Promise();
    
    // Determine scope
    std::string actualScope = scope;
    if (actualScope.empty()) {
        // Default to script directory
        size_t lastSlash = scriptURL.rfind('/');
        if (lastSlash != std::string::npos) {
            actualScope = scriptURL.substr(0, lastSlash + 1);
        } else {
            actualScope = "/";
        }
    }
    
    // Check for existing registration
    for (auto* reg : registrations_) {
        if (reg->scope() == actualScope) {
            // Update existing registration
            p->resolve(Value::object(reg));
            return p;
        }
    }
    
    // Create new registration
    ServiceWorkerRegistration* reg = new ServiceWorkerRegistration(actualScope);
    registrations_.push_back(reg);
    
    // Create installing worker
    ServiceWorker* sw = new ServiceWorker(scriptURL);
    sw->setState(ServiceWorker::State::Installing);
    reg->installing_ = sw;
    
    // Simulate installation
    // In real implementation, this would load and parse the script
    sw->setState(ServiceWorker::State::Installed);
    reg->waiting_ = sw;
    reg->installing_ = nullptr;
    
    // Simulate activation (in real impl, depends on skipWaiting/clients.claim)
    sw->setState(ServiceWorker::State::Activating);
    sw->setState(ServiceWorker::State::Activated);
    reg->active_ = sw;
    reg->waiting_ = nullptr;
    
    // Set as controller if this is first registration
    if (!controller_) {
        controller_ = sw;
        if (oncontrollerchange) {
            oncontrollerchange(sw);
        }
    }
    
    // Resolve ready promise if pending
    if (readyPromise_) {
        readyPromise_->resolve(Value::object(reg));
    }
    
    p->resolve(Value::object(reg));
    return p;
}

Promise* ServiceWorkerContainer::getRegistration(const std::string& scope) {
    Promise* p = new Promise();
    
    std::string targetScope = scope.empty() ? "/" : scope;
    
    for (auto* reg : registrations_) {
        if (reg->scope() == targetScope) {
            p->resolve(Value::object(reg));
            return p;
        }
    }
    
    p->resolve(Value::undefined());
    return p;
}

Promise* ServiceWorkerContainer::getRegistrations() {
    Promise* p = new Promise();
    
    Runtime::Array* arr = new Runtime::Array();
    for (auto* reg : registrations_) {
        arr->push(Value::object(reg));
    }
    
    p->resolve(Value::object(arr));
    return p;
}

void ServiceWorkerContainer::startMessages() {
    messagesStarted_ = true;
}

// =============================================================================
// ServiceWorkerGlobalScope Implementation
// =============================================================================

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope()
    : Object(Runtime::ObjectType::Global) {}

Promise* ServiceWorkerGlobalScope::skipWaiting() {
    Promise* p = new Promise();
    
    // Skip waiting and activate immediately
    if (serviceWorker_ && serviceWorker_->state() == ServiceWorker::State::Installed) {
        serviceWorker_->setState(ServiceWorker::State::Activating);
        serviceWorker_->setState(ServiceWorker::State::Activated);
        
        if (registration_) {
            registration_->active_ = serviceWorker_;
            registration_->waiting_ = nullptr;
        }
    }
    
    p->resolve(Value::undefined());
    return p;
}

Promise* ServiceWorkerGlobalScope::clients() {
    Promise* p = new Promise();
    
    // Return empty clients list for now
    Runtime::Array* clientList = new Runtime::Array();
    
    p->resolve(Value::object(clientList));
    return p;
}

// =============================================================================
// Builtin Functions
// =============================================================================

Value serviceWorkerBuiltin(Runtime::Context*, const std::vector<Value>&) {
    return Value::object(ServiceWorkerContainer::instance());
}

void initServiceWorker() {
    // Register navigator.serviceWorker on global object
}

} // namespace Zepra::Browser
