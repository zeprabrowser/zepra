// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "input.h"
#include "events.h"
#include "web/box/box_tree.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace NXRender {
namespace Web {

// ==================================================================
// HitTester
// ==================================================================

HitTester::HitTester() {}

HitTestResult HitTester::hitTest(BoxNode* root, float x, float y,
                                    float scrollX, float scrollY) {
    HitTestResult result;
    if (!root) return result;

    // Adjust for viewport scroll
    float testX = x + scrollX;
    float testY = y + scrollY;

    hitTestNode(root, testX, testY, result);

    if (result.node) {
        result.scrollX = scrollX;
        result.scrollY = scrollY;
    }

    return result;
}

std::vector<HitTestResult> HitTester::hitTestAll(BoxNode* root, float x, float y) {
    std::vector<HitTestResult> results;

    std::function<void(BoxNode*)> collect = [&](BoxNode* node) {
        if (!node) return;
        if (node->boxType() == BoxType::None) return;
        if (!acceptsPointerEvents(node)) return;

        if (pointInNode(node, x, y)) {
            HitTestResult hr;
            hr.node = node;
            auto local = toLocalCoords(node, x, y);
            hr.localX = local.x;
            hr.localY = local.y;
            results.push_back(hr);
        }

        for (auto& child : node->children()) {
            collect(child.get());
        }
    };

    collect(root);
    return results;
}

bool HitTester::hitTestNode(BoxNode* node, float x, float y, HitTestResult& result) {
    if (!node) return false;
    if (node->boxType() == BoxType::None) return false;
    if (!acceptsPointerEvents(node)) return false;

    auto& lb = node->layoutBox();

    // Check if point is inside the node's border box
    bool inside = (x >= lb.x && x < lb.x + lb.width &&
                   y >= lb.y && y < lb.y + lb.height);

    if (!inside) return false;

    // Add to path
    result.path.push_back(node);

    // Account for scroll offset on this node
    auto* scrollMgr = &ScrollManager::instance();
    auto* scrollState = scrollMgr->getScrollState(node);
    float adjustedX = x;
    float adjustedY = y;
    if (scrollState) {
        adjustedX += scrollState->scrollLeft;
        adjustedY += scrollState->scrollTop;
    }

    // Test children in reverse order (front-to-back painting → back-to-front hit test)
    // Positioned children with higher z-index are tested first
    bool childHit = false;

    // Collect children with z-index for stacking context ordering
    struct ZChild {
        BoxNode* node;
        int zIndex;
    };
    std::vector<ZChild> positiveZ, zeroZ, negativeZ;

    for (auto& child : node->children()) {
        int z = child->computed().zIndex;
        bool positioned = child->isPositioned();
        if (positioned && z > 0) positiveZ.push_back({child.get(), z});
        else if (positioned && z < 0) negativeZ.push_back({child.get(), z});
        else zeroZ.push_back({child.get(), z});
    }

    // Sort: highest z-index first
    std::sort(positiveZ.begin(), positiveZ.end(),
              [](const ZChild& a, const ZChild& b) { return a.zIndex > b.zIndex; });
    std::sort(negativeZ.begin(), negativeZ.end(),
              [](const ZChild& a, const ZChild& b) { return a.zIndex > b.zIndex; });

    // Test positive z-index first
    for (auto& zc : positiveZ) {
        if (hitTestNode(zc.node, adjustedX, adjustedY, result)) {
            childHit = true;
            break;
        }
    }

    // Then z-index: 0 / auto (in reverse paint order)
    if (!childHit) {
        for (int i = static_cast<int>(zeroZ.size()) - 1; i >= 0; i--) {
            if (hitTestNode(zeroZ[i].node, adjustedX, adjustedY, result)) {
                childHit = true;
                break;
            }
        }
    }

    // Then negative z-index
    if (!childHit) {
        for (auto& zc : negativeZ) {
            if (hitTestNode(zc.node, adjustedX, adjustedY, result)) {
                childHit = true;
                break;
            }
        }
    }

    if (!childHit) {
        // This node is the hit target
        result.node = node;
        auto local = toLocalCoords(node, x, y);
        result.localX = local.x;
        result.localY = local.y;

        // Check content vs scrollbar area
        float contentRight = lb.x + lb.width - lb.borderRight;
        float contentBottom = lb.y + lb.height - lb.borderBottom;
        float contentLeft = lb.x + lb.borderLeft;
        float contentTop = lb.y + lb.borderTop;

        // Scrollbar width: browser default ~15px
        constexpr float scrollbarWidth = 15.0f;
        if (scrollState) {
            if (scrollState->canScrollVertically() &&
                x >= contentRight - scrollbarWidth && x < contentRight) {
                result.onVerticalScrollbar = true;
            }
            if (scrollState->canScrollHorizontally() &&
                y >= contentBottom - scrollbarWidth && y < contentBottom) {
                result.onHorizontalScrollbar = true;
            }
        }

        result.insideContent = (x >= contentLeft && x < contentRight &&
                                  y >= contentTop && y < contentBottom &&
                                  !result.onVerticalScrollbar && !result.onHorizontalScrollbar);

        // Text offset
        if (node->isTextNode()) {
            result.textOffset = textOffsetAtX(node, result.localX);
        }
    }

    return true;
}

bool HitTester::pointInNode(BoxNode* node, float x, float y) {
    auto& lb = node->layoutBox();
    return (x >= lb.x && x < lb.x + lb.width &&
            y >= lb.y && y < lb.y + lb.height);
}

bool HitTester::acceptsPointerEvents(BoxNode* node) {
    auto& cv = node->computed();
    if (cv.display == 0) return false;   // display:none
    if (cv.visibility == 1) return false; // hidden
    if (cv.pointerEvents == "none") return false;
    return true;
}

int HitTester::textOffsetAtX(BoxNode* textNode, float localX) {
    if (!textNode->isTextNode()) return -1;

    float fontSize = textNode->computed().fontSize;
    if (fontSize <= 0) fontSize = 16.0f;
    float charWidth = fontSize * 0.55f;

    int offset = static_cast<int>(localX / charWidth);
    int textLen = static_cast<int>(textNode->text().size());
    return std::clamp(offset, 0, textLen);
}

Point HitTester::toLocalCoords(BoxNode* node, float x, float y) {
    auto& lb = node->layoutBox();
    return Point(x - lb.x - lb.borderLeft - lb.paddingLeft,
                   y - lb.y - lb.borderTop - lb.paddingTop);
}

// ==================================================================
// ScrollManager
// ==================================================================

ScrollManager& ScrollManager::instance() {
    static ScrollManager inst;
    return inst;
}

void ScrollManager::registerScrollContainer(BoxNode* node) {
    if (!node) return;
    for (auto* c : containers_) {
        if (c == node) return; // Already registered
    }
    containers_.push_back(node);

    ScrollContainer sc;
    sc.node = node;
    scrollData_.push_back(sc);
}

void ScrollManager::unregisterScrollContainer(BoxNode* node) {
    containers_.erase(std::remove(containers_.begin(), containers_.end(), node), containers_.end());
    scrollData_.erase(
        std::remove_if(scrollData_.begin(), scrollData_.end(),
                        [node](const ScrollContainer& sc) { return sc.node == node; }),
        scrollData_.end());
}

ScrollState* ScrollManager::getScrollState(BoxNode* node) {
    auto* sc = findContainer(node);
    return sc ? &sc->state : nullptr;
}

void ScrollManager::setScrollState(BoxNode* node, const ScrollState& state) {
    auto* sc = findContainer(node);
    if (sc) {
        sc->state = state;
        sc->state.clamp();
    }
}

void ScrollManager::scrollTo(BoxNode* node, float x, float y, bool smooth) {
    auto* sc = findContainer(node);
    if (!sc) return;

    if (smooth) {
        sc->animating = true;
        sc->startX = sc->state.scrollLeft;
        sc->startY = sc->state.scrollTop;
        sc->targetX = std::clamp(x, 0.0f, sc->state.maxScrollLeft());
        sc->targetY = std::clamp(y, 0.0f, sc->state.maxScrollTop());
        sc->animStart = -1; // Will be set on next tick
    } else {
        sc->state.scrollLeft = x;
        sc->state.scrollTop = y;
        sc->state.clamp();
        sc->animating = false;
    }
}

void ScrollManager::scrollBy(BoxNode* node, float dx, float dy, bool smooth) {
    auto* sc = findContainer(node);
    if (!sc) return;
    scrollTo(node, sc->state.scrollLeft + dx, sc->state.scrollTop + dy, smooth);
}

void ScrollManager::scrollIntoView(BoxNode* target, bool alignTop) {
    if (!target) return;
    BoxNode* container = findScrollContainer(target);
    if (!container) return;

    auto* sc = findContainer(container);
    if (!sc) return;

    auto& targetLB = target->layoutBox();
    auto& containerLB = container->layoutBox();

    float relativeTop = targetLB.y - containerLB.y + sc->state.scrollTop;
    float relativeBottom = relativeTop + targetLB.height;

    if (alignTop) {
        scrollTo(container, sc->state.scrollLeft, relativeTop, true);
    } else {
        // Scroll minimum to make visible
        if (relativeTop < sc->state.scrollTop) {
            scrollTo(container, sc->state.scrollLeft, relativeTop, true);
        } else if (relativeBottom > sc->state.scrollTop + sc->state.clientHeight) {
            scrollTo(container, sc->state.scrollLeft,
                       relativeBottom - sc->state.clientHeight, true);
        }
    }
}

BoxNode* ScrollManager::findScrollContainer(BoxNode* node) {
    BoxNode* current = node ? node->parent() : nullptr;
    while (current) {
        if (isScrollContainer(current)) return current;
        current = current->parent();
    }
    return nullptr;
}

bool ScrollManager::handleWheel(BoxNode* target, float deltaX, float deltaY) {
    BoxNode* container = target;
    while (container) {
        auto* sc = findContainer(container);
        if (sc) {
            bool consumed = false;
            if (std::abs(deltaY) > 0 && sc->state.canScrollVertically()) {
                sc->state.scrollTop += deltaY;
                sc->state.clamp();
                consumed = true;
            }
            if (std::abs(deltaX) > 0 && sc->state.canScrollHorizontally()) {
                sc->state.scrollLeft += deltaX;
                sc->state.clamp();
                consumed = true;
            }
            if (consumed) return true;
        }
        container = container->parent();
    }
    return false;
}

void ScrollManager::tick(double timestamp) {
    for (auto& sc : scrollData_) {
        if (!sc.animating) continue;

        if (sc.animStart < 0) sc.animStart = timestamp;

        double elapsed = timestamp - sc.animStart;
        float t = static_cast<float>(std::min(elapsed / sc.animDuration, 1.0));
        float easedT = easeOutCubic(t);

        sc.state.scrollLeft = sc.startX + (sc.targetX - sc.startX) * easedT;
        sc.state.scrollTop = sc.startY + (sc.targetY - sc.startY) * easedT;
        sc.state.clamp();

        if (t >= 1.0f) {
            sc.animating = false;
        }
    }
}

ScrollManager::ScrollContainer* ScrollManager::findContainer(BoxNode* node) {
    for (auto& sc : scrollData_) {
        if (sc.node == node) return &sc;
    }
    return nullptr;
}

bool ScrollManager::isScrollContainer(BoxNode* node) {
    if (!node) return false;
    auto& cv = node->computed();
    // overflow: auto (3), scroll (2), hidden (1) create scroll containers
    return cv.overflowX >= 1 || cv.overflowY >= 1;
}

float ScrollManager::easeOutCubic(float t) {
    float f = t - 1.0f;
    return f * f * f + 1.0f;
}

// ==================================================================
// Selection
// ==================================================================

Selection& Selection::instance() {
    static Selection inst;
    return inst;
}

void Selection::setAnchor(BoxNode* node, int offset) {
    anchor_.node = node;
    anchor_.offset = offset;
}

void Selection::setFocus(BoxNode* node, int offset) {
    focus_.node = node;
    focus_.offset = offset;
}

void Selection::collapse(BoxNode* node, int offset) {
    anchor_.node = focus_.node = node;
    anchor_.offset = focus_.offset = offset;
    caretVisible_ = true;
}

void Selection::clear() {
    anchor_ = {};
    focus_ = {};
}

void Selection::selectAll(BoxNode* root) {
    if (!root) return;

    // Find first text node
    std::function<BoxNode*(BoxNode*)> firstText = [&](BoxNode* n) -> BoxNode* {
        if (n->isTextNode()) return n;
        for (auto& child : n->children()) {
            auto* found = firstText(child.get());
            if (found) return found;
        }
        return nullptr;
    };

    // Find last text node
    std::function<BoxNode*(BoxNode*)> lastText = [&](BoxNode* n) -> BoxNode* {
        for (int i = static_cast<int>(n->children().size()) - 1; i >= 0; i--) {
            auto* found = lastText(n->children()[i].get());
            if (found) return found;
        }
        if (n->isTextNode()) return n;
        return nullptr;
    };

    BoxNode* first = firstText(root);
    BoxNode* last = lastText(root);
    if (first && last) {
        setAnchor(first, 0);
        setFocus(last, static_cast<int>(last->text().size()));
    }
}

void Selection::extendToWord(BoxNode* node, int offset) {
    if (!node || !node->isTextNode()) return;
    const auto& text = node->text();
    int len = static_cast<int>(text.size());

    // Find word boundaries
    int start = offset;
    int end = offset;

    while (start > 0 && !std::isspace(static_cast<unsigned char>(text[start - 1]))) start--;
    while (end < len && !std::isspace(static_cast<unsigned char>(text[end]))) end++;

    setAnchor(node, start);
    setFocus(node, end);
}

void Selection::extendToLine(BoxNode* node) {
    if (!node || !node->isTextNode()) return;
    setAnchor(node, 0);
    setFocus(node, static_cast<int>(node->text().size()));
}

std::string Selection::getSelectedText() const {
    if (!hasSelection() || isCollapsed()) return "";

    if (anchor_.node == focus_.node && anchor_.node && anchor_.node->isTextNode()) {
        // Same node
        int start = std::min(anchor_.offset, focus_.offset);
        int end = std::max(anchor_.offset, focus_.offset);
        const auto& text = anchor_.node->text();
        if (start < 0) start = 0;
        if (end > static_cast<int>(text.size())) end = static_cast<int>(text.size());
        return text.substr(start, end - start);
    }

    // Multi-node selection
    auto nodes = textNodesBetween(anchor_.node, focus_.node);
    std::string result;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!nodes[i]->isTextNode()) continue;
        const auto& text = nodes[i]->text();
        if (nodes[i] == anchor_.node) {
            result += text.substr(std::max(0, anchor_.offset));
        } else if (nodes[i] == focus_.node) {
            int end = std::min(focus_.offset, static_cast<int>(text.size()));
            result += text.substr(0, end);
        } else {
            result += text;
        }
    }
    return result;
}

