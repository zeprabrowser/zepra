// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace NXRender {
namespace Web {

class BoxNode;

// ==================================================================
// DOM Event (W3C DOM Events spec)
// ==================================================================

class Event {
public:
    enum class Phase : uint8_t {
        None = 0, Capturing = 1, AtTarget = 2, Bubbling = 3
    };

    Event(const std::string& type, bool bubbles = true, bool cancelable = true);
    virtual ~Event();

    const std::string& type() const { return type_; }
    BoxNode* target() const { return target_; }
    BoxNode* currentTarget() const { return currentTarget_; }
    Phase eventPhase() const { return phase_; }
    bool bubbles() const { return bubbles_; }
    bool cancelable() const { return cancelable_; }
    double timeStamp() const { return timeStamp_; }

    void stopPropagation() { propagationStopped_ = true; }
    void stopImmediatePropagation() { immediatePropagationStopped_ = true; propagationStopped_ = true; }
    void preventDefault() { if (cancelable_) defaultPrevented_ = true; }

    bool defaultPrevented() const { return defaultPrevented_; }
    bool propagationStopped() const { return propagationStopped_; }
    bool immediatePropagationStopped() const { return immediatePropagationStopped_; }

    bool isTrusted() const { return trusted_; }
    void setTrusted(bool t) { trusted_ = t; }

protected:
    friend class EventDispatcher;

    std::string type_;
    BoxNode* target_ = nullptr;
    BoxNode* currentTarget_ = nullptr;
    Phase phase_ = Phase::None;
    bool bubbles_;
    bool cancelable_;
    double timeStamp_ = 0;
    bool defaultPrevented_ = false;
    bool propagationStopped_ = false;
    bool immediatePropagationStopped_ = false;
    bool trusted_ = false;
};

// ==================================================================
// Mouse Event
// ==================================================================

class MouseEvent : public Event {
public:
    MouseEvent(const std::string& type, float clientX, float clientY, int button = 0);

    float clientX() const { return clientX_; }
    float clientY() const { return clientY_; }
    float pageX() const { return pageX_; }
    float pageY() const { return pageY_; }
    float screenX() const { return screenX_; }
    float screenY() const { return screenY_; }
    float offsetX() const { return offsetX_; }
    float offsetY() const { return offsetY_; }

    int button() const { return button_; }
    uint16_t buttons() const { return buttons_; }

    bool altKey() const { return altKey_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool metaKey() const { return metaKey_; }

    BoxNode* relatedTarget() const { return relatedTarget_; }

    void setPageCoords(float px, float py) { pageX_ = px; pageY_ = py; }
    void setScreenCoords(float sx, float sy) { screenX_ = sx; screenY_ = sy; }
    void setOffsetCoords(float ox, float oy) { offsetX_ = ox; offsetY_ = oy; }
    void setModifiers(bool alt, bool ctrl, bool shift, bool meta) {
        altKey_ = alt; ctrlKey_ = ctrl; shiftKey_ = shift; metaKey_ = meta;
    }
    void setButtons(uint16_t btns) { buttons_ = btns; }
    void setRelatedTarget(BoxNode* rt) { relatedTarget_ = rt; }

private:
    float clientX_, clientY_;
    float pageX_ = 0, pageY_ = 0;
    float screenX_ = 0, screenY_ = 0;
    float offsetX_ = 0, offsetY_ = 0;
    int button_;
    uint16_t buttons_ = 0;
    bool altKey_ = false, ctrlKey_ = false, shiftKey_ = false, metaKey_ = false;
    BoxNode* relatedTarget_ = nullptr;
};

// ==================================================================
// Keyboard Event
// ==================================================================

class KeyboardEvent : public Event {
public:
    KeyboardEvent(const std::string& type, const std::string& key, const std::string& code);

    const std::string& key() const { return key_; }
    const std::string& code() const { return code_; }
    uint32_t keyCode() const { return keyCode_; }
    bool repeat() const { return repeat_; }

    bool altKey() const { return altKey_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool metaKey() const { return metaKey_; }

    void setKeyCode(uint32_t kc) { keyCode_ = kc; }
    void setRepeat(bool r) { repeat_ = r; }
    void setModifiers(bool alt, bool ctrl, bool shift, bool meta) {
        altKey_ = alt; ctrlKey_ = ctrl; shiftKey_ = shift; metaKey_ = meta;
    }

private:
    std::string key_;
    std::string code_;
    uint32_t keyCode_ = 0;
    bool repeat_ = false;
    bool altKey_ = false, ctrlKey_ = false, shiftKey_ = false, metaKey_ = false;
};

// ==================================================================
// Wheel Event
// ==================================================================

class WheelEvent : public MouseEvent {
public:
    enum class DeltaMode : uint8_t { Pixel = 0, Line = 1, Page = 2 };

