// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "nxgfx/primitives.h"
#include <algorithm>
#include "nxgfx/transform.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>

namespace NXRender {
namespace Web {

class BoxNode;
class Event;

// ==================================================================
// HitTestResult — what was hit at a point
// ==================================================================

struct HitTestResult {
    BoxNode* node = nullptr;          // Innermost node at the point
    float localX = 0, localY = 0;    // Point in node's local coordinate space
    float scrollX = 0, scrollY = 0;  // Scroll offset at the hit point

    // Full path from root to innermost node (root first)
    std::vector<BoxNode*> path;

    // Is the point inside the node's content area (excluding scrollbars)?
    bool insideContent = false;

    // Is the point on a scrollbar?
    bool onVerticalScrollbar = false;
    bool onHorizontalScrollbar = false;

    // Text offset if on a text node
    int textOffset = -1;

    operator bool() const { return node != nullptr; }
};

// ==================================================================
// HitTester — finds elements at screen coordinates
// ==================================================================

class HitTester {
public:
    HitTester();

    // Hit test at a viewport coordinate
    HitTestResult hitTest(BoxNode* root, float x, float y, float scrollX = 0, float scrollY = 0);

    // Hit test for all elements at a point (not just innermost)
    std::vector<HitTestResult> hitTestAll(BoxNode* root, float x, float y);

    // Check if a point is inside a node's border box
    static bool pointInNode(BoxNode* node, float x, float y);

    // Check pointer-events property
    static bool acceptsPointerEvents(BoxNode* node);

    // Approximate text offset at x coordinate within a text node
    static int textOffsetAtX(BoxNode* textNode, float localX);

private:
    // Recursive hit test — returns true if a hit was found
    bool hitTestNode(BoxNode* node, float x, float y, HitTestResult& result);

    // Transform point from parent to local coordinates
    Point toLocalCoords(BoxNode* node, float x, float y);
};

// ==================================================================
// ScrollState — per-node scroll state
// ==================================================================

struct ScrollState {
    float scrollTop = 0;
    float scrollLeft = 0;
    float scrollWidth = 0;    // Content width (may exceed viewport)
    float scrollHeight = 0;   // Content height
    float clientWidth = 0;    // Viewport width (visible area)
    float clientHeight = 0;

    float maxScrollTop() const { return std::max(0.0f, scrollHeight - clientHeight); }
    float maxScrollLeft() const { return std::max(0.0f, scrollWidth - clientWidth); }

    bool canScrollVertically() const { return scrollHeight > clientHeight; }
    bool canScrollHorizontally() const { return scrollWidth > clientWidth; }

    // Clamp scroll position
    void clamp() {
        scrollTop = std::clamp(scrollTop, 0.0f, maxScrollTop());
        scrollLeft = std::clamp(scrollLeft, 0.0f, maxScrollLeft());
    }
};

// ==================================================================
// ScrollManager — manages scroll containers in the tree
// ==================================================================

class ScrollManager {
public:
    static ScrollManager& instance();

    // Register a scroll container (overflow:auto|scroll|hidden)
    void registerScrollContainer(BoxNode* node);
    void unregisterScrollContainer(BoxNode* node);

    // Get/set scroll state for a node
    ScrollState* getScrollState(BoxNode* node);
    void setScrollState(BoxNode* node, const ScrollState& state);

    // Scroll operations
    void scrollTo(BoxNode* node, float x, float y, bool smooth = false);
    void scrollBy(BoxNode* node, float dx, float dy, bool smooth = false);
    void scrollIntoView(BoxNode* target, bool alignTop = true);

    // Find the nearest scroll container for a node
    BoxNode* findScrollContainer(BoxNode* node);

    // Process wheel event: find scroll container, apply delta, return consumed
    bool handleWheel(BoxNode* target, float deltaX, float deltaY);

    // Smooth scroll animation tick
    void tick(double timestamp);

    // Get all scroll containers
    const std::vector<BoxNode*>& scrollContainers() const { return containers_; }

private:
    ScrollManager() = default;

