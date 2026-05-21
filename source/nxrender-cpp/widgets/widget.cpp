// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file widget.cpp
 * @brief Base Widget implementation
 */

#include "widgets/widget.h"
#include <algorithm>
#include "nxgfx/context.h"

namespace NXRender {

WidgetId Widget::nextId_ = 1;

Widget::Widget() : id_(nextId_++) {}

Widget::~Widget() = default;

void Widget::setBounds(const Rect& bounds) {
    bounds_ = bounds;
}

void Widget::setPosition(float x, float y) {
    bounds_.x = x;
    bounds_.y = y;
}

void Widget::setSize(float width, float height) {
    bounds_.width = width;
    bounds_.height = height;
}

void Widget::setVisible(bool visible) {
    state_.visible = visible;
}

void Widget::setEnabled(bool enabled) {
    state_.enabled = enabled;
}

void Widget::setFocused(bool focused) {
    state_.focused = focused;
}

void Widget::render(GpuContext* ctx) {
    if (!state_.visible) return;
    
    // Draw background if set
    if (backgroundColor_.a > 0) {
        ctx->fillRect(bounds_, backgroundColor_);
    }
    
    // Render children
    renderChildren(ctx);
}

void Widget::renderChildren(GpuContext* ctx) {
    for (auto& child : children_) {
        if (child->isVisible()) {
            child->render(ctx);
        }
    }
}

Size Widget::measure(const Size& available) {
    // Default: use current bounds size
    return Size(bounds_.width, bounds_.height);
}

void Widget::layout() {
    // Default: do nothing
}

EventResult Widget::handleEvent(const Event& event) {
    if (!state_.enabled) return EventResult::Ignored;
    
    switch (event.type) {
        case EventType::MouseDown:
            return onMouseDown(event.mouse.x, event.mouse.y, event.mouse.button);
        case EventType::MouseUp:
            return onMouseUp(event.mouse.x, event.mouse.y, event.mouse.button);
        case EventType::MouseMove:
            return onMouseMove(event.mouse.x, event.mouse.y);
        case EventType::MouseEnter:
            return onMouseEnter();
        case EventType::MouseLeave:
            return onMouseLeave();
        case EventType::KeyDown:
            return onKeyDown(event.key.key, event.key.modifiers);
        case EventType::KeyUp:
            return onKeyUp(event.key.key, event.key.modifiers);
        case EventType::TextInput:
            return onTextInput(event.textInput);
        case EventType::Focus:
            return onFocus();
        case EventType::Blur:
            return onBlur();
        default:
            return EventResult::Ignored;
    }
}

EventResult Widget::handleRoutedEvent(const Input::Event& event) {
    if (!state_.enabled) return EventResult::Ignored;
    
    // Map the new input::Event to the legacy virtual dispatch for compatibility
    if (auto mouseEv = dynamic_cast<const Input::MouseEvent*>(&event)) {
        switch (mouseEv->type()) {
            case Input::EventType::MouseDown: return onMouseDown(mouseEv->x(), mouseEv->y(), static_cast<MouseButton>(mouseEv->button() + 1));
            case Input::EventType::MouseUp: return onMouseUp(mouseEv->x(), mouseEv->y(), static_cast<MouseButton>(mouseEv->button() + 1));
            case Input::EventType::MouseMove: return onMouseMove(mouseEv->x(), mouseEv->y());
            case Input::EventType::MouseEnter: return onMouseEnter();
            case Input::EventType::MouseLeave: return onMouseLeave();
            default: break;
        }
    } else if (auto keyEv = dynamic_cast<const Input::KeyEvent*>(&event)) {
        Modifiers legacyMods;
        legacyMods.shift = keyEv->modifiers().shift;
        legacyMods.ctrl = keyEv->modifiers().ctrl;
        legacyMods.alt = keyEv->modifiers().alt;
        legacyMods.meta = keyEv->modifiers().meta;

        switch (keyEv->type()) {
            case Input::EventType::KeyDown: return onKeyDown(static_cast<KeyCode>(keyEv->keyCode()), legacyMods);
            case Input::EventType::KeyUp: return onKeyUp(static_cast<KeyCode>(keyEv->keyCode()), legacyMods);
            default: break;
        }
    } else if (event.type() == Input::EventType::FocusIn) {
        return onFocus();
    } else if (event.type() == Input::EventType::FocusOut) {
        return onBlur();
    }
    
    return EventResult::Ignored;
}

EventResult Widget::onMouseDown(float x, float y, MouseButton button) {
    (void)x; (void)y; (void)button;
    return EventResult::Ignored;
}

EventResult Widget::onMouseUp(float x, float y, MouseButton button) {
    (void)x; (void)y; (void)button;
    return EventResult::Ignored;
}

EventResult Widget::onMouseMove(float x, float y) {
    (void)x; (void)y;
    return EventResult::Ignored;
}

EventResult Widget::onMouseEnter() {
    state_.hovered = true;
    return EventResult::NeedsRedraw;
}

EventResult Widget::onMouseLeave() {
    state_.hovered = false;
    return EventResult::NeedsRedraw;
}

EventResult Widget::onKeyDown(KeyCode key, Modifiers mods) {
    (void)key; (void)mods;
    return EventResult::Ignored;
}

EventResult Widget::onKeyUp(KeyCode key, Modifiers mods) {
    (void)key; (void)mods;
    return EventResult::Ignored;
}

EventResult Widget::onTextInput(const std::string& text) {
    (void)text;
    return EventResult::Ignored;
}

EventResult Widget::onFocus() {
    state_.focused = true;
    return EventResult::NeedsRedraw;
}

EventResult Widget::onBlur() {
    state_.focused = false;
    return EventResult::NeedsRedraw;
}

void Widget::addChild(std::unique_ptr<Widget> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
}

void Widget::removeChild(Widget* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
        [child](const auto& ptr) { return ptr.get() == child; });
    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void Widget::clearChildren() {
    for (auto& child : children_) {
        child->parent_ = nullptr;
    }
    children_.clear();
}

bool Widget::hitTest(float x, float y) const {
    if (!state_.visible || !bounds_.contains(x, y)) {
        return false;
    }
    return true;
}

Input::EventTarget* Widget::hitTestDeep(float x, float y) {
    if (!hitTest(x, y)) return nullptr;
    
    // Check children in reverse order (top to bottom)
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->hitTest(x, y)) {
            if (auto deep = (*it)->hitTestDeep(x, y)) {
                return deep;
            }
            return it->get();
        }
    }
    
    return this;
}

void Widget::invalidate() {
    // Would notify compositor of damage
}

} // namespace NXRender
