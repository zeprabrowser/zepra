// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "advanced.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>
#include <chrono>
#include <numeric>

namespace NXRender {
namespace Web {

// ==================================================================
// PointerEvent
// ==================================================================

PointerEvent::PointerEvent(const std::string& type, int pointerId,
                               float clientX, float clientY, int button)
    : MouseEvent(type, clientX, clientY, button), pointerId_(pointerId) {}

// ==================================================================
// TouchList
// ==================================================================

const Touch* TouchList::identifiedTouch(int id) const {
    for (const auto& t : touches_) {
        if (t.identifier == id) return &t;
    }
    return nullptr;
}

// ==================================================================
// TouchEvent
// ==================================================================

TouchEvent::TouchEvent(const std::string& type)
    : Event(type, true, true) {}

// ==================================================================
// PointerCapture
// ==================================================================

PointerCapture& PointerCapture::instance() {
    static PointerCapture inst;
    return inst;
}

void PointerCapture::setCapture(int pointerId, BoxNode* element) {
    pending_.push_back({pointerId, element, false});
}

void PointerCapture::releaseCapture(int pointerId) {
    pending_.push_back({pointerId, nullptr, false});
}

BoxNode* PointerCapture::getCaptureTarget(int pointerId) const {
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) {
        if (it->pointerId == pointerId) return it->element;
    }
    return nullptr;
}

bool PointerCapture::hasCapture(int pointerId) const {
    return getCaptureTarget(pointerId) != nullptr;
}

void PointerCapture::setImplicitCapture(int pointerId, BoxNode* element) {
    pending_.push_back({pointerId, element, true});
}

void PointerCapture::processPending() {
    for (auto& entry : pending_) {
        if (!entry.element) {
            // Release
            active_.erase(
                std::remove_if(active_.begin(), active_.end(),
                    [&](const CaptureEntry& e) { return e.pointerId == entry.pointerId; }),
                active_.end());
        } else {
            // Remove existing capture for this pointer
            active_.erase(
                std::remove_if(active_.begin(), active_.end(),
                    [&](const CaptureEntry& e) { return e.pointerId == entry.pointerId; }),
                active_.end());
            active_.push_back(entry);
        }
    }
    pending_.clear();
}

// ==================================================================
// PointerInputHandler
// ==================================================================

PointerInputHandler::PointerInputHandler() {}

bool PointerInputHandler::dispatchPointer(
    const std::string& type, BoxNode* target,
    int pointerId, float x, float y,
    const std::string& pointerType, int button,
    float pressure, float width, float height,
    int tiltX, int tiltY, int twist, bool isPrimary,
    bool shift, bool ctrl, bool alt, bool meta) {

    if (!target) return false;

    PointerEvent ev(type, pointerId, x, y, button);
    ev.setPointerType(pointerType);
    ev.setPressure(pressure);
    ev.setDimensions(width, height);
    ev.setTilt(tiltX, tiltY);
    ev.setTwist(twist);
    ev.setPrimary(isPrimary);
    ev.setModifiers(alt, ctrl, shift, meta);
    ev.setTrusted(true);

    // Check if pointer is captured
    BoxNode* captureTarget = PointerCapture::instance().getCaptureTarget(pointerId);
    BoxNode* dispatchTarget = captureTarget ? captureTarget : target;

    EventDispatcher::dispatch(ev, dispatchTarget);
    return ev.defaultPrevented();
}

void PointerInputHandler::dispatchCompatMouse(
    const std::string& type, BoxNode* target,
    float x, float y, int button,
    bool shift, bool ctrl, bool alt, bool meta) {

    if (!target) return;

    MouseEvent me(type, x, y, button);
    me.setModifiers(alt, ctrl, shift, meta);
    me.setTrusted(true);
    EventDispatcher::dispatch(me, target);
}

void PointerInputHandler::dispatchTouch(
    const std::string& type, BoxNode* target,
    const TouchList& touches, const TouchList& targetTouches,
    const TouchList& changedTouches,
    bool shift, bool ctrl, bool alt, bool meta) {

    if (!target) return;

    TouchEvent te(type);
    te.touches() = touches;
    te.targetTouches() = targetTouches;
    te.changedTouches() = changedTouches;
    te.setModifiers(alt, ctrl, shift, meta);
    te.setTrusted(true);
    EventDispatcher::dispatch(te, target);
}

void PointerInputHandler::updatePointerOver(
    BoxNode* root, int pointerId, BoxNode* newTarget,
    float x, float y, const std::string& pointerType,
    bool shift, bool ctrl, bool alt, bool meta) {

    auto it = pointers_.find(pointerId);
    BoxNode* oldTarget = (it != pointers_.end()) ? it->second.overTarget : nullptr;

    if (oldTarget == newTarget) return;

    // pointerout on old target
    if (oldTarget) {
        dispatchPointer("pointerout", oldTarget, pointerId, x, y, pointerType, 0,
                          0, 1, 1, 0, 0, 0, true, shift, ctrl, alt, meta);
    }

    // pointerleave on old ancestors not shared with new target
    // (simplified: just dispatch on old target)
    if (oldTarget) {
        dispatchPointer("pointerleave", oldTarget, pointerId, x, y, pointerType, 0,
                          0, 1, 1, 0, 0, 0, true, shift, ctrl, alt, meta);
    }

    // pointerover on new target
    if (newTarget) {
        dispatchPointer("pointerover", newTarget, pointerId, x, y, pointerType, 0,
                          0, 1, 1, 0, 0, 0, true, shift, ctrl, alt, meta);
    }

    // pointerenter on new target
    if (newTarget) {
        dispatchPointer("pointerenter", newTarget, pointerId, x, y, pointerType, 0,
                          0, 1, 1, 0, 0, 0, true, shift, ctrl, alt, meta);
    }

    if (it != pointers_.end()) {
        it->second.overTarget = newTarget;
    }
}

void PointerInputHandler::onPointerDown(
    BoxNode* root, int pointerId, float x, float y,
    const std::string& pointerType, int button,
    float pressure, float width, float height,
    int tiltX, int tiltY, int twist,
    bool shift, bool ctrl, bool alt, bool meta) {

    // Hit test
    HitTestResult hit = hitTester_.hitTest(root, x, y);
    BoxNode* target = hit.node ? hit.node : root;

    // Update pointer state
    PointerState& ps = pointers_[pointerId];
    ps.id = pointerId;
    ps.type = pointerType;
    ps.down = true;
    ps.x = x;
    ps.y = y;
    ps.pressure = pressure;
    ps.target = target;

    bool isPrimary = (pointerType == "mouse") || (pointerId == 1);

    // Update hover
    updatePointerOver(root, pointerId, target, x, y, pointerType,
                        shift, ctrl, alt, meta);

    // Dispatch pointerdown
    bool prevented = dispatchPointer("pointerdown", target, pointerId, x, y,
                                        pointerType, button, pressure, width, height,
                                        tiltX, tiltY, twist, isPrimary,
                                        shift, ctrl, alt, meta);

    // Implicit capture for direct manipulation (touch)
    if (pointerType == "touch") {
        PointerCapture::instance().setImplicitCapture(pointerId, target);
    }

    PointerCapture::instance().processPending();

    // Fire compatibility mouse event unless prevented
    if (!prevented && isPrimary) {
        dispatchCompatMouse("mousedown", target, x, y, button, shift, ctrl, alt, meta);
    }
}

void PointerInputHandler::onPointerMove(
    BoxNode* root, int pointerId, float x, float y,
    const std::string& pointerType,
    float pressure, float width, float height,
    int tiltX, int tiltY, int twist,
    bool shift, bool ctrl, bool alt, bool meta) {

    auto it = pointers_.find(pointerId);
    if (it == pointers_.end()) {
        pointers_[pointerId] = {pointerId, pointerType, false, x, y, pressure, nullptr, nullptr};
        it = pointers_.find(pointerId);
    }

    it->second.x = x;
    it->second.y = y;
    it->second.pressure = pressure;

    // Hit test (or use captured target)
    BoxNode* target;
    BoxNode* captureTarget = PointerCapture::instance().getCaptureTarget(pointerId);
    if (captureTarget) {
        target = captureTarget;
    } else {
        HitTestResult hit = hitTester_.hitTest(root, x, y);
        target = hit.node ? hit.node : root;
    }

    bool isPrimary = (pointerType == "mouse") || (pointerId == 1);

    // Update hover
    updatePointerOver(root, pointerId, target, x, y, pointerType,
                        shift, ctrl, alt, meta);

    // Dispatch pointermove
    bool prevented = dispatchPointer("pointermove", target, pointerId, x, y,
                                        pointerType, 0, pressure, width, height,
                                        tiltX, tiltY, twist, isPrimary,
                                        shift, ctrl, alt, meta);

    if (!prevented && isPrimary) {
        dispatchCompatMouse("mousemove", target, x, y, 0, shift, ctrl, alt, meta);
    }
}

void PointerInputHandler::onPointerUp(
    BoxNode* root, int pointerId, float x, float y,
    const std::string& pointerType, int button,
    bool shift, bool ctrl, bool alt, bool meta) {

    auto it = pointers_.find(pointerId);
    BoxNode* target = root;
    if (it != pointers_.end()) {
        it->second.down = false;
        it->second.pressure = 0;
    }

    BoxNode* captureTarget = PointerCapture::instance().getCaptureTarget(pointerId);
    if (captureTarget) {
        target = captureTarget;
    } else {
        HitTestResult hit = hitTester_.hitTest(root, x, y);
        target = hit.node ? hit.node : root;
    }

    bool isPrimary = (pointerType == "mouse") || (pointerId == 1);

    // Dispatch pointerup
    bool prevented = dispatchPointer("pointerup", target, pointerId, x, y,
                                        pointerType, button, 0, 1, 1,
                                        0, 0, 0, isPrimary,
                                        shift, ctrl, alt, meta);

    // Release implicit capture for touch
    if (pointerType == "touch") {
        PointerCapture::instance().releaseCapture(pointerId);
        PointerCapture::instance().processPending();
    }

    if (!prevented && isPrimary) {
        dispatchCompatMouse("mouseup", target, x, y, button, shift, ctrl, alt, meta);
    }

    // Clean up touch pointer state
    if (pointerType == "touch") {
        pointers_.erase(pointerId);
    }
}

void PointerInputHandler::onPointerCancel(BoxNode* root, int pointerId) {
    auto it = pointers_.find(pointerId);
    BoxNode* target = root;
    if (it != pointers_.end()) target = it->second.target ? it->second.target : root;

    dispatchPointer("pointercancel", target, pointerId, 0, 0, "", 0,
                      0, 1, 1, 0, 0, 0, false, false, false, false, false);

    PointerCapture::instance().releaseCapture(pointerId);
    PointerCapture::instance().processPending();

    pointers_.erase(pointerId);
}

void PointerInputHandler::onTouchStart(
    BoxNode* root, const std::vector<Touch>& touches,
    bool shift, bool ctrl, bool alt, bool meta) {

    for (const auto& touch : touches) {
        int pointerId = nextTouchPointerId_++;
        activeTouches_.add(touch);

        onPointerDown(root, pointerId, touch.clientX, touch.clientY,
                        "touch", 0, touch.force, touch.radiusX * 2, touch.radiusY * 2,
                        0, 0, 0, shift, ctrl, alt, meta);
    }
}

void PointerInputHandler::onTouchMove(
    BoxNode* root, const std::vector<Touch>& changed,
    bool shift, bool ctrl, bool alt, bool meta) {

    for (const auto& touch : changed) {
        // Find existing pointer for this touch id
        for (auto& [id, ps] : pointers_) {
            if (ps.type == "touch") {
                // Map by proximity (simplified)
                onPointerMove(root, id, touch.clientX, touch.clientY,
                                "touch", touch.force, touch.radiusX * 2, touch.radiusY * 2,
                                0, 0, 0, shift, ctrl, alt, meta);
                break;
            }
        }
    }
}

void PointerInputHandler::onTouchEnd(
    BoxNode* root, const std::vector<Touch>& changed,
    bool shift, bool ctrl, bool alt, bool meta) {

    for (const auto& touch : changed) {
        for (auto& [id, ps] : pointers_) {
            if (ps.type == "touch") {
                onPointerUp(root, id, touch.clientX, touch.clientY,
                              "touch", 0, shift, ctrl, alt, meta);
                break;
            }
        }
    }
}

void PointerInputHandler::onTouchCancel(
    BoxNode* root, const std::vector<Touch>& changed) {
    for (auto& [id, ps] : pointers_) {
        if (ps.type == "touch") {
            onPointerCancel(root, id);
        }
    }
}

void PointerInputHandler::flushCoalesced() {
    coalescedBuffer_.clear();
}

const PointerInputHandler::PointerState* PointerInputHandler::getPointer(int id) const {
    auto it = pointers_.find(id);
    return (it != pointers_.end()) ? &it->second : nullptr;
}

// ==================================================================
// DialogManager
// ==================================================================

DialogManager& DialogManager::instance() {
    static DialogManager inst;
    return inst;
}

void DialogManager::showModal(BoxNode* dialog) {
    if (!dialog || isOpen(dialog)) return;

    DialogState state;
    state.dialogBox = dialog;
    state.modal = true;
    dialogs_.push_back(state);

    addToTopLayer(dialog);

    // Mark everything else as inert
    updateInertState();

    // Push close watcher for Escape key
    pushCloseWatcher({dialog,
        [this, dialog]() { /* onCancel */ if (onCancel_) onCancel_(dialog, ""); },
        [this, dialog]() { close(dialog); }
    });
}

void DialogManager::show(BoxNode* dialog) {
    if (!dialog || isOpen(dialog)) return;

    DialogState state;
    state.dialogBox = dialog;
    state.modal = false;
    dialogs_.push_back(state);
}

void DialogManager::close(BoxNode* dialog, const std::string& returnValue) {
    for (auto it = dialogs_.begin(); it != dialogs_.end(); ++it) {
        if (it->dialogBox == dialog) {
            it->returnValue = returnValue;
            if (it->modal) {
                removeFromTopLayer(dialog);
            }
            dialogs_.erase(it);
            break;
        }
    }

    // Remove close watcher
    for (auto it = closeWatchers_.begin(); it != closeWatchers_.end(); ++it) {
        if (it->target == dialog) {
            closeWatchers_.erase(it);
            break;
        }
    }

    updateInertState();

    if (onClose_) onClose_(dialog, returnValue);
}

bool DialogManager::isOpen(BoxNode* dialog) const {
    for (const auto& d : dialogs_) {
        if (d.dialogBox == dialog) return true;
    }
    return false;
}

bool DialogManager::isModal(BoxNode* dialog) const {
    for (const auto& d : dialogs_) {
        if (d.dialogBox == dialog) return d.modal;
    }
    return false;
}

void DialogManager::addToTopLayer(BoxNode* element) {
    removeFromTopLayer(element);
    topLayer_.push_back(element);
}

void DialogManager::removeFromTopLayer(BoxNode* element) {
    topLayer_.erase(
        std::remove(topLayer_.begin(), topLayer_.end(), element),
        topLayer_.end());
}

bool DialogManager::isInTopLayer(BoxNode* element) const {
    return std::find(topLayer_.begin(), topLayer_.end(), element) != topLayer_.end();
}

bool DialogManager::isInertAt(BoxNode* node) const {
    for (const auto& root : inertRoots_) {
        // Check if node is a descendant of an inert root
        BoxNode* parent = const_cast<BoxNode*>(node);
        while (parent) {
            if (parent == root) return true;
            parent = parent->parent();
        }
    }
    return false;
}

void DialogManager::pushCloseWatcher(CloseWatcher watcher) {
    closeWatchers_.push_back(std::move(watcher));
}

void DialogManager::popCloseWatcher() {
    if (!closeWatchers_.empty()) closeWatchers_.pop_back();
}

bool DialogManager::handleEscapeKey() {
    if (closeWatchers_.empty()) return false;
    auto& watcher = closeWatchers_.back();
    if (watcher.onCancel) watcher.onCancel();
    if (watcher.onClose) watcher.onClose();
    return true;
}

void DialogManager::showPopover(BoxNode* popover) {
    if (!popover || isPopoverOpen(popover)) return;
    activePopovers_.push_back(popover);
    addToTopLayer(popover);
}

void DialogManager::hidePopover(BoxNode* popover) {
    activePopovers_.erase(
        std::remove(activePopovers_.begin(), activePopovers_.end(), popover),
        activePopovers_.end());
    removeFromTopLayer(popover);
}

bool DialogManager::togglePopover(BoxNode* popover) {
    if (isPopoverOpen(popover)) {
        hidePopover(popover);
        return false;
    }
    showPopover(popover);
    return true;
}

bool DialogManager::isPopoverOpen(BoxNode* popover) const {
    return std::find(activePopovers_.begin(), activePopovers_.end(), popover) != activePopovers_.end();
}

bool DialogManager::handleLightDismiss(float x, float y) {
    // Light dismiss: close popovers that don't contain the click point
    bool dismissed = false;
    HitTester tester;

    for (int i = static_cast<int>(activePopovers_.size()) - 1; i >= 0; i--) {
        BoxNode* popover = activePopovers_[i];
        if (!HitTester::pointInNode(popover, x, y)) {
            hidePopover(popover);
            dismissed = true;
        } else {
            break; // Stop — clicked inside this popover or one above
        }
    }
    return dismissed;
}

std::unique_ptr<BoxNode> DialogManager::createBackdrop() {
    auto backdrop = std::make_unique<BoxNode>();
    backdrop->setTag("::backdrop");
    backdrop->setBoxType(BoxType::Block);
    ComputedValues cv;
    cv.display = 1;
    cv.position = 3; // fixed
    cv.top = cv.left = 0;
    cv.topAuto = cv.leftAuto = false;
    cv.right = cv.bottom = 0;
    cv.rightAuto = cv.bottomAuto = false;
    cv.backgroundColor = 0x00000066; // semi-transparent black
    cv.zIndex = 9998;
    backdrop->setComputedValues(cv);
    return backdrop;
}

void DialogManager::updateInertState() {
    inertRoots_.clear();
    // Find the topmost modal dialog
    for (auto it = dialogs_.rbegin(); it != dialogs_.rend(); ++it) {
        if (it->modal) {
            // Everything outside this dialog's subtree is inert
            // (simplified: track the dialog box itself as a non-inert exception)
            break;
        }
    }
}

// ==================================================================
// CSS Grid Track Sizing
// ==================================================================

std::vector<GridTrackSizer::TrackSize> GridTrackSizer::parseTrackList(const std::string& input) {
    std::vector<TrackSize> tracks;
    std::istringstream stream(input);
    std::string token;

    while (stream >> token) {
        TrackSize ts;

        // fr unit
        if (token.size() > 2 && token.substr(token.size() - 2) == "fr") {
            ts.type = TrackType::Fr;
            ts.value = std::stof(token.substr(0, token.size() - 2));
        }
        // px unit
        else if (token.size() > 2 && token.substr(token.size() - 2) == "px") {
            ts.type = TrackType::Fixed;
            ts.value = std::stof(token.substr(0, token.size() - 2));
        }
        // percentage
        else if (token.back() == '%') {
            ts.type = TrackType::Percent;
            ts.value = std::stof(token.substr(0, token.size() - 1));
        }
        // auto
        else if (token == "auto") {
            ts.type = TrackType::Auto;
        }
        // min-content
        else if (token == "min-content") {
            ts.type = TrackType::MinContent;
        }
        // max-content
        else if (token == "max-content") {
            ts.type = TrackType::MaxContent;
        }
        // minmax(...)
        else if (token.find("minmax(") == 0) {
            ts.type = TrackType::MinMax;
            // Parse the full minmax expression
            std::string expr = token.substr(7);
            // Collect until closing )
            while (!expr.empty() && expr.back() != ')') {
                std::string more;
                if (stream >> more) expr += " " + more;
                else break;
            }
            if (!expr.empty() && expr.back() == ')') expr.pop_back();

            // Split by comma
            size_t comma = expr.find(',');
            if (comma != std::string::npos) {
                std::string minPart = expr.substr(0, comma);
                std::string maxPart = expr.substr(comma + 1);
                // Trim
                while (!minPart.empty() && std::isspace(minPart.front())) minPart.erase(0, 1);
                while (!maxPart.empty() && std::isspace(maxPart.front())) maxPart.erase(0, 1);
                while (!minPart.empty() && std::isspace(minPart.back())) minPart.pop_back();
                while (!maxPart.empty() && std::isspace(maxPart.back())) maxPart.pop_back();

                // Parse min
                if (minPart == "auto") ts.minType = TrackType::Auto;
                else if (minPart == "min-content") ts.minType = TrackType::MinContent;
                else if (minPart == "max-content") ts.minType = TrackType::MaxContent;
                else {
                    ts.minType = TrackType::Fixed;
                    ts.minValue = std::stof(minPart);
                }

                // Parse max
                if (maxPart == "auto") ts.maxType = TrackType::Auto;
                else if (maxPart.back() == 'r' && maxPart.size() > 2) {
                    ts.maxType = TrackType::Fr;
                    ts.maxValue = std::stof(maxPart.substr(0, maxPart.size() - 2));
                } else if (maxPart == "min-content") ts.maxType = TrackType::MinContent;
                else if (maxPart == "max-content") ts.maxType = TrackType::MaxContent;
                else {
                    ts.maxType = TrackType::Fixed;
                    ts.maxValue = std::stof(maxPart);
                }
            }
        }
        // repeat(...)
        else if (token.find("repeat(") == 0) {
            ts.type = TrackType::Repeat;
            std::string expr = token.substr(7);
            while (!expr.empty() && expr.back() != ')') {
                std::string more;
                if (stream >> more) expr += " " + more;
                else break;
            }
            if (!expr.empty() && expr.back() == ')') expr.pop_back();

            size_t comma = expr.find(',');
            if (comma != std::string::npos) {
                std::string countStr = expr.substr(0, comma);
                std::string pattern = expr.substr(comma + 1);
                while (!countStr.empty() && std::isspace(countStr.front())) countStr.erase(0, 1);
                while (!pattern.empty() && std::isspace(pattern.front())) pattern.erase(0, 1);

                if (countStr == "auto-fill") ts.repeatCount = 0;
                else if (countStr == "auto-fit") ts.repeatCount = -1;
                else ts.repeatCount = std::stoi(countStr);

                ts.repeatPattern = parseTrackList(pattern);
            }

            // Expand fixed repeat counts inline
            if (ts.repeatCount > 0) {
                for (int r = 0; r < ts.repeatCount; r++) {
                    for (const auto& p : ts.repeatPattern) {
                        tracks.push_back(p);
                    }
                }
                continue; // Don't add the repeat() itself
            }
        }
        // Numeric fallback (assume px)
        else {
            try {
                ts.type = TrackType::Fixed;
                ts.value = std::stof(token);
            } catch (...) {
                continue;
            }
        }

        tracks.push_back(ts);
    }

    return tracks;
}

std::vector<GridTrackSizer::TrackSize> GridTrackSizer::expandRepeats(
    const std::vector<TrackSize>& tracks, float availableSize) {

    std::vector<TrackSize> result;
    for (const auto& ts : tracks) {
        if (ts.type == TrackType::Repeat && (ts.repeatCount == 0 || ts.repeatCount == -1)) {
            // auto-fill / auto-fit
            if (ts.repeatPattern.empty()) continue;

            // Calculate fixed track size in pattern
            float patternSize = 0;
            for (const auto& p : ts.repeatPattern) {
                if (p.type == TrackType::Fixed) patternSize += p.value;
                else patternSize += 100; // Fallback estimate for intrinsic
            }

            int count = (patternSize > 0) ? static_cast<int>(availableSize / patternSize) : 1;
            if (count < 1) count = 1;

            for (int r = 0; r < count; r++) {
                for (const auto& p : ts.repeatPattern) {
                    result.push_back(p);
                }
            }
        } else {
            result.push_back(ts);
        }
    }
    return result;
}

void GridTrackSizer::initializeTrackSizes(std::vector<Track>& tracks, float availableSize) {
    for (auto& track : tracks) {
        const auto& ts = track.sizing;
        switch (ts.type) {
            case TrackType::Fixed:
                track.baseSize = ts.value;
                track.growthLimit = ts.value;
                break;
            case TrackType::Percent:
                track.baseSize = ts.value * availableSize / 100.0f;
                track.growthLimit = track.baseSize;
                break;
            case TrackType::Fr:
                track.baseSize = 0;
                track.growthLimit = -1; // infinity
                break;
            case TrackType::Auto:
            case TrackType::MinContent:
            case TrackType::MaxContent:
            case TrackType::FitContent:
                track.baseSize = 0;
                track.growthLimit = -1;
                break;
            case TrackType::MinMax:
                // Base size from min function
                track.baseSize = resolveFixed(TrackSize{ts.minType, ts.minValue}, availableSize);
                // Growth limit from max function
                if (ts.maxType == TrackType::Fixed || ts.maxType == TrackType::Percent) {
                    track.growthLimit = resolveFixed(TrackSize{ts.maxType, ts.maxValue}, availableSize);
                } else {
                    track.growthLimit = -1;
                }
                break;
            default:
                track.baseSize = 0;
                track.growthLimit = -1;
                break;
        }

        // Growth limit must be >= base size
        if (track.growthLimit >= 0 && track.growthLimit < track.baseSize) {
            track.growthLimit = track.baseSize;
        }
    }
}

void GridTrackSizer::resolveIntrinsicSizes(
    std::vector<Track>& tracks, const std::vector<GridItem>& items, bool isColumn) {

    for (const auto& item : items) {
        int start = isColumn ? item.area.colStart : item.area.rowStart;
        int end = isColumn ? item.area.colEnd : item.area.rowEnd;
        if (start < 0 || end < 0) continue;

        float minC = minContribution(item, isColumn);
        float maxC = maxContribution(item, isColumn);

        // Items spanning a single track
        if (end - start == 1 && start < static_cast<int>(tracks.size())) {
            auto& track = tracks[start];
            if (track.sizing.isIntrinsic()) {
                track.baseSize = std::max(track.baseSize, minC);
                if (track.growthLimit < 0) {
                    track.growthLimit = maxC;
                } else {
                    track.growthLimit = std::max(track.growthLimit, maxC);
                }
            }
        }
        // Multi-track spanning items
        else if (end - start > 1) {
            // Distribute across spanned intrinsic tracks
            std::vector<int> intrinsicIndices;
            float existingSize = 0;
            for (int i = start; i < end && i < static_cast<int>(tracks.size()); i++) {
                existingSize += tracks[i].baseSize;
                if (tracks[i].sizing.isIntrinsic()) {
                    intrinsicIndices.push_back(i);
                }
            }

            float extraNeeded = minC - existingSize;
            if (extraNeeded > 0 && !intrinsicIndices.empty()) {
                distributeExtraSpace(tracks, intrinsicIndices, extraNeeded, true);
            }
        }
    }

    // Ensure growth limit >= base size
    for (auto& track : tracks) {
        if (track.growthLimit >= 0 && track.growthLimit < track.baseSize) {
            track.growthLimit = track.baseSize;
        }
    }
}

void GridTrackSizer::maximizeTracks(std::vector<Track>& tracks, float freeSpace) {
    if (freeSpace <= 0) return;

    // Distribute free space equally to tracks with infinite growth limits
    std::vector<int> growable;
    for (int i = 0; i < static_cast<int>(tracks.size()); i++) {
        if (tracks[i].growthLimit < 0 || tracks[i].baseSize < tracks[i].growthLimit) {
            growable.push_back(i);
        }
    }

    if (growable.empty()) return;

    float perTrack = freeSpace / static_cast<float>(growable.size());
    for (int idx : growable) {
        auto& track = tracks[idx];
        float maxGrowth = (track.growthLimit >= 0) ? track.growthLimit - track.baseSize : perTrack;
        float growth = std::min(perTrack, maxGrowth);
        if (growth > 0) {
            track.baseSize += growth;
        }
    }
}

void GridTrackSizer::expandFlexibleTracks(
    std::vector<Track>& tracks, const std::vector<GridItem>& items, float availableSize) {

    // Calculate total fr and fixed space
    float totalFr = 0;
    float fixedSpace = 0;

    for (const auto& track : tracks) {
        if (track.sizing.isFlexible()) {
            totalFr += track.sizing.value;
        } else {
            fixedSpace += track.baseSize;
        }
    }

    // Add gaps
    float totalGaps = (tracks.size() > 1) ? (tracks.size() - 1) * columnGap : 0;
    fixedSpace += totalGaps;

    float flexSpace = availableSize - fixedSpace;
    if (flexSpace <= 0 || totalFr <= 0) return;

    float frSize = flexSpace / totalFr;

    // Hypothetical fr size — clamp to track min contribution
    for (auto& track : tracks) {
        if (track.sizing.isFlexible()) {
            float hypothetical = frSize * track.sizing.value;
            track.baseSize = std::max(track.baseSize, hypothetical);
        }
    }
}

void GridTrackSizer::distributeExtraSpace(
    std::vector<Track>& tracks, const std::vector<int>& trackIndices,
    float extraSpace, bool toBase) {

    if (trackIndices.empty() || extraSpace <= 0) return;

    float perTrack = extraSpace / static_cast<float>(trackIndices.size());
    for (int idx : trackIndices) {
        if (toBase) {
            tracks[idx].baseSize += perTrack;
        } else {
            if (tracks[idx].growthLimit < 0) {
                tracks[idx].growthLimit = tracks[idx].baseSize + perTrack;
            } else {
                tracks[idx].growthLimit += perTrack;
            }
        }
    }
}

float GridTrackSizer::minContribution(const GridItem& item, bool isColumn) const {
    return isColumn ? item.minContentWidth : item.minContentHeight;
}

float GridTrackSizer::maxContribution(const GridItem& item, bool isColumn) const {
    return isColumn ? item.maxContentWidth : item.maxContentHeight;
}

float GridTrackSizer::resolveFixed(const TrackSize& ts, float availableSize) const {
    switch (ts.type) {
        case TrackType::Fixed: return ts.value;
        case TrackType::Percent: return ts.value * availableSize / 100.0f;
        default: return 0;
    }
}

void GridTrackSizer::sizeColumnTracks(
    std::vector<Track>& tracks, const std::vector<GridItem>& items, float availableWidth) {

    initializeTrackSizes(tracks, availableWidth);
    resolveIntrinsicSizes(tracks, items, true);

    float usedSpace = 0;
    for (const auto& t : tracks) usedSpace += t.baseSize;
    float totalGaps = (tracks.size() > 1) ? (tracks.size() - 1) * columnGap : 0;
    float freeSpace = availableWidth - usedSpace - totalGaps;

    if (freeSpace > 0) {
        // Check if any tracks are flexible
        bool hasFlex = false;
        for (const auto& t : tracks) {
            if (t.sizing.isFlexible()) { hasFlex = true; break; }
        }

        if (hasFlex) {
            expandFlexibleTracks(tracks, items, availableWidth);
        } else {
            maximizeTracks(tracks, freeSpace);
        }
    }

    // Assign positions
    float offset = 0;
    for (auto& track : tracks) {
        track.offset = offset;
        track.size = track.baseSize;
        offset += track.size + columnGap;
    }
}

void GridTrackSizer::sizeRowTracks(
    std::vector<Track>& tracks, const std::vector<GridItem>& items,
    float availableHeight, const std::vector<Track>& columnTracks) {

    initializeTrackSizes(tracks, availableHeight);
    resolveIntrinsicSizes(tracks, items, false);

    float usedSpace = 0;
    for (const auto& t : tracks) usedSpace += t.baseSize;
    float totalGaps = (tracks.size() > 1) ? (tracks.size() - 1) * rowGap : 0;
    float freeSpace = availableHeight - usedSpace - totalGaps;

    if (freeSpace > 0) {
        bool hasFlex = false;
        for (const auto& t : tracks) {
            if (t.sizing.isFlexible()) { hasFlex = true; break; }
        }
        if (hasFlex) {
            expandFlexibleTracks(tracks, items, availableHeight);
        } else {
            maximizeTracks(tracks, freeSpace);
        }
    }

    float offset = 0;
    for (auto& track : tracks) {
        track.offset = offset;
        track.size = track.baseSize;
        offset += track.size + rowGap;
    }
}

float GridTrackSizer::totalTrackSize(const std::vector<Track>& tracks, float gap) {
    if (tracks.empty()) return 0;
    float total = 0;
    for (const auto& t : tracks) total += t.size;
    total += gap * (tracks.size() - 1);
    return total;
}

// ==================================================================
// Grid Placement
// ==================================================================

void GridPlacement::OccupancyGrid::resize(int r, int c) {
    rows = r; cols = c;
    cells.resize(r);
    for (auto& row : cells) row.resize(c, false);
}

bool GridPlacement::OccupancyGrid::isAvailable(int r, int c, int rowSpan, int colSpan) const {
    for (int ri = r; ri < r + rowSpan && ri < rows; ri++) {
        for (int ci = c; ci < c + colSpan && ci < cols; ci++) {
            if (cells[ri][ci]) return false;
        }
    }
    return true;
}

void GridPlacement::OccupancyGrid::occupy(int r, int c, int rowSpan, int colSpan) {
    for (int ri = r; ri < r + rowSpan && ri < rows; ri++) {
        for (int ci = c; ci < c + colSpan && ci < cols; ci++) {
            cells[ri][ci] = true;
        }
    }
}

std::pair<int,int> GridPlacement::OccupancyGrid::findSlot(
    int rowSpan, int colSpan, int startRow, int startCol, bool dense) const {

    int sr = dense ? 0 : startRow;
    int sc = dense ? 0 : startCol;

    for (int r = sr; r <= rows - rowSpan; r++) {
        for (int c = (r == sr) ? sc : 0; c <= cols - colSpan; c++) {
            if (isAvailable(r, c, rowSpan, colSpan)) {
                return {r, c};
            }
        }
    }
    // Expand grid
    return {rows, 0};
}

void GridPlacement::parsePlacement(const std::string& value, int& start, int& end, int trackCount) {
    if (value.empty() || value == "auto") {
        start = -1; end = -1;
        return;
    }

    // "span N" → span
    if (value.find("span") == 0) {
        start = -1;
        std::string num = value.substr(4);
        while (!num.empty() && std::isspace(num.front())) num.erase(0, 1);
        end = num.empty() ? 1 : std::stoi(num);
        return;
    }

    // "N / M" → explicit start/end
    size_t slash = value.find('/');
    if (slash != std::string::npos) {
        std::string startStr = value.substr(0, slash);
        std::string endStr = value.substr(slash + 1);
        while (!startStr.empty() && std::isspace(startStr.back())) startStr.pop_back();
        while (!endStr.empty() && std::isspace(endStr.front())) endStr.erase(0, 1);

        start = std::stoi(startStr) - 1; // Convert 1-indexed to 0-indexed
        if (endStr.find("span") == 0) {
            std::string spanNum = endStr.substr(4);
            while (!spanNum.empty() && std::isspace(spanNum.front())) spanNum.erase(0, 1);
            int span = spanNum.empty() ? 1 : std::stoi(spanNum);
            end = start + span;
        } else {
            end = std::stoi(endStr) - 1;
        }
        return;
    }

    // Single number
    start = std::stoi(value) - 1;
    end = start + 1;
}

void GridPlacement::resolveGridPosition(
    const std::string& gridRow, const std::string& gridCol,
    int rowCount, int colCount,
    int& rowStart, int& rowEnd, int& colStart, int& colEnd) {

    parsePlacement(gridRow, rowStart, rowEnd, rowCount);
    parsePlacement(gridCol, colStart, colEnd, colCount);
}

std::vector<GridPlacement::PlacedItem> GridPlacement::place(
    const std::vector<BoxNode*>& items,
    int explicitRowCount, int explicitColCount,
    bool denseFlow) {

    std::vector<PlacedItem> result;

    int maxRows = std::max(explicitRowCount, 1);
    int maxCols = std::max(explicitColCount, 1);

    // Expand grid to accommodate items
    for (const auto* item : items) {
        // Check if item has explicit placement
        maxRows = std::max(maxRows, 10); // reasonable default
        maxCols = std::max(maxCols, 10);
    }

    OccupancyGrid grid;
    grid.resize(maxRows, maxCols);

    int cursorRow = 0, cursorCol = 0;

    for (auto* item : items) {
        int rowSpan = 1, colSpan = 1;
        int rowStart = -1, colStart = -1;

        // Check computed values for grid placement
        // (In real implementation, this would parse grid-row/grid-column from ComputedValues)

        PlacedItem placed;
        placed.node = item;

        if (rowStart >= 0 && colStart >= 0) {
            // Explicitly placed
            placed.rowStart = rowStart;
            placed.rowEnd = rowStart + rowSpan;
            placed.colStart = colStart;
            placed.colEnd = colStart + colSpan;
        } else {
            // Auto-placed
            auto [r, c] = grid.findSlot(rowSpan, colSpan, cursorRow, cursorCol, denseFlow);

            // Expand grid if needed
            while (r + rowSpan > grid.rows) {
                grid.rows++;
                grid.cells.emplace_back(grid.cols, false);
            }

            placed.rowStart = r;
            placed.rowEnd = r + rowSpan;
            placed.colStart = c;
            placed.colEnd = c + colSpan;

            cursorRow = r;
            cursorCol = c + colSpan;
            if (cursorCol >= grid.cols) {
                cursorCol = 0;
                cursorRow++;
            }
        }

        grid.occupy(placed.rowStart, placed.colStart,
                      placed.rowEnd - placed.rowStart, placed.colEnd - placed.colStart);

        result.push_back(placed);
    }

    return result;
}

} // namespace Web
} // namespace NXRender