    struct ScrollContainer {
        BoxNode* node = nullptr;
        ScrollState state;
        // Smooth scroll animation
        bool animating = false;
        float targetX = 0, targetY = 0;
        double animStart = 0;
        double animDuration = 300; // ms
        float startX = 0, startY = 0;
    };

    std::vector<BoxNode*> containers_;
    std::vector<ScrollContainer> scrollData_;

    ScrollContainer* findContainer(BoxNode* node);
    static bool isScrollContainer(BoxNode* node);
    static float easeOutCubic(float t);
};

// ==================================================================
// Selection — text selection state
// ==================================================================

struct SelectionEndpoint {
    BoxNode* node = nullptr;
    int offset = 0;   // Character offset within the text node
};

class Selection {
public:
    static Selection& instance();

    // Selection state
    bool isCollapsed() const { return anchor_.node == focus_.node && anchor_.offset == focus_.offset; }
    bool hasSelection() const { return anchor_.node != nullptr; }

    const SelectionEndpoint& anchor() const { return anchor_; }
    const SelectionEndpoint& focus() const { return focus_; }

    // Set selection
    void setAnchor(BoxNode* node, int offset);
    void setFocus(BoxNode* node, int offset);
    void collapse(BoxNode* node, int offset);
    void selectAll(BoxNode* root);
    void clear();

    // Extend selection
    void extendToWord(BoxNode* node, int offset);
    void extendToLine(BoxNode* node);

    // Get selected text
    std::string getSelectedText() const;

    // Get selection rects (for rendering highlight)
    std::vector<Rect> getSelectionRects() const;

    // Caret position (when collapsed)
    Rect caretRect() const;
    bool caretVisible() const { return caretVisible_; }
    void toggleCaretBlink() { caretVisible_ = !caretVisible_; }

    // Selection direction
    enum class Direction { None, Forward, Backward };
    Direction direction() const;

private:
    Selection() = default;
    SelectionEndpoint anchor_;
    SelectionEndpoint focus_;
    bool caretVisible_ = true;

    // Collect text nodes in document order between two endpoints
    std::vector<BoxNode*> textNodesBetween(BoxNode* from, BoxNode* to) const;
};

// ==================================================================
// InputHandler — bridges native input to DOM events
// ==================================================================

class InputHandler {
public:
    InputHandler();

    // Mouse events from the platform
    void onMouseDown(BoxNode* root, float x, float y, int button,
                       bool shift, bool ctrl, bool alt, bool meta);
    void onMouseUp(BoxNode* root, float x, float y, int button,
                     bool shift, bool ctrl, bool alt, bool meta);
    void onMouseMove(BoxNode* root, float x, float y,
                       bool shift, bool ctrl, bool alt, bool meta);
    void onDoubleClick(BoxNode* root, float x, float y);

    // Keyboard events from the platform
    void onKeyDown(BoxNode* root, const std::string& key, const std::string& code,
                     bool shift, bool ctrl, bool alt, bool meta);
    void onKeyUp(BoxNode* root, const std::string& key, const std::string& code,
                   bool shift, bool ctrl, bool alt, bool meta);

    // Wheel events
    void onWheel(BoxNode* root, float x, float y, float deltaX, float deltaY);

    // Track hover state for :hover pseudo-class
    BoxNode* hoveredNode() const { return hovered_; }

    // Track active state for :active pseudo-class
    BoxNode* activeNode() const { return active_; }

private:
    HitTester hitTester_;

    BoxNode* hovered_ = nullptr;
    BoxNode* active_ = nullptr;
    bool mouseDown_ = false;
    float mouseDownX_ = 0, mouseDownY_ = 0;

    // Hover enter/leave tracking
    void updateHover(BoxNode* root, BoxNode* newTarget);
    void dispatchMouseEvent(const std::string& type, BoxNode* target, float x, float y,
                              int button, bool shift, bool ctrl, bool alt, bool meta);
};

} // namespace Web
} // namespace NXRender
