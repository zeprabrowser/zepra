// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/events.h"
#include <algorithm>
#include "web/input.h"
#include "web/box/box_tree.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace NXRender {
namespace Web {

// ==================================================================
// Pointer Event (W3C Pointer Events Level 3)
// ==================================================================

class PointerEvent : public MouseEvent {
public:
    PointerEvent(const std::string& type, int pointerId,
                   float clientX, float clientY, int button = 0);

    int pointerId() const { return pointerId_; }
    float width() const { return width_; }
    float height() const { return height_; }
    float pressure() const { return pressure_; }
    float tangentialPressure() const { return tangentialPressure_; }
    int tiltX() const { return tiltX_; }
    int tiltY() const { return tiltY_; }
    int twist() const { return twist_; }
    float altitudeAngle() const { return altitudeAngle_; }
    float azimuthAngle() const { return azimuthAngle_; }
    const std::string& pointerType() const { return pointerType_; }
    bool isPrimary() const { return isPrimary_; }

    void setDimensions(float w, float h) { width_ = w; height_ = h; }
    void setPressure(float p) { pressure_ = p; }
    void setTangentialPressure(float p) { tangentialPressure_ = p; }
    void setTilt(int x, int y) { tiltX_ = x; tiltY_ = y; }
    void setTwist(int t) { twist_ = t; }
    void setAngles(float altitude, float azimuth) { altitudeAngle_ = altitude; azimuthAngle_ = azimuth; }
    void setPointerType(const std::string& type) { pointerType_ = type; }
    void setPrimary(bool p) { isPrimary_ = p; }

    // Coalesced events (for high-frequency input)
    const std::vector<std::unique_ptr<PointerEvent>>& getCoalescedEvents() const { return coalesced_; }
    void addCoalescedEvent(std::unique_ptr<PointerEvent> ev) { coalesced_.push_back(std::move(ev)); }

    // Predicted events (for reducing latency)
    const std::vector<std::unique_ptr<PointerEvent>>& getPredictedEvents() const { return predicted_; }
    void addPredictedEvent(std::unique_ptr<PointerEvent> ev) { predicted_.push_back(std::move(ev)); }

private:
    int pointerId_;
    float width_ = 1;
    float height_ = 1;
    float pressure_ = 0;
    float tangentialPressure_ = 0;
    int tiltX_ = 0, tiltY_ = 0;
    int twist_ = 0;
    float altitudeAngle_ = 1.5707963f; // π/2
    float azimuthAngle_ = 0;
    std::string pointerType_ = "mouse";
    bool isPrimary_ = true;
    std::vector<std::unique_ptr<PointerEvent>> coalesced_;
    std::vector<std::unique_ptr<PointerEvent>> predicted_;
};

// ==================================================================
// Touch Event (W3C Touch Events Level 2)
// ==================================================================

struct Touch {
    int identifier = 0;
    BoxNode* target = nullptr;
    float clientX = 0, clientY = 0;
    float pageX = 0, pageY = 0;
    float screenX = 0, screenY = 0;
    float radiusX = 0, radiusY = 0;
    float rotationAngle = 0;
    float force = 0;
    float altitudeAngle = 1.5707963f;
    float azimuthAngle = 0;
    std::string touchType = "direct"; // "direct" or "stylus"
};

class TouchList {
public:
    size_t length() const { return touches_.size(); }
    const Touch* item(size_t index) const {
        return (index < touches_.size()) ? &touches_[index] : nullptr;
    }
    void add(const Touch& touch) { touches_.push_back(touch); }
    const Touch* identifiedTouch(int id) const;

    const std::vector<Touch>& all() const { return touches_; }

private:
    std::vector<Touch> touches_;
};

class TouchEvent : public Event {
public:
    TouchEvent(const std::string& type);

    const TouchList& touches() const { return touches_; }
    const TouchList& targetTouches() const { return targetTouches_; }
    const TouchList& changedTouches() const { return changedTouches_; }

    TouchList& touches() { return touches_; }
    TouchList& targetTouches() { return targetTouches_; }
    TouchList& changedTouches() { return changedTouches_; }

