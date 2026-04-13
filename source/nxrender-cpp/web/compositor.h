// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/box/box_tree.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace NXRender {
namespace Web {

// ==================================================================
// CSSOM Query APIs — getComputedStyle, getBoundingClientRect, etc.
// ==================================================================

class CSSOMQuery {
public:
    // DOMRect — return type for getBoundingClientRect
    struct DOMRect {
        float x = 0, y = 0;
        float width = 0, height = 0;
        float top() const { return y; }
        float left() const { return x; }
        float bottom() const { return y + height; }
        float right() const { return x + width; }
    };

    // getBoundingClientRect() — viewport-relative
    static DOMRect getBoundingClientRect(const BoxNode* node, float scrollX = 0, float scrollY = 0);

    // getClientRects() — for inline elements spanning multiple lines
    static std::vector<DOMRect> getClientRects(const BoxNode* node, float scrollX = 0, float scrollY = 0);

    // Element dimension properties
    static float clientWidth(const BoxNode* node);
    static float clientHeight(const BoxNode* node);
    static float clientTop(const BoxNode* node);
    static float clientLeft(const BoxNode* node);

    static float offsetWidth(const BoxNode* node);
    static float offsetHeight(const BoxNode* node);
    static float offsetTop(const BoxNode* node);
    static float offsetLeft(const BoxNode* node);
    static const BoxNode* offsetParent(const BoxNode* node);

    static float scrollWidth(const BoxNode* node);
    static float scrollHeight(const BoxNode* node);
    static float scrollTop(const BoxNode* node);
    static float scrollLeft(const BoxNode* node);

    // window.innerWidth / innerHeight
    static float innerWidth(float viewportWidth);
    static float innerHeight(float viewportHeight);

    // getComputedStyle(element) — returns serialized CSS property value
    static std::string getComputedStyleProperty(const BoxNode* node, const std::string& property);

    // matchMedia() — evaluate a media query
    static bool matchMedia(const std::string& query, float viewportW, float viewportH, float dpr);

    // elementFromPoint(x, y) — alias for hit testing
    static BoxNode* elementFromPoint(BoxNode* root, float x, float y);

    // elementsFromPoint(x, y)
    static std::vector<BoxNode*> elementsFromPoint(BoxNode* root, float x, float y);

    // caretPositionFromPoint / caretRangeFromPoint
    struct CaretPosition {
        BoxNode* node = nullptr;
        int offset = 0;
    };
    static CaretPosition caretPositionFromPoint(BoxNode* root, float x, float y);
};

// ==================================================================
// Media Query Evaluator (CSS Media Queries Level 5)
// ==================================================================

class MediaQueryEvaluator {
public:
    struct Environment {
        float viewportWidth = 1920;
        float viewportHeight = 1080;
        float devicePixelRatio = 1;
        std::string colorScheme = "light";    // "light" or "dark"
        std::string orientation = "landscape"; // "portrait" or "landscape"
        bool reducedMotion = false;
        bool reducedTransparency = false;
        bool reducedData = false;
        bool forcedColors = false;
        bool invertedColors = false;
        bool hover = true;                     // primary pointing device can hover
        bool anyHover = true;
        std::string pointer = "fine";          // "fine", "coarse", "none"
        std::string anyPointer = "fine";
        int colorBitDepth = 8;
        int monochrome = 0;
        float aspectRatio() const { return viewportWidth / viewportHeight; }
    };

    MediaQueryEvaluator();

    void setEnvironment(const Environment& env) { env_ = env; }
    const Environment& environment() const { return env_; }

    // Evaluate a full media query string
    bool evaluate(const std::string& query) const;

    // Parse and cache media query list
    struct MediaQuery {
        bool negated = false;
        std::string mediaType;  // "all", "screen", "print"

        struct Feature {
            std::string name;
            std::string value;
            bool isRange = false;
            // For range syntax: (min-width: 768px) or (width >= 768px)
            float minValue = 0;
            float maxValue = 1e9f;
            bool hasMin = false;
            bool hasMax = false;
        };
        std::vector<Feature> features;
    };