std::vector<Rect> Selection::getSelectionRects() const {
    std::vector<Rect> rects;
    if (!hasSelection() || isCollapsed()) return rects;

    if (anchor_.node == focus_.node && anchor_.node && anchor_.node->isTextNode()) {
        auto& lb = anchor_.node->layoutBox();
        float fontSize = anchor_.node->computed().fontSize;
        if (fontSize <= 0) fontSize = 16.0f;
        float charWidth = fontSize * 0.55f;
        float lineHeight = fontSize * anchor_.node->computed().lineHeight;

        int start = std::min(anchor_.offset, focus_.offset);
        int end = std::max(anchor_.offset, focus_.offset);

        float x = lb.x + lb.borderLeft + lb.paddingLeft + start * charWidth;
        float w = (end - start) * charWidth;
        rects.push_back(Rect(x, lb.y, w, lineHeight));
    }

    return rects;
}

Rect Selection::caretRect() const {
    if (!focus_.node) return Rect(0, 0, 0, 0);

    auto& lb = focus_.node->layoutBox();
    float fontSize = focus_.node->computed().fontSize;
    if (fontSize <= 0) fontSize = 16.0f;
    float charWidth = fontSize * 0.55f;
    float lineHeight = fontSize * focus_.node->computed().lineHeight;

    float x = lb.x + lb.borderLeft + lb.paddingLeft + focus_.offset * charWidth;
    return Rect(x, lb.y, 2.0f, lineHeight); // 2px wide caret
}