    bool altKey() const { return altKey_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool metaKey() const { return metaKey_; }

    void setModifiers(bool alt, bool ctrl, bool shift, bool meta) {
        altKey_ = alt; ctrlKey_ = ctrl; shiftKey_ = shift; metaKey_ = meta;
    }

private:
    TouchList touches_;
    TouchList targetTouches_;
    TouchList changedTouches_;
    bool altKey_ = false, ctrlKey_ = false, shiftKey_ = false, metaKey_ = false;
};

// ==================================================================
// Pointer capture state
// ==================================================================

class PointerCapture {
public:
    static PointerCapture& instance();

    void setCapture(int pointerId, BoxNode* element);
    void releaseCapture(int pointerId);
    BoxNode* getCaptureTarget(int pointerId) const;
    bool hasCapture(int pointerId) const;

    // Implicit capture (for direct-manipulation pointers like touch)
    void setImplicitCapture(int pointerId, BoxNode* element);

    // Pending capture changes (applied between events)
    void processPending();

private:
    PointerCapture() = default;

    struct CaptureEntry {
        int pointerId;
        BoxNode* element;
        bool implicit;
    };
    std::vector<CaptureEntry> active_;
    std::vector<CaptureEntry> pending_;
};

// ==================================================================
// Pointer/Touch input handler — extends InputHandler
// ==================================================================

class PointerInputHandler {
public:
    PointerInputHandler();

    // Pointer events from platform
    void onPointerDown(BoxNode* root, int pointerId, float x, float y,
                         const std::string& pointerType, int button,
                         float pressure, float width, float height,
                         int tiltX, int tiltY, int twist,
                         bool shift, bool ctrl, bool alt, bool meta);

    void onPointerMove(BoxNode* root, int pointerId, float x, float y,
                         const std::string& pointerType,
                         float pressure, float width, float height,
                         int tiltX, int tiltY, int twist,
                         bool shift, bool ctrl, bool alt, bool meta);

    void onPointerUp(BoxNode* root, int pointerId, float x, float y,
                       const std::string& pointerType, int button,
                       bool shift, bool ctrl, bool alt, bool meta);

    void onPointerCancel(BoxNode* root, int pointerId);

    // Touch events from platform (translated to pointer + touch events)
    void onTouchStart(BoxNode* root, const std::vector<Touch>& touches,
                        bool shift, bool ctrl, bool alt, bool meta);
    void onTouchMove(BoxNode* root, const std::vector<Touch>& changed,
                       bool shift, bool ctrl, bool alt, bool meta);
    void onTouchEnd(BoxNode* root, const std::vector<Touch>& changed,
                      bool shift, bool ctrl, bool alt, bool meta);
    void onTouchCancel(BoxNode* root, const std::vector<Touch>& changed);

    // Coalesced event buffer (for pointermove batching)
    void flushCoalesced();

    // Pointer state queries
    struct PointerState {
        int id;
        std::string type;
        bool down = false;
        float x = 0, y = 0;
        float pressure = 0;
        BoxNode* target = nullptr;
        BoxNode* overTarget = nullptr;
    };
    const PointerState* getPointer(int id) const;

private:
    HitTester hitTester_;
    std::unordered_map<int, PointerState> pointers_;
    std::vector<std::unique_ptr<PointerEvent>> coalescedBuffer_;
    int nextTouchPointerId_ = 100; // Touch pointer IDs start at 100

    // Active touch state
    TouchList activeTouches_;

    // Dispatch pointer event and return whether default was prevented
    bool dispatchPointer(const std::string& type, BoxNode* target,
                           int pointerId, float x, float y,
                           const std::string& pointerType, int button,
                           float pressure, float width, float height,
                           int tiltX, int tiltY, int twist, bool isPrimary,
                           bool shift, bool ctrl, bool alt, bool meta);

    // Dispatch compatibility mouse event (after pointer event, unless prevented)
    void dispatchCompatMouse(const std::string& type, BoxNode* target,
                               float x, float y, int button,
                               bool shift, bool ctrl, bool alt, bool meta);

    // Dispatch touch event
    void dispatchTouch(const std::string& type, BoxNode* target,
                         const TouchList& touches,
                         const TouchList& targetTouches,
                         const TouchList& changedTouches,
                         bool shift, bool ctrl, bool alt, bool meta);

