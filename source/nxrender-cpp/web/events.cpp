// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "events.h"
#include "web/box/box_tree.h"
#include <algorithm>
#include <chrono>

namespace NXRender {
namespace Web {

// ==================================================================
// Event
// ==================================================================

Event::Event(const std::string& type, bool bubbles, bool cancelable)
    : type_(type), bubbles_(bubbles), cancelable_(cancelable) {
    timeStamp_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Event::~Event() {}

// ==================================================================
// MouseEvent
// ==================================================================

MouseEvent::MouseEvent(const std::string& type, float clientX, float clientY, int button)
    : Event(type, true, true), clientX_(clientX), clientY_(clientY), button_(button) {}

// ==================================================================
// KeyboardEvent
// ==================================================================

KeyboardEvent::KeyboardEvent(const std::string& type, const std::string& key,
                               const std::string& code)
    : Event(type, true, true), key_(key), code_(code) {}

// ==================================================================
// WheelEvent
// ==================================================================

WheelEvent::WheelEvent(float clientX, float clientY, float deltaX, float deltaY, float deltaZ)
    : MouseEvent("wheel", clientX, clientY, 0),
      deltaX_(deltaX), deltaY_(deltaY), deltaZ_(deltaZ) {}

// ==================================================================
// FocusEvent
// ==================================================================

FocusEvent::FocusEvent(const std::string& type, BoxNode* relatedTarget)
    : Event(type, false, false), relatedTarget_(relatedTarget) {}

// ==================================================================
// InputEvent
// ==================================================================

InputEvent::InputEvent(const std::string& inputType, const std::string& data)
    : Event("input", true, false), inputType_(inputType), data_(data) {}

// ==================================================================
// EventTarget
// ==================================================================

EventTarget::EventTarget() {}
EventTarget::~EventTarget() {}

uint32_t EventTarget::addEventListener(const std::string& type,
                                            std::function<void(Event&)> callback,
                                            const EventListenerOptions& options) {
    RegisteredListener rl;
    rl.type = type;
    rl.callback = std::move(callback);
    rl.options = options;
    rl.id = nextListenerId_++;
    listeners_.push_back(std::move(rl));
    return rl.id;
}

void EventTarget::removeEventListener(uint32_t listenerId) {
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                        [listenerId](const RegisteredListener& rl) { return rl.id == listenerId; }),
        listeners_.end());
}

void EventTarget::removeEventListener(const std::string& type, bool capture) {
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                        [&type, capture](const RegisteredListener& rl) {
                            return rl.type == type && rl.options.capture == capture;
                        }),
        listeners_.end());
}

bool EventTarget::dispatchEvent(Event& event) {
    std::vector<uint32_t> toRemove;

    for (auto& rl : listeners_) {
        if (rl.type != event.type()) continue;

        // Check phase matching
        bool isCapture = rl.options.capture;
        if (event.eventPhase() == Event::Phase::Capturing && !isCapture) continue;
        if (event.eventPhase() == Event::Phase::Bubbling && isCapture) continue;
        // At target: fire both capture and non-capture

        if (event.immediatePropagationStopped()) break;

        // Passive listeners cannot call preventDefault
        rl.callback(event);

        if (rl.options.once) {
            toRemove.push_back(rl.id);
        }
    }

    // Remove once-listeners
    for (uint32_t id : toRemove) {
        removeEventListener(id);
    }

    return !event.defaultPrevented();
}

bool EventTarget::hasEventListeners(const std::string& type) const {
    for (const auto& rl : listeners_) {
        if (rl.type == type) return true;
    }
    return false;
}

// ==================================================================
// EventDispatcher
// ==================================================================

bool EventDispatcher::dispatch(Event& event, BoxNode* target) {
    if (!target) return true;

    event.target_ = target;
    auto path = buildPath(target);

    // Phase 1: Capturing (root → parent of target)
    event.phase_ = Event::Phase::Capturing;
    for (size_t i = 0; i < path.size() - 1 && !event.propagationStopped(); i++) {
        event.currentTarget_ = path[i];
        invokeListeners(path[i], event, Event::Phase::Capturing);
    }

    // Phase 2: At target
    if (!event.propagationStopped()) {
        event.phase_ = Event::Phase::AtTarget;
        event.currentTarget_ = target;
        invokeListeners(target, event, Event::Phase::AtTarget);
    }

    // Phase 3: Bubbling (parent → root)
    if (event.bubbles() && !event.propagationStopped()) {
        event.phase_ = Event::Phase::Bubbling;
        for (int i = static_cast<int>(path.size()) - 2; i >= 0 && !event.propagationStopped(); i--) {
            event.currentTarget_ = path[i];
            invokeListeners(path[i], event, Event::Phase::Bubbling);
        }
    }

    event.phase_ = Event::Phase::None;
    event.currentTarget_ = nullptr;

    return !event.defaultPrevented();
}