Selection::Direction Selection::direction() const {
    if (!anchor_.node || !focus_.node) return Direction::None;
    if (anchor_.node == focus_.node) {
        if (focus_.offset > anchor_.offset) return Direction::Forward;
        if (focus_.offset < anchor_.offset) return Direction::Backward;
        return Direction::None;
    }

    // Determine tree order by walking ancestor chains
    std::vector<BoxNode*> chainA, chainF;
    for (auto* n = anchor_.node; n; n = n->parent()) chainA.push_back(n);
    for (auto* n = focus_.node; n; n = n->parent()) chainF.push_back(n);

    // Walk from root (back of chain) toward leaves until chains diverge
    int ia = static_cast<int>(chainA.size()) - 1;
    int ifa = static_cast<int>(chainF.size()) - 1;
    while (ia >= 0 && ifa >= 0 && chainA[ia] == chainF[ifa]) { ia--; ifa--; }

    if (ia < 0) return Direction::Forward;  // anchor is ancestor of focus
    if (ifa < 0) return Direction::Backward; // focus is ancestor of anchor

    // Compare sibling indices under their common ancestor
    size_t idxA = chainA[ia]->childIndex();
    size_t idxF = chainF[ifa]->childIndex();
    return (idxA < idxF) ? Direction::Forward : Direction::Backward;
}

