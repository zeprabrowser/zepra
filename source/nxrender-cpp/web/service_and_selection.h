// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/resource_loader.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace NXRender {
namespace Web {

// ==================================================================
// Service Worker Lifecycle (W3C Service Workers)
// ==================================================================

enum class ServiceWorkerState : uint8_t {
    Parsed, Installing, Installed, Activating, Activated, Redundant
};

class ServiceWorkerRegistration;
class ServiceWorkerContainer;

class ServiceWorker {
public:
    ServiceWorker(const std::string& scriptUrl, const std::string& scope);
    ~ServiceWorker();

    const std::string& scriptUrl() const { return scriptUrl_; }
    const std::string& scope() const { return scope_; }
    ServiceWorkerState state() const { return state_; }

    // Lifecycle
    void install();
    void activate();
    void terminate();

    // Messaging
    void postMessage(const std::string& message);

    // Event dispatch (internal)
    void dispatchInstallEvent(std::function<void(bool)> onComplete);
    void dispatchActivateEvent(std::function<void(bool)> onComplete);
    void dispatchFetchEvent(const ResourceRequest& request,
                              std::function<void(const ResourceResponse&)> respondWith);

    // Skip waiting
    void skipWaiting() { skipWaiting_ = true; }
    bool wantsSkipWaiting() const { return skipWaiting_; }

    // Clients.claim()
    void claimClients() { claimed_ = true; }

    // State change callback
    using StateChangeCallback = std::function<void(ServiceWorkerState)>;
    void onStateChange(StateChangeCallback cb) { stateChangeCb_ = cb; }

private:
    std::string scriptUrl_;
    std::string scope_;
    ServiceWorkerState state_ = ServiceWorkerState::Parsed;
    bool skipWaiting_ = false;
    bool claimed_ = false;
    StateChangeCallback stateChangeCb_;

    void setState(ServiceWorkerState newState);
};

// ==================================================================
// Service Worker Registration
// ==================================================================

class ServiceWorkerRegistration {
public:
    ServiceWorkerRegistration(const std::string& scope);

    const std::string& scope() const { return scope_; }

    ServiceWorker* installing() const { return installing_.get(); }
    ServiceWorker* waiting() const { return waiting_.get(); }
    ServiceWorker* active() const { return active_.get(); }

    // Update flow
    void setInstalling(std::unique_ptr<ServiceWorker> sw);
    void promoteInstallingToWaiting();
    void promoteWaitingToActive();

    // Unregister
    void unregister();
    bool isUnregistered() const { return unregistered_; }

    // Navigation preload
    void enableNavigationPreload(bool enable) { navPreload_ = enable; }
    bool navigationPreloadEnabled() const { return navPreload_; }
    void setNavigationPreloadHeader(const std::string& h) { navPreloadHeader_ = h; }
    const std::string& navigationPreloadHeader() const { return navPreloadHeader_; }

    // Update found callback
    using UpdateFoundCallback = std::function<void()>;
    void onUpdateFound(UpdateFoundCallback cb) { updateFoundCb_ = cb; }

private:
    std::string scope_;
    std::unique_ptr<ServiceWorker> installing_;
    std::unique_ptr<ServiceWorker> waiting_;
    std::unique_ptr<ServiceWorker> active_;
    bool unregistered_ = false;
    bool navPreload_ = false;
    std::string navPreloadHeader_ = "true";
    UpdateFoundCallback updateFoundCb_;
};

// ==================================================================
// Service Worker Container (navigator.serviceWorker)
// ==================================================================

class ServiceWorkerContainer {
public:
    static ServiceWorkerContainer& instance();

    // register(scriptUrl, { scope })
    ServiceWorkerRegistration* registerWorker(const std::string& scriptUrl,
                                                  const std::string& scope = "/");

    // getRegistration(scope)
    ServiceWorkerRegistration* getRegistration(const std::string& scope) const;