    // Pointer enter/leave sequences
    void updatePointerOver(BoxNode* root, int pointerId, BoxNode* newTarget,
                              float x, float y, const std::string& pointerType,
                              bool shift, bool ctrl, bool alt, bool meta);
};

// ==================================================================
// Dialog & Popover API (HTML Living Standard)
// ==================================================================

class DialogManager {
public:
    static DialogManager& instance();

    // <dialog> showModal()
    struct DialogState {
        BoxNode* dialogBox = nullptr;
        bool modal = false;
        std::string returnValue;
    };

    void showModal(BoxNode* dialog);
    void show(BoxNode* dialog);
    void close(BoxNode* dialog, const std::string& returnValue = "");
    bool isOpen(BoxNode* dialog) const;
    bool isModal(BoxNode* dialog) const;

    // Top layer stack (modals, popovers, fullscreen)
    const std::vector<BoxNode*>& topLayerStack() const { return topLayer_; }
    void addToTopLayer(BoxNode* element);
    void removeFromTopLayer(BoxNode* element);
    bool isInTopLayer(BoxNode* element) const;

    // Inert subtrees (behind modal dialogs)
    bool isInertAt(BoxNode* node) const;

    // Close watcher (Escape key handling)
    struct CloseWatcher {
        BoxNode* target;
        std::function<void()> onCancel;
        std::function<void()> onClose;
    };
    void pushCloseWatcher(CloseWatcher watcher);
    void popCloseWatcher();
    bool handleEscapeKey();

    // <dialog> callbacks
    using DialogCallback = std::function<void(BoxNode* dialog, const std::string& returnValue)>;
    void setOnClose(DialogCallback cb) { onClose_ = std::move(cb); }
    void setOnCancel(DialogCallback cb) { onCancel_ = std::move(cb); }

    // Popover API
    void showPopover(BoxNode* popover);
    void hidePopover(BoxNode* popover);
    bool togglePopover(BoxNode* popover);
    bool isPopoverOpen(BoxNode* popover) const;

    // Popover auto-dismiss (light dismiss)
    bool handleLightDismiss(float x, float y);

    // Active popovers
    const std::vector<BoxNode*>& activePopovers() const { return activePopovers_; }

private:
    DialogManager() = default;

    std::vector<BoxNode*> topLayer_;
    std::vector<DialogState> dialogs_;
    std::vector<CloseWatcher> closeWatchers_;
    std::vector<BoxNode*> activePopovers_;

    DialogCallback onClose_;
    DialogCallback onCancel_;

    // Backdrop node for modal dialogs
    std::unique_ptr<BoxNode> createBackdrop();

    // Track inert roots
    std::vector<BoxNode*> inertRoots_;
    void updateInertState();
};

// ==================================================================
// CSS Grid Track Sizing (CSS Grid Layout Module Level 2)
// ==================================================================

class GridTrackSizer {
public:
    // Track sizing function types
    enum class TrackType : uint8_t {
        Fixed,      // e.g., 200px
        Percent,    // e.g., 25%
        Fr,         // e.g., 1fr
        Auto,       // auto
        MinContent, // min-content
        MaxContent, // max-content
        FitContent, // fit-content(200px)
        MinMax,     // minmax(100px, 1fr)
        Repeat,     // repeat(3, 1fr)
    };

    struct TrackSize {
        TrackType type = TrackType::Auto;
        float value = 0;

        // For minmax()
        TrackType minType = TrackType::Auto;
        float minValue = 0;
        TrackType maxType = TrackType::Auto;
        float maxValue = 0;

        // For repeat()
        int repeatCount = 0;          // 0 = auto-fill, -1 = auto-fit
        std::vector<TrackSize> repeatPattern;

        // For fit-content()
        float fitContentLimit = 0;

        bool isIntrinsic() const {
            return type == TrackType::Auto || type == TrackType::MinContent ||
                   type == TrackType::MaxContent || type == TrackType::FitContent;
        }
        bool isFlexible() const { return type == TrackType::Fr; }
    };