std::vector<BoxNode*> Selection::textNodesBetween(BoxNode* from, BoxNode* to) const {
    std::vector<BoxNode*> result;
    if (!from || !to) return result;

    // DFS to find all text nodes between from and to
    BoxNode* root = from;
    while (root->parent()) root = root->parent();

    bool collecting = false;
    std::function<bool(BoxNode*)> walk = [&](BoxNode* node) -> bool {
        if (node == from) collecting = true;
        if (collecting && node->isTextNode()) {
            result.push_back(node);
        }
        if (node == to) return true; // Done

        for (auto& child : node->children()) {
            if (walk(child.get())) return true;
        }
        return false;
    };
    walk(root);

    return result;
}

// ==================================================================
// InputHandler
// ==================================================================

InputHandler::InputHandler() {}

void InputHandler::onMouseDown(BoxNode* root, float x, float y, int button,
                                  bool shift, bool ctrl, bool alt, bool meta) {
    auto hit = hitTester_.hitTest(root, x, y);
    if (!hit) return;

    mouseDown_ = true;
    mouseDownX_ = x;
    mouseDownY_ = y;
    active_ = hit.node;

    // Focus management
    if (FocusManager::isFocusable(hit.node)) {
        FocusManager::instance().setFocus(hit.node);
    }

    // Selection: collapse caret at click point
    if (hit.textOffset >= 0) {
        if (shift) {
            Selection::instance().setFocus(hit.node, hit.textOffset);
        } else {
            Selection::instance().collapse(hit.node, hit.textOffset);
        }
    }

    dispatchMouseEvent("mousedown", hit.node, x, y, button, shift, ctrl, alt, meta);
}