    // getRegistrations()
    std::vector<ServiceWorkerRegistration*> getRegistrations() const;

    // Controller for current page
    ServiceWorker* controller() const { return controller_; }
    void setController(ServiceWorker* sw) { controller_ = sw; }

    // Match a request URL against all registrations
    ServiceWorkerRegistration* matchRegistration(const std::string& url) const;

    // Route a fetch through service worker if applicable
    bool interceptFetch(const ResourceRequest& request,
                          std::function<void(const ResourceResponse&)> onResponse);

    // Message to controller
    void postMessage(const std::string& message);

    // Ready promise
    using ReadyCallback = std::function<void(ServiceWorkerRegistration*)>;
    void onReady(ReadyCallback cb);

    // controllerchange event
    using ControllerChangeCallback = std::function<void()>;
    void onControllerChange(ControllerChangeCallback cb) { controllerChangeCb_ = cb; }

private:
    ServiceWorkerContainer() = default;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ServiceWorkerRegistration>> registrations_;
    ServiceWorker* controller_ = nullptr;
    ReadyCallback readyCb_;
    ControllerChangeCallback controllerChangeCb_;

    bool scopeMatches(const std::string& scope, const std::string& url) const;
};

// ==================================================================
// Cache API (CacheStorage + Cache)
// ==================================================================

class Cache {
public:
    explicit Cache(const std::string& name);

    const std::string& name() const { return name_; }

    // cache.match(request)
    bool match(const ResourceRequest& request, ResourceResponse& out) const;

    // cache.matchAll(request?)
    std::vector<ResourceResponse> matchAll(const std::string& url = "") const;

    // cache.add(request)
    void add(const ResourceRequest& request, const ResourceResponse& response);

    // cache.put(request, response)
    void put(const std::string& url, const ResourceResponse& response);

    // cache.delete(request)
    bool remove(const std::string& url);

    // cache.keys()
    std::vector<std::string> keys() const;

    // Size tracking
    size_t size() const;
    size_t byteSize() const;

private:
    std::string name_;
    mutable std::mutex mutex_;
    struct CacheItem {
        std::string url;
        ResourceResponse response;
        double timestamp = 0;
    };
    std::vector<CacheItem> items_;
};

class CacheStorage {
public:
    static CacheStorage& instance();

    // caches.open(name)
    Cache* open(const std::string& name);

    // caches.has(name)
    bool has(const std::string& name) const;

    // caches.delete(name)
    bool remove(const std::string& name);

    // caches.keys()
    std::vector<std::string> keys() const;

    // caches.match(request) — search all caches
    bool match(const ResourceRequest& request, ResourceResponse& out) const;

private:
    CacheStorage() = default;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Cache>> caches_;
};

// ==================================================================
// Text Selection Model (W3C Selection API)
// ==================================================================

struct SelectionPoint {
    void* node = nullptr;   // DOM node (opaque)
    int offset = 0;         // Character offset within text node
    float x = 0, y = 0;    // Screen coordinates
};

enum class SelectionDirection : uint8_t { None, Forward, Backward };

class SelectionRange {
public:
    SelectionRange();

    // Boundary points
    void setStart(void* node, int offset);
    void setEnd(void* node, int offset);

    void* startNode() const { return start_.node; }
    int startOffset() const { return start_.offset; }
    void* endNode() const { return end_.node; }
    int endOffset() const { return end_.offset; }

    // Collapsed = start == end
    bool isCollapsed() const;

    // Text content within the range
    std::string toString() const;

    // Bounding rects
    struct Rect { float x, y, w, h; };
    Rect boundingRect() const { return boundingRect_; }
    void setBoundingRect(const Rect& r) { boundingRect_ = r; }
    std::vector<Rect> clientRects() const { return clientRects_; }
    void addClientRect(const Rect& r) { clientRects_.push_back(r); }

    // Manipulation
    void collapse(bool toStart = true);
    void selectNode(void* node);
    void selectNodeContents(void* node);