    std::vector<MediaQuery> parse(const std::string& queryList) const;
    bool evaluateQuery(const MediaQuery& query) const;
    bool evaluateFeature(const MediaQuery::Feature& feature) const;

    // Listener for changes
    using ChangeCallback = std::function<void(const std::string& query, bool matches)>;
    int addChangeListener(const std::string& query, ChangeCallback cb);
    void removeChangeListener(int id);

    // Re-evaluate all watched queries (called when environment changes)
    void reevaluate();

private:
    Environment env_;

    struct WatchedQuery {
        int id;
        std::string query;
        bool lastResult;
        ChangeCallback callback;
    };
    std::vector<WatchedQuery> watched_;
    int nextWatchId_ = 1;

    float parseLength(const std::string& value) const;
};

// ==================================================================
// Compositor — off-main-thread compositing and animation
// ==================================================================

class Compositor {
public:
    Compositor();
    ~Compositor();

    // Layer representation for compositing
    struct Layer {
        int id = 0;
        BoxNode* owner = nullptr;

        // Transform relative to parent layer
        float translateX = 0, translateY = 0;
        float scaleX = 1, scaleY = 1;
        float rotation = 0;
        float opacity = 1;

        // Content bounds
        float contentWidth = 0, contentHeight = 0;

        // Clip
        float clipX = 0, clipY = 0;
        float clipWidth = 0, clipHeight = 0;
        bool hasClip = false;

        // Rasterization
        bool needsRaster = true;
        bool isScrollContainer = false;
        float scrollOffsetX = 0, scrollOffsetY = 0;

        // Compositing reasons
        bool has3DTransform = false;
        bool hasWillChange = false;
        bool hasBackdropFilter = false;
        bool isFixedPosition = false;

        // Children
        std::vector<int> childIds;
    };

    // Layer tree management
    int createLayer(BoxNode* owner);
    void destroyLayer(int id);
    Layer* getLayer(int id);
    const Layer* getLayer(int id) const;

    void setLayerParent(int childId, int parentId);
    void removeLayerFromParent(int childId);

    // Update layer properties (can be called from compositor thread)
    void setLayerTransform(int id, float tx, float ty, float sx, float sy, float rotation);
    void setLayerOpacity(int id, float opacity);
    void setLayerScrollOffset(int id, float ox, float oy);
    void setLayerClip(int id, float x, float y, float w, float h);
    void clearLayerClip(int id);

    // Compositing
    void setRootLayerId(int id) { rootLayerId_ = id; }
    int rootLayerId() const { return rootLayerId_; }

    // Build layer tree from box tree
    void buildLayerTree(BoxNode* root);

    // Determine if a box needs its own layer
    static bool needsOwnLayer(const BoxNode* node);

    // Composite all layers (produces final frame)
    void composite();

    // Compositor-driven animations
    struct CompositorAnimation {
        int id = 0;
        int layerId = 0;
        std::string property; // "transform", "opacity"

        // Keyframes (normalized 0..1)
        struct Keyframe {
            float offset;
            float value;
        };
        std::vector<Keyframe> keyframes;

        double startTime = 0;
        double duration = 0;
        double delay = 0;
        int iterations = 1; // -1 = infinite
        bool alternate = false;
        bool paused = false;

        std::function<float(float)> easing; // timing function
    };

    int addAnimation(const CompositorAnimation& anim);
    void removeAnimation(int id);
    void pauseAnimation(int id);
    void resumeAnimation(int id);

    // Tick all compositor animations (called on compositor thread)
    void tickAnimations(double timestamp);

    // Frame stats
    struct CompositorStats {
        int layerCount = 0;
        int animationCount = 0;
        double compositeTimeMs = 0;
        double rasterTimeMs = 0;
        int rasterizedTiles = 0;
    };
    const CompositorStats& stats() const { return stats_; }

    // Thread safety: lock for main-thread access to compositor state
    void beginFrame() { mutex_.lock(); }
    void endFrame() { mutex_.unlock(); }

private:
    std::vector<Layer> layers_;
    int nextLayerId_ = 1;
    int rootLayerId_ = 0;