void InputHandler::onMouseUp(BoxNode* root, float x, float y, int button,
                                bool shift, bool ctrl, bool alt, bool meta) {
    auto hit = hitTester_.hitTest(root, x, y);
    mouseDown_ = false;
    active_ = nullptr;

    if (hit) {
        dispatchMouseEvent("mouseup", hit.node, x, y, button, shift, ctrl, alt, meta);

        // Click if same target as mousedown
        float dx = x - mouseDownX_;
        float dy = y - mouseDownY_;
        if (std::sqrt(dx * dx + dy * dy) < 5.0f) {
            dispatchMouseEvent("click", hit.node, x, y, button, shift, ctrl, alt, meta);
        }
    }
}

void InputHandler::onMouseMove(BoxNode* root, float x, float y,
                                  bool shift, bool ctrl, bool alt, bool meta) {
    auto hit = hitTester_.hitTest(root, x, y);
    BoxNode* newTarget = hit ? hit.node : nullptr;

    // Update hover
    updateHover(root, newTarget);

    // Selection drag
    if (mouseDown_ && hit && hit.textOffset >= 0) {
        Selection::instance().setFocus(hit.node, hit.textOffset);
    }

    if (newTarget) {
        dispatchMouseEvent("mousemove", newTarget, x, y, 0, shift, ctrl, alt, meta);
    }
}