    // Comparison
    enum class CompareResult { Before, Equal, After };
    CompareResult compareBoundaryPoints(const SelectionRange& other, bool compareStart) const;

    // Clone
    std::unique_ptr<SelectionRange> cloneRange() const;

private:
    SelectionPoint start_;
    SelectionPoint end_;
    Rect boundingRect_ = {0, 0, 0, 0};
    std::vector<Rect> clientRects_;
};

class Selection {
public:
    static Selection& instance();

    // Current selection
    SelectionRange* getRangeAt(int index) const;
    int rangeCount() const { return ranges_.empty() ? 0 : 1; }

    // Modify
    void addRange(std::unique_ptr<SelectionRange> range);
    void removeRange(int index);
    void removeAllRanges();
    void collapse(void* node, int offset);
    void collapseToStart();
    void collapseToEnd();
    void extend(void* node, int offset);
    void selectAllChildren(void* node);

    // Selection type
    bool isCollapsed() const;
    SelectionDirection direction() const { return direction_; }

    // Anchor and focus
    void* anchorNode() const { return anchor_.node; }
    int anchorOffset() const { return anchor_.offset; }
    void* focusNode() const { return focus_.node; }
    int focusOffset() const { return focus_.offset; }

    // String conversion
    std::string toString() const;

    // Clipboard integration
    void copy();
    void cut();
    void paste(const std::string& text);

    // State change callback
    using SelectionChangeCallback = std::function<void()>;
    void onSelectionChange(SelectionChangeCallback cb) { changeCb_ = cb; }

    // Visual highlight rects for painting
    struct HighlightRect { float x, y, w, h; uint32_t color; };
    std::vector<HighlightRect> highlightRects() const { return highlights_; }
    void setHighlightRects(const std::vector<HighlightRect>& rects) { highlights_ = rects; }

private:
    Selection() = default;
    std::vector<std::unique_ptr<SelectionRange>> ranges_;
    SelectionPoint anchor_;
    SelectionPoint focus_;
    SelectionDirection direction_ = SelectionDirection::None;
    SelectionChangeCallback changeCb_;
    std::vector<HighlightRect> highlights_;
};

// ==================================================================
// Viewport / Meta Tag Handler
// ==================================================================

struct ViewportMeta {
    float width = 0;            // 0 = device-width
    float height = 0;           // 0 = device-height
    float initialScale = 1.0f;
    float minimumScale = 0.1f;
    float maximumScale = 10.0f;
    bool userScalable = true;
    bool widthDeviceWidth = true;
    float viewportFit = 0;      // 0 = auto, 1 = contain, 2 = cover

    // Parse <meta name="viewport" content="...">
    static ViewportMeta parse(const std::string& content);

    // Apply to layout viewport
    struct LayoutViewport {
        float width, height;
        float scale;
    };
    LayoutViewport computeLayout(float deviceWidth, float deviceHeight, float dpr) const;
};

// ==================================================================
// CSS Transitions Integration
// ==================================================================

struct TransitionDefinition {
    std::string property;   // "all", "opacity", "transform", etc.
    float duration = 0;     // seconds
    float delay = 0;        // seconds
    std::string timingFunction = "ease";
};

class TransitionManager {
public:
    static TransitionManager& instance();

    // Parse transition shorthand
    static std::vector<TransitionDefinition> parse(const std::string& transition);

    // Check if a property change should be transitioned
    bool shouldTransition(const std::string& property,
                           const std::vector<TransitionDefinition>& defs) const;

    // Start a transition
    struct ActiveTransition {
        uint64_t id;
        std::string property;
        std::string fromValue;
        std::string toValue;
        float duration;
        float delay;
        float elapsed = 0;
        std::string timingFunction;
        void* targetNode = nullptr; // BoxNode*
    };

    uint64_t startTransition(void* node, const std::string& property,
                                const std::string& from, const std::string& to,
                                const TransitionDefinition& def);