std::vector<BoxNode*> EventDispatcher::buildPath(BoxNode* target) {
    std::vector<BoxNode*> path;
    BoxNode* current = target;
    while (current) {
        path.push_back(current);
        current = current->parent();
    }
    // Reverse: root first
    std::reverse(path.begin(), path.end());
    return path;
}

void EventDispatcher::invokeListeners(BoxNode* node, Event& event, Event::Phase phase) {
    (void)phase;
    // BoxNode stores an EventTarget via its domNode_ pointer.
    // The integration layer maps domNode_ → EventTarget.
    // Walk the node's listener list directly via the EventTarget interface.
    if (!node) return;

    // Access listener registry through the node's event target binding.
    // EventTargetRegistry is populated by the DOM bridge when event listeners
    // are attached via addEventListener() in ZepraScript.
    auto it = EventTargetRegistry::instance().targets.find(node);
    if (it != EventTargetRegistry::instance().targets.end()) {
        it->second->dispatchEvent(event);
    }
}

// ==================================================================
// FocusManager
// ==================================================================

FocusManager& FocusManager::instance() {
    static FocusManager inst;
    return inst;
}

void FocusManager::setFocus(BoxNode* node) {
    if (node == focused_) return;

    BoxNode* old = focused_;

    // Blur old element
    if (old) {
        FocusEvent blurEvent("blur", node);
        blurEvent.setTrusted(true);
        EventDispatcher::dispatch(blurEvent, old);

        FocusEvent focusOutEvent("focusout", node);
        focusOutEvent.setTrusted(true);
        EventDispatcher::dispatch(focusOutEvent, old);
    }

    focused_ = node;

    // Focus new element
    if (node) {
        FocusEvent focusEvent("focus", old);
        focusEvent.setTrusted(true);
        EventDispatcher::dispatch(focusEvent, node);

        FocusEvent focusInEvent("focusin", old);
        focusInEvent.setTrusted(true);
        EventDispatcher::dispatch(focusInEvent, node);
    }
}

void FocusManager::blur() {
    setFocus(nullptr);
}

bool FocusManager::isFocusable(BoxNode* node) {
    if (!node) return false;
    auto& cv = node->computed();
    if (cv.display == 0) return false;  // display:none
    if (cv.visibility == 1) return false; // hidden

    // Interactive elements: input, button, a, textarea, select
    // Or elements with tabindex >= 0
    // Simplified: check tag name
    const std::string& tag = node->tag();
    if (tag == "input" || tag == "button" || tag == "a" ||
        tag == "textarea" || tag == "select" || tag == "details" ||
        tag == "summary") {
        return true;
    }

    // Elements with tabindex >= 0 are focusable
    // The integration layer stores tabindex in ComputedValues::zIndex
    // when the element has an explicit tabindex attribute (checked via content convention)
    const std::string& content = cv.content;
    if (content.find("tabindex=") == 0) {
        int tabIdx = std::atoi(content.c_str() + 9);
        if (tabIdx >= 0) return true;
    }

    // contenteditable elements
    if (content.find("contenteditable") != std::string::npos) return true;

    return false;
}

void FocusManager::focusNext() {
    if (!root_) return;
    auto order = buildTabOrder(root_);
    if (order.empty()) return;

    if (!focused_) {
        setFocus(order[0]);
        return;
    }

    for (size_t i = 0; i < order.size(); i++) {
        if (order[i] == focused_) {
            setFocus(order[(i + 1) % order.size()]);
            return;
        }
    }
    setFocus(order[0]);
}

void FocusManager::focusPrev() {
    if (!root_) return;
    auto order = buildTabOrder(root_);
    if (order.empty()) return;

    if (!focused_) {
        setFocus(order.back());
        return;
    }

    for (size_t i = 0; i < order.size(); i++) {
        if (order[i] == focused_) {
            setFocus(order[(i + order.size() - 1) % order.size()]);
            return;
        }
    }
    setFocus(order.back());
}

std::vector<BoxNode*> FocusManager::buildTabOrder(BoxNode* root) {
    std::vector<BoxNode*> order;
    if (!root) return order;

    // DFS walk, collect focusable elements
    std::function<void(BoxNode*)> walk = [&](BoxNode* node) {
        if (isFocusable(node)) {
            order.push_back(node);
        }
        for (auto& child : node->children()) {
            walk(child.get());
        }
    };
    walk(root);

    return order;
}

} // namespace Web
} // namespace NXRender