    std::vector<CompositorAnimation> animations_;
    int nextAnimId_ = 1;

    CompositorStats stats_;
    std::mutex mutex_;

    // Recursive compositing
    void compositeLayer(const Layer& layer, float parentX, float parentY, float parentOpacity);

    // Layer promotion logic
    void promoteLayer(BoxNode* node, int parentLayerId);
};

// ==================================================================
// Tile Rasterizer — on-demand tile-based rasterization
// ==================================================================

class TileRasterizer {
public:
    static constexpr int TILE_SIZE = 256;

    struct Tile {
        int id = 0;
        int layerId = 0;
        int col = 0, row = 0;
        bool dirty = true;
        bool rasterized = false;
        uint32_t textureId = 0;
        // Priority for rasterization scheduling
        float priority = 0;
    };

    TileRasterizer();
    ~TileRasterizer();

    // Create tiles for a layer
    void createTilesForLayer(int layerId, float contentWidth, float contentHeight);

    // Invalidate tiles in a region
    void invalidateRegion(int layerId, float x, float y, float w, float h);

    // Invalidate all tiles for a layer
    void invalidateLayer(int layerId);

    // Get visible tiles for a viewport
    std::vector<Tile*> getVisibleTiles(int layerId,
                                          float viewportX, float viewportY,
                                          float viewportW, float viewportH);

    // Rasterize a single tile (called from raster thread)
    void rasterizeTile(Tile* tile, BoxNode* layerRoot);

    // Prioritize tiles by distance from viewport center
    void prioritizeTiles(float viewportCenterX, float viewportCenterY);

    // Stats
    int totalTileCount() const;
    int dirtyTileCount() const;

private:
    std::unordered_map<int, std::vector<Tile>> layerTiles_;
    int nextTileId_ = 1;
};

// ==================================================================
// Scroll snap (CSS Scroll Snap Module Level 1)
// ==================================================================

class ScrollSnap {
public:
    enum class SnapType : uint8_t { None, Mandatory, Proximity };
    enum class SnapAlign : uint8_t { Start, End, Center };

    struct SnapPoint {
        float position;     // Snap position in scroll coordinates
        SnapAlign alignX = SnapAlign::Start;
        SnapAlign alignY = SnapAlign::Start;
    };

    // Find the nearest snap point for a scroll position
    static float snap(float scrollPos, float velocity,
                        const std::vector<SnapPoint>& points,
                        SnapType type, float viewportSize);

    // Collect snap points from the box tree
    static std::vector<SnapPoint> collectSnapPoints(BoxNode* scrollContainer, bool horizontal);

    // Apply scroll-snap-type to a scroll container
    static SnapType parseSnapType(const std::string& value);
    static SnapAlign parseSnapAlign(const std::string& value);
};

// ==================================================================
// CSS Containment (CSS Containment Module Level 2)
// — extended with content-visibility
// ==================================================================

class ContainmentEngine {
public:
    enum class ContainType : uint8_t {
        None = 0,
        Size = 1,
        Layout = 2,
        Style = 4,
        Paint = 8,
        Strict = Size | Layout | Style | Paint,
        Content = Layout | Style | Paint
    };

    // Check what containment applies to a node
    static uint8_t getContainment(const BoxNode* node);

    // content-visibility states
    enum class ContentVisibility : uint8_t { Visible, Hidden, Auto };
    static ContentVisibility getContentVisibility(const BoxNode* node);

    // Is this subtree skipped by content-visibility:auto?
    static bool isOffscreen(const BoxNode* node, float viewportTop, float viewportBottom);

    // Intrinsic size override for size containment
    struct ContainedSize {
        float width = 0;
        float height = 0;
        bool hasWidth = false;
        bool hasHeight = false;
    };
    static ContainedSize getContainedSize(const BoxNode* node);

    // Skip layout for contained subtrees
    static bool shouldSkipLayout(const BoxNode* node, float viewportTop, float viewportBottom);

    // Skip paint for contained subtrees
    static bool shouldSkipPaint(const BoxNode* node, float viewportTop, float viewportBottom);
};

} // namespace Web
} // namespace NXRender