    // Tick transitions (call per frame, returns dirty nodes)
    std::vector<void*> tick(float deltaTime);

    // Cancel transitions on a node
    void cancelTransitions(void* node);
    void cancelTransition(uint64_t id);

    // Query active transitions
    bool hasActiveTransitions(void* node) const;
    const ActiveTransition* getTransition(uint64_t id) const;

    // Interpolation
    static float interpolate(float from, float to, float t, const std::string& timing);
    static uint32_t interpolateColor(uint32_t from, uint32_t to, float t);
    static std::string interpolateTransform(const std::string& from, const std::string& to, float t);

    // Easing functions
    static float easeLinear(float t);
    static float easeIn(float t);
    static float easeOut(float t);
    static float easeInOut(float t);
    static float cubicBezier(float t, float x1, float y1, float x2, float y2);
    static float steps(float t, int stepCount, bool jumpStart);

    // Parse timing function
    static std::function<float(float)> parseTimingFunction(const std::string& value);

private:
    TransitionManager() = default;
    mutable std::mutex mutex_;
    uint64_t nextId_ = 1;
    std::vector<ActiveTransition> active_;
};

// ==================================================================
// IntersectionObserver integration (W3C)
// ==================================================================

struct IntersectionEntry {
    void* target = nullptr;       // BoxNode*
    float intersectionRatio = 0;
    bool isIntersecting = false;
    struct Rect { float x, y, w, h; };
    Rect boundingClientRect = {0, 0, 0, 0};
    Rect intersectionRect = {0, 0, 0, 0};
    Rect rootBounds = {0, 0, 0, 0};
    double time = 0;
};

class IntersectionObserverEngine {
public:
    static IntersectionObserverEngine& instance();

    struct ObserverConfig {
        void* root = nullptr;       // null = viewport
        float rootMarginTop = 0, rootMarginRight = 0;
        float rootMarginBottom = 0, rootMarginLeft = 0;
        std::vector<float> thresholds = {0};
    };

    using IntersectionCallback = std::function<void(const std::vector<IntersectionEntry>&)>;

    uint64_t observe(void* target, const ObserverConfig& config, IntersectionCallback cb);
    void unobserve(uint64_t observerId, void* target);
    void disconnect(uint64_t observerId);

    // Called by frame orchestrator after layout
    void checkIntersections(float viewportX, float viewportY,
                              float viewportW, float viewportH);

private:
    IntersectionObserverEngine() = default;
    mutable std::mutex mutex_;
    uint64_t nextId_ = 1;

    struct ObserverEntry {
        uint64_t id;
        ObserverConfig config;
        IntersectionCallback callback;
        std::vector<void*> targets;
        std::unordered_map<void*, float> lastRatios;
    };
    std::vector<ObserverEntry> observers_;

    float computeIntersection(void* target, const ObserverConfig& config,
                                 float vpX, float vpY, float vpW, float vpH,
                                 IntersectionEntry& entry);
};

// ==================================================================
// Clipboard API (async)
// ==================================================================

class ClipboardAPI {
public:
    static ClipboardAPI& instance();

    // Async read/write
    void readText(std::function<void(const std::string&)> callback);
    void writeText(const std::string& text, std::function<void(bool)> callback = nullptr);

    // Read/write with MIME types
    struct ClipboardItem {
        std::string mimeType;
        std::vector<uint8_t> data;
    };
    void read(std::function<void(const std::vector<ClipboardItem>&)> callback);
    void write(const std::vector<ClipboardItem>& items, std::function<void(bool)> callback = nullptr);

    // Platform bridge (set by platform layer)
    using PlatformRead = std::function<std::string()>;
    using PlatformWrite = std::function<bool(const std::string&)>;
    void setPlatformBridge(PlatformRead readFn, PlatformWrite writeFn);

private:
    ClipboardAPI() = default;
    PlatformRead platformRead_;
    PlatformWrite platformWrite_;
};

} // namespace Web
} // namespace NXRender