    // Named grid lines
    struct NamedLine {
        std::string name;
        int index;
    };

    // Resolved track
    struct Track {
        TrackSize sizing;
        float baseSize = 0;
        float growthLimit = -1; // -1 = infinity
        float plannedIncrease = 0;
        bool frozen = false;

        // Resolved position and size
        float offset = 0;
        float size = 0;
    };

    // Grid placement
    struct GridArea {
        int rowStart = -1, rowEnd = -1;
        int colStart = -1, colEnd = -1;
    };

    struct GridItem {
        BoxNode* node = nullptr;
        GridArea area;
        // Intrinsic sizes
        float minContentWidth = 0, maxContentWidth = 0;
        float minContentHeight = 0, maxContentHeight = 0;
    };

    // Parse grid-template-columns/rows string
    static std::vector<TrackSize> parseTrackList(const std::string& trackList);

    // Resolve auto-fill/auto-fit repeat counts
    static std::vector<TrackSize> expandRepeats(const std::vector<TrackSize>& tracks,
                                                      float availableSize);

    // Main algorithm: CSS Grid Track Sizing Algorithm (§12.3-12.7)
    void sizeColumnTracks(std::vector<Track>& tracks,
                            const std::vector<GridItem>& items,
                            float availableWidth);
    void sizeRowTracks(std::vector<Track>& tracks,
                         const std::vector<GridItem>& items,
                         float availableHeight,
                         const std::vector<Track>& columnTracks);

    // Gap
    float rowGap = 0;
    float columnGap = 0;

    // Compute total track size
    static float totalTrackSize(const std::vector<Track>& tracks, float gap);

private:
    // §12.4: Initialize track sizes
    void initializeTrackSizes(std::vector<Track>& tracks, float availableSize);

    // §12.5: Resolve intrinsic track sizes
    void resolveIntrinsicSizes(std::vector<Track>& tracks,
                                 const std::vector<GridItem>& items,
                                 bool isColumn);

    // §12.6: Maximize tracks (distribute free space)
    void maximizeTracks(std::vector<Track>& tracks, float freeSpace);

    // §12.7: Expand flexible tracks (fr units)
    void expandFlexibleTracks(std::vector<Track>& tracks,
                                const std::vector<GridItem>& items,
                                float availableSize);

    // Distribute extra space to intrinsic tracks
    void distributeExtraSpace(std::vector<Track>& tracks,
                                const std::vector<int>& trackIndices,
                                float extraSpace,
                                bool toBase);

    // Compute min/max contribution for an item spanning tracks
    float minContribution(const GridItem& item, bool isColumn) const;
    float maxContribution(const GridItem& item, bool isColumn) const;

    // Resolve a track sizing function to a fixed value (if possible)
    float resolveFixed(const TrackSize& ts, float availableSize) const;
};

// ==================================================================
// Grid placement algorithm (CSS Grid §8)
// ==================================================================

class GridPlacement {
public:
    struct PlacedItem {
        BoxNode* node = nullptr;
        int rowStart, rowEnd;
        int colStart, colEnd;
    };

    // Auto-placement algorithm
    static std::vector<PlacedItem> place(
        const std::vector<BoxNode*>& items,
        int explicitRowCount, int explicitColCount,
        bool denseFlow = false);

    // Resolve grid-row/grid-column shorthand to line numbers
    static void resolveGridPosition(const std::string& gridRow, const std::string& gridCol,
                                       int rowCount, int colCount,
                                       int& rowStart, int& rowEnd,
                                       int& colStart, int& colEnd);

    // Parse span notation (e.g., "span 2", "2 / span 3", "auto")
    static void parsePlacement(const std::string& value, int& start, int& end, int trackCount);

private:
    struct OccupancyGrid {
        std::vector<std::vector<bool>> cells;
        int rows, cols;

        void resize(int r, int c);
        bool isAvailable(int r, int c, int rowSpan, int colSpan) const;
        void occupy(int r, int c, int rowSpan, int colSpan);
        // Find next available slot
        std::pair<int,int> findSlot(int rowSpan, int colSpan, int startRow, int startCol, bool dense) const;
    };
};

} // namespace Web
} // namespace NXRender