void InputHandler::onDoubleClick(BoxNode* root, float x, float y) {
    auto hit = hitTester_.hitTest(root, x, y);
    if (!hit) return;

    // Double-click selects a word
    if (hit.textOffset >= 0) {
        Selection::instance().extendToWord(hit.node, hit.textOffset);
    }

    dispatchMouseEvent("dblclick", hit.node, x, y, 0, false, false, false, false);
}

void InputHandler::onKeyDown(BoxNode* root, const std::string& key, const std::string& code,
                                bool shift, bool ctrl, bool alt, bool meta) {
    BoxNode* target = FocusManager::instance().focusedElement();
    if (!target) target = root;

    KeyboardEvent event("keydown", key, code);
    event.setModifiers(alt, ctrl, shift, meta);
    event.setTrusted(true);
    EventDispatcher::dispatch(event, target);

    // Tab navigation
    if (key == "Tab" && !event.defaultPrevented()) {
        if (shift) FocusManager::instance().focusPrev();
        else FocusManager::instance().focusNext();
    }

    // Select All
    if (key == "a" && ctrl && !event.defaultPrevented()) {
        Selection::instance().selectAll(root);
    }
}

void InputHandler::onKeyUp(BoxNode* root, const std::string& key, const std::string& code,
                               bool shift, bool ctrl, bool alt, bool meta) {
    BoxNode* target = FocusManager::instance().focusedElement();
    if (!target) target = root;

    KeyboardEvent event("keyup", key, code);
    event.setModifiers(alt, ctrl, shift, meta);
    event.setTrusted(true);
    EventDispatcher::dispatch(event, target);
}

void InputHandler::onWheel(BoxNode* root, float x, float y, float deltaX, float deltaY) {
    auto hit = hitTester_.hitTest(root, x, y);
    if (!hit) return;

    WheelEvent event(x, y, deltaX, deltaY);
    event.setTrusted(true);
    EventDispatcher::dispatch(event, hit.node);

    if (!event.defaultPrevented()) {
        ScrollManager::instance().handleWheel(hit.node, deltaX, deltaY);
    }
}

void InputHandler::updateHover(BoxNode* root, BoxNode* newTarget) {
    if (newTarget == hovered_) return;

    BoxNode* oldTarget = hovered_;
    hovered_ = newTarget;

    // Dispatch mouseleave on old, mouseenter on new
    if (oldTarget) {
        MouseEvent leaveEvent("mouseleave", 0, 0, 0);
        leaveEvent.setTrusted(true);
        EventDispatcher::dispatch(leaveEvent, oldTarget);
    }

    if (newTarget) {
        MouseEvent enterEvent("mouseenter", 0, 0, 0);
        enterEvent.setTrusted(true);
        EventDispatcher::dispatch(enterEvent, newTarget);
    }

    (void)root;
}

void InputHandler::dispatchMouseEvent(const std::string& type, BoxNode* target,
                                          float x, float y, int button,
                                          bool shift, bool ctrl, bool alt, bool meta) {
    MouseEvent event(type, x, y, button);
    event.setModifiers(alt, ctrl, shift, meta);
    event.setTrusted(true);
    EventDispatcher::dispatch(event, target);
}

} // namespace Web
} // namespace NXRender
