// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "event.h"
#include <algorithm>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace NXRender {
namespace Input {

using EventCallback = std::function<void(Event&)>;

struct EventListener {
    EventType type;
    EventCallback callback;
    bool useCapture;
};

// Abstract interface for any UI Node that can receive events
class EventTarget {
public:
    virtual ~EventTarget() = default;

    // Node hierarchy traversal required for routing algorithms
    virtual EventTarget* getParentTarget() const = 0;
    
    // Spatial Hit-Testing required for spatial dispatch
    virtual bool hitTest(float x, float y) const = 0;
    
    // Fetches the deepest child that intersects (x, y)
    virtual EventTarget* hitTestDeep(float x, float y) = 0;

    void addEventListener(EventType type, EventCallback cb, bool useCapture = false) {
        listeners_.push_back({type, std::move(cb), useCapture});
    }

    void removeEventListener(EventType type, bool useCapture = false) {
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                [&](const EventListener& l) { return l.type == type && l.useCapture == useCapture; }),
            listeners_.end()
        );
    }

    const std::vector<EventListener>& listeners() const { return listeners_; }

private:
    std::vector<EventListener> listeners_;
};

// Central Hub responsible for propagating physics and input events recursively
class EventRouter {
public:
    EventRouter(EventTarget* rootTarget);
    ~EventRouter();

    // Re-assign root dynamically (e.g. if overlay stack shifts priority)
    void setRootTarget(EventTarget* target);

    // Primary Dispatch Engine: Executes Capture -> Target -> Bubble phases sequentially
    bool dispatchEvent(EventTarget* target, Event& event);

    // Spatial Dispatch: Locates the exact node under cursor and routes a given mouse/scroll event
    bool dispatchSpatialEvent(float localX, float localY, Event& event);

    // Pointer-State Access
    void setPointerState(EventTarget* currentHover);
    EventTarget* getHoverTarget() const { return hoverTarget_; }

    // Modal isolation: If set, routing is strictly constrained to this branch
    void setModalBarrier(EventTarget* modalTarget);
    void clearModalBarrier();

private:
    EventTarget* rootTarget_ = nullptr;
    EventTarget* hoverTarget_ = nullptr;
    EventTarget* modalBarrier_ = nullptr; // Crucial for Dropdowns/Dialogs

    // Core execution pipeline
    void executeListeners(EventTarget* target, Event& event, EventPhase matchingPhase);
    
    // Builds the propagation chain from Root to Target
    std::vector<EventTarget*> buildPropagationPath(EventTarget* target) const;
};

} // namespace Input
} // namespace NXRender