    WheelEvent(float clientX, float clientY, float deltaX, float deltaY, float deltaZ = 0);

    float deltaX() const { return deltaX_; }
    float deltaY() const { return deltaY_; }
    float deltaZ() const { return deltaZ_; }
    DeltaMode deltaMode() const { return deltaMode_; }
    void setDeltaMode(DeltaMode m) { deltaMode_ = m; }

private:
    float deltaX_, deltaY_, deltaZ_;
    DeltaMode deltaMode_ = DeltaMode::Pixel;
};

// ==================================================================
// Focus Event
// ==================================================================

class FocusEvent : public Event {
public:
    FocusEvent(const std::string& type, BoxNode* relatedTarget = nullptr);
    BoxNode* relatedTarget() const { return relatedTarget_; }

private:
    BoxNode* relatedTarget_;
};

// ==================================================================
// Input Event
// ==================================================================

class InputEvent : public Event {
public:
    InputEvent(const std::string& inputType, const std::string& data = "");

    const std::string& inputType() const { return inputType_; }
    const std::string& data() const { return data_; }
    bool isComposing() const { return isComposing_; }
    void setComposing(bool c) { isComposing_ = c; }

private:
    std::string inputType_;
    std::string data_;
    bool isComposing_ = false;
};

// ==================================================================
// Listener registration
// ==================================================================

struct EventListenerOptions {
    bool capture = false;
    bool once = false;
    bool passive = false;
};

struct RegisteredListener {
    std::string type;
    std::function<void(Event&)> callback;
    EventListenerOptions options;
    uint32_t id = 0;
};

// ==================================================================
// EventTarget — mixin for nodes that receive events
// ==================================================================

class EventTarget {
public:
    EventTarget();
    virtual ~EventTarget();

    uint32_t addEventListener(const std::string& type,
                                std::function<void(Event&)> callback,
                                const EventListenerOptions& options = {});
    void removeEventListener(uint32_t listenerId);
    void removeEventListener(const std::string& type, bool capture = false);

    bool dispatchEvent(Event& event);

    bool hasEventListeners(const std::string& type) const;

protected:
    std::vector<RegisteredListener> listeners_;
    uint32_t nextListenerId_ = 1;
};

// ==================================================================
// EventTargetRegistry — maps BoxNodes to their EventTarget listeners
// ==================================================================

class EventTargetRegistry {
public:
    static EventTargetRegistry& instance() {
        static EventTargetRegistry reg;
        return reg;
    }

    std::unordered_map<BoxNode*, EventTarget*> targets;

    void registerTarget(BoxNode* node, EventTarget* target) {
        targets[node] = target;
    }
    void unregisterTarget(BoxNode* node) {
        targets.erase(node);
    }

private:
    EventTargetRegistry() = default;
};

// ==================================================================
// EventDispatcher — captures + bubbles through the DOM tree
// ==================================================================

class EventDispatcher {
public:
    // Dispatch event with full capture → target → bubble path
    static bool dispatch(Event& event, BoxNode* target);

private:
    // Build the ancestor chain from target to root
    static std::vector<BoxNode*> buildPath(BoxNode* target);

    // Invoke listeners on a single node for a given phase
    static void invokeListeners(BoxNode* node, Event& event, Event::Phase phase);
};

// ==================================================================
// FocusManager — tracks focused element
// ==================================================================

class FocusManager {
public:
    static FocusManager& instance();

    BoxNode* focusedElement() const { return focused_; }
    void setFocus(BoxNode* node);
    void blur();

    // Tab navigation
    void focusNext();
    void focusPrev();

    // Is this node focusable?
    static bool isFocusable(BoxNode* node);

private:
    FocusManager() = default;
    BoxNode* focused_ = nullptr;
    BoxNode* root_ = nullptr;

public:
    void setRoot(BoxNode* root) { root_ = root; }

private:
    std::vector<BoxNode*> buildTabOrder(BoxNode* root);
};

} // namespace Web
} // namespace NXRender
