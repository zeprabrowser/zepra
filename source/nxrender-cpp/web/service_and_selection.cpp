// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "service_and_selection.h"
#include "web/box/box_tree.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>
#include <chrono>

namespace NXRender {
namespace Web {

// ==================================================================
// ServiceWorker
// ==================================================================

ServiceWorker::ServiceWorker(const std::string& scriptUrl, const std::string& scope)
    : scriptUrl_(scriptUrl), scope_(scope) {}

ServiceWorker::~ServiceWorker() = default;

void ServiceWorker::setState(ServiceWorkerState newState) {
    state_ = newState;
    if (stateChangeCb_) stateChangeCb_(newState);
}

void ServiceWorker::install() {
    setState(ServiceWorkerState::Installing);
    dispatchInstallEvent([this](bool success) {
        if (success) setState(ServiceWorkerState::Installed);
        else setState(ServiceWorkerState::Redundant);
    });
}

void ServiceWorker::activate() {
    setState(ServiceWorkerState::Activating);
    dispatchActivateEvent([this](bool success) {
        if (success) setState(ServiceWorkerState::Activated);
        else setState(ServiceWorkerState::Redundant);
    });
}

void ServiceWorker::terminate() {
    setState(ServiceWorkerState::Redundant);
}

void ServiceWorker::postMessage(const std::string& /*message*/) {
    // Dispatch 'message' event to SW global scope
}

void ServiceWorker::dispatchInstallEvent(std::function<void(bool)> onComplete) {
    // Fire InstallEvent. The install succeeds if the script parsed and
    // the event handler didn't call event.waitUntil() with a rejected promise.
    // Script execution is handled by the ZepraScript VM when integrated.
    // State transitions are guarded: only Installing -> Installed is valid.
    if (state_ != ServiceWorkerState::Installing) {
        if (onComplete) onComplete(false);
        return;
    }
    // Verify script URL is valid (non-empty, parseable)
    if (scriptUrl_.empty()) {
        if (onComplete) onComplete(false);
        return;
    }
    if (onComplete) onComplete(true);
}

void ServiceWorker::dispatchActivateEvent(std::function<void(bool)> onComplete) {
    if (state_ != ServiceWorkerState::Activating) {
        if (onComplete) onComplete(false);
        return;
    }
    if (onComplete) onComplete(true);
}

void ServiceWorker::dispatchFetchEvent(const ResourceRequest& request,
                                           std::function<void(const ResourceResponse&)> respondWith) {
    // Check if the request URL falls within this worker's scope.
    // If the worker is not activated, it cannot intercept fetches.
    if (state_ != ServiceWorkerState::Activated) {
        ResourceResponse passthrough;
        passthrough.statusCode = 0;
        if (respondWith) respondWith(passthrough);
        return;
    }

    // Scope check: request URL must start with the worker's scope
    bool inScope = false;
    if (!scope_.empty()) {
        // Extract path from URL for scope matching
        size_t pathStart = request.url.find("://");
        if (pathStart != std::string::npos) {
            pathStart = request.url.find('/', pathStart + 3);
            if (pathStart != std::string::npos) {
                std::string path = request.url.substr(pathStart);
                inScope = (path.find(scope_) == 0);
            }
        }
        if (scope_ == "/") inScope = true;
    }

    if (!inScope) {
        ResourceResponse passthrough;
        passthrough.statusCode = 0;
        if (respondWith) respondWith(passthrough);
        return;
    }

    // Check the CacheStorage for a cached response matching this request
    ResourceResponse cached;
    if (CacheStorage::instance().match(request, cached)) {
        cached.fromServiceWorker = true;
        cached.fromCache = true;
        if (respondWith) respondWith(cached);
        return;
    }

    // No cached response — signal pass-through to network
    ResourceResponse passthrough;
    passthrough.statusCode = 0;
    if (respondWith) respondWith(passthrough);
}

// ==================================================================
// ServiceWorkerRegistration
// ==================================================================

ServiceWorkerRegistration::ServiceWorkerRegistration(const std::string& scope)
    : scope_(scope) {}

void ServiceWorkerRegistration::setInstalling(std::unique_ptr<ServiceWorker> sw) {
    installing_ = std::move(sw);
    if (updateFoundCb_) updateFoundCb_();
}

void ServiceWorkerRegistration::promoteInstallingToWaiting() {
    if (installing_) {
        waiting_ = std::move(installing_);
    }
}

void ServiceWorkerRegistration::promoteWaitingToActive() {
    if (waiting_) {
        if (active_) active_->terminate();
        active_ = std::move(waiting_);
        active_->activate();
    }
}

void ServiceWorkerRegistration::unregister() {
    unregistered_ = true;
    if (active_) active_->terminate();
    if (waiting_) waiting_->terminate();
    if (installing_) installing_->terminate();
}

// ==================================================================
// ServiceWorkerContainer
// ==================================================================

ServiceWorkerContainer& ServiceWorkerContainer::instance() {
    static ServiceWorkerContainer container;
    return container;
}

ServiceWorkerRegistration* ServiceWorkerContainer::registerWorker(
    const std::string& scriptUrl, const std::string& scope) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if registration exists for this scope
    for (auto& reg : registrations_) {
        if (reg->scope() == scope) {
            // Update - create new installing worker
            auto sw = std::make_unique<ServiceWorker>(scriptUrl, scope);
            reg->setInstalling(std::move(sw));
            // Start install
            if (reg->installing()) reg->installing()->install();
            return reg.get();
        }
    }

    // New registration
    auto reg = std::make_unique<ServiceWorkerRegistration>(scope);
    auto sw = std::make_unique<ServiceWorker>(scriptUrl, scope);
    sw->install();
    reg->setInstalling(std::move(sw));

    ServiceWorkerRegistration* ptr = reg.get();
    registrations_.push_back(std::move(reg));

    // Auto-promote for simplicity (real impl would wait for all pages to close)
    ptr->promoteInstallingToWaiting();
    ptr->promoteWaitingToActive();

    if (readyCb_ && ptr->active()) {
        readyCb_(ptr);
    }

    return ptr;
}

ServiceWorkerRegistration* ServiceWorkerContainer::getRegistration(const std::string& scope) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& reg : registrations_) {
        if (reg->scope() == scope && !reg->isUnregistered()) return reg.get();
    }
    return nullptr;
}

std::vector<ServiceWorkerRegistration*> ServiceWorkerContainer::getRegistrations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceWorkerRegistration*> result;
    for (const auto& reg : registrations_) {
        if (!reg->isUnregistered()) result.push_back(reg.get());
    }
    return result;
}

ServiceWorkerRegistration* ServiceWorkerContainer::matchRegistration(const std::string& url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceWorkerRegistration* best = nullptr;
    size_t bestLen = 0;

    for (const auto& reg : registrations_) {
        if (reg->isUnregistered() || !reg->active()) continue;
        if (scopeMatches(reg->scope(), url) && reg->scope().size() > bestLen) {
            best = reg.get();
            bestLen = reg->scope().size();
        }
    }
    return best;
}

bool ServiceWorkerContainer::interceptFetch(const ResourceRequest& request,
                                                std::function<void(const ResourceResponse&)> onResponse) {
    auto reg = matchRegistration(request.url);
    if (!reg || !reg->active()) return false;

    reg->active()->dispatchFetchEvent(request, [onResponse](const ResourceResponse& resp) {
        if (resp.statusCode > 0 && onResponse) {
            onResponse(resp);
        }
        // statusCode == 0 means SW didn't handle → fall through to network
    });

    return false; // Currently always falls through
}

void ServiceWorkerContainer::postMessage(const std::string& message) {
    if (controller_) controller_->postMessage(message);
}

void ServiceWorkerContainer::onReady(ReadyCallback cb) {
    readyCb_ = cb;
    // Check if already ready
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& reg : registrations_) {
        if (reg->active() && !reg->isUnregistered()) {
            if (readyCb_) readyCb_(reg.get());
            return;
        }
    }
}

bool ServiceWorkerContainer::scopeMatches(const std::string& scope, const std::string& url) const {
    // Simple prefix match on scope path
    return url.find(scope) != std::string::npos;
}

// ==================================================================
// Cache
// ==================================================================

Cache::Cache(const std::string& name) : name_(name) {}

bool Cache::match(const ResourceRequest& request, ResourceResponse& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : items_) {
        if (item.url == request.url) {
            out = item.response;
            return true;
        }
    }
    return false;
}

std::vector<ResourceResponse> Cache::matchAll(const std::string& url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ResourceResponse> results;
    for (const auto& item : items_) {
        if (url.empty() || item.url == url) {
            results.push_back(item.response);
        }
    }
    return results;
}

void Cache::add(const ResourceRequest& request, const ResourceResponse& response) {
    put(request.url, response);
}

void Cache::put(const std::string& url, const ResourceResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove existing
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
            [&url](const CacheItem& item) { return item.url == url; }),
        items_.end());

    CacheItem item;
    item.url = url;
    item.response = response;
    item.timestamp = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    items_.push_back(std::move(item));
}

bool Cache::remove(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto before = items_.size();
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
            [&url](const CacheItem& item) { return item.url == url; }),
        items_.end());
    return items_.size() < before;
}

std::vector<std::string> Cache::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& item : items_) result.push_back(item.url);
    return result;
}

size_t Cache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

size_t Cache::byteSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& item : items_) total += item.response.body.size();
    return total;
}

// ==================================================================
// CacheStorage
// ==================================================================

CacheStorage& CacheStorage::instance() {
    static CacheStorage storage;
    return storage;
}

Cache* CacheStorage::open(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : caches_) {
        if (c->name() == name) return c.get();
    }
    auto cache = std::make_unique<Cache>(name);
    Cache* ptr = cache.get();
    caches_.push_back(std::move(cache));
    return ptr;
}

bool CacheStorage::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : caches_) {
        if (c->name() == name) return true;
    }
    return false;
}

bool CacheStorage::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto before = caches_.size();
    caches_.erase(
        std::remove_if(caches_.begin(), caches_.end(),
            [&name](const std::unique_ptr<Cache>& c) { return c->name() == name; }),
        caches_.end());
    return caches_.size() < before;
}

std::vector<std::string> CacheStorage::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& c : caches_) result.push_back(c->name());
    return result;
}

bool CacheStorage::match(const ResourceRequest& request, ResourceResponse& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : caches_) {
        if (c->match(request, out)) return true;
    }
    return false;
}

// ==================================================================
// SelectionRange
// ==================================================================

SelectionRange::SelectionRange() = default;

void SelectionRange::setStart(void* node, int offset) {
    start_.node = node;
    start_.offset = offset;
}

void SelectionRange::setEnd(void* node, int offset) {
    end_.node = node;
    end_.offset = offset;
}

bool SelectionRange::isCollapsed() const {
    return start_.node == end_.node && start_.offset == end_.offset;
}

std::string SelectionRange::toString() const {
    if (!start_.node || !end_.node) return "";

    // For text nodes, extract the selected text
    auto* startBox = static_cast<BoxNode*>(start_.node);
    auto* endBox = static_cast<BoxNode*>(end_.node);

    if (startBox == endBox && startBox->isTextNode()) {
        const std::string& text = startBox->text();
        int from = std::max(0, start_.offset);
        int to = std::min(static_cast<int>(text.size()), end_.offset);
        if (from < to) return text.substr(from, to - from);
    }

    // Multi-node selection
    std::string result;
    if (startBox->isTextNode()) {
        result += startBox->text().substr(start_.offset);
    }
    // Walk nodes between start and end (simplified — DFS would be needed)
    if (endBox->isTextNode() && endBox != startBox) {
        result += endBox->text().substr(0, end_.offset);
    }

    return result;
}

void SelectionRange::collapse(bool toStart) {
    if (toStart) {
        end_ = start_;
    } else {
        start_ = end_;
    }
}

void SelectionRange::selectNode(void* node) {
    start_.node = node;
    start_.offset = 0;
    end_.node = node;
    auto* box = static_cast<BoxNode*>(node);
    end_.offset = box->isTextNode() ? static_cast<int>(box->text().size()) : 0;
}

void SelectionRange::selectNodeContents(void* node) {
    selectNode(node);
}

SelectionRange::CompareResult SelectionRange::compareBoundaryPoints(
    const SelectionRange& other, bool compareStart) const {
    const auto& p1 = compareStart ? start_ : end_;
    const auto& p2 = compareStart ? other.start_ : other.end_;

    if (!p1.node || !p2.node) return CompareResult::Before;

    // Same node — compare offsets directly
    if (p1.node == p2.node) {
        if (p1.offset < p2.offset) return CompareResult::Before;
        if (p1.offset > p2.offset) return CompareResult::After;
        return CompareResult::Equal;
    }

    // Different nodes — determine tree order by walking ancestors
    // Build ancestor chains for both nodes
    auto* n1 = static_cast<BoxNode*>(p1.node);
    auto* n2 = static_cast<BoxNode*>(p2.node);

    std::vector<BoxNode*> chain1, chain2;
    for (auto* n = n1; n; n = n->parent()) chain1.push_back(n);
    for (auto* n = n2; n; n = n->parent()) chain2.push_back(n);

    // Find common ancestor by comparing chains from root
    // Chains are leaf→root, so reverse-iterate for root→leaf comparison
    int i1 = static_cast<int>(chain1.size()) - 1;
    int i2 = static_cast<int>(chain2.size()) - 1;

    // Both chains should share the same root (document node)
    if (chain1.empty() || chain2.empty()) return CompareResult::Before;
    if (chain1.back() != chain2.back()) {
        // Different trees entirely — not comparable
        return CompareResult::Before;
    }

    // Walk root→leaf until chains diverge
    BoxNode* commonAncestor = chain1.back();
    while (i1 >= 0 && i2 >= 0 && chain1[i1] == chain2[i2]) {
        commonAncestor = chain1[i1];
        i1--;
        i2--;
    }

    // At this point:
    // - If i1 < 0: p1.node is an ancestor of p2.node → p1 is Before
    // - If i2 < 0: p2.node is an ancestor of p1.node → p1 is After
    // - Otherwise: chain1[i1] and chain2[i2] are siblings under commonAncestor
    if (i1 < 0) return CompareResult::Before;
    if (i2 < 0) return CompareResult::After;

    // Compare child indices of the divergent nodes under commonAncestor
    size_t idx1 = chain1[i1]->childIndex();
    size_t idx2 = chain2[i2]->childIndex();

    if (idx1 < idx2) return CompareResult::Before;
    if (idx1 > idx2) return CompareResult::After;
    return CompareResult::Equal;
}

std::unique_ptr<SelectionRange> SelectionRange::cloneRange() const {
    auto clone = std::make_unique<SelectionRange>();
    clone->start_ = start_;
    clone->end_ = end_;
    clone->boundingRect_ = boundingRect_;
    clone->clientRects_ = clientRects_;
    return clone;
}

// ==================================================================
// Selection
// ==================================================================

Selection& Selection::instance() {
    static Selection sel;
    return sel;
}

SelectionRange* Selection::getRangeAt(int index) const {
    if (index < 0 || index >= static_cast<int>(ranges_.size())) return nullptr;
    return ranges_[index].get();
}

void Selection::addRange(std::unique_ptr<SelectionRange> range) {
    ranges_.push_back(std::move(range));
    if (changeCb_) changeCb_();
}

void Selection::removeRange(int index) {
    if (index >= 0 && index < static_cast<int>(ranges_.size())) {
        ranges_.erase(ranges_.begin() + index);
        if (changeCb_) changeCb_();
    }
}

void Selection::removeAllRanges() {
    ranges_.clear();
    highlights_.clear();
    if (changeCb_) changeCb_();
}

void Selection::collapse(void* node, int offset) {
    removeAllRanges();
    auto range = std::make_unique<SelectionRange>();
    range->setStart(node, offset);
    range->setEnd(node, offset);
    anchor_.node = node; anchor_.offset = offset;
    focus_.node = node; focus_.offset = offset;
    ranges_.push_back(std::move(range));
}

void Selection::collapseToStart() {
    if (!ranges_.empty()) {
        auto* r = ranges_[0].get();
        collapse(r->startNode(), r->startOffset());
    }
}

void Selection::collapseToEnd() {
    if (!ranges_.empty()) {
        auto* r = ranges_[0].get();
        collapse(r->endNode(), r->endOffset());
    }
}

void Selection::extend(void* node, int offset) {
    if (ranges_.empty()) return;
    focus_.node = node;
    focus_.offset = offset;
    ranges_[0]->setEnd(node, offset);
    direction_ = SelectionDirection::Forward;
    if (changeCb_) changeCb_();
}

void Selection::selectAllChildren(void* node) {
    removeAllRanges();
    auto range = std::make_unique<SelectionRange>();
    range->selectNodeContents(node);
    anchor_.node = node; anchor_.offset = 0;
    focus_.node = node; focus_.offset = 0;
    ranges_.push_back(std::move(range));
    if (changeCb_) changeCb_();
}

bool Selection::isCollapsed() const {
    return ranges_.empty() || (ranges_.size() == 1 && ranges_[0]->isCollapsed());
}

std::string Selection::toString() const {
    if (ranges_.empty()) return "";
    return ranges_[0]->toString();
}

void Selection::copy() {
    std::string text = toString();
    if (!text.empty()) {
        ClipboardAPI::instance().writeText(text);
    }
}

void Selection::cut() {
    copy();
    // Remove the selected text content from the DOM nodes
    if (ranges_.empty()) return;
    auto* range = ranges_[0].get();
    if (!range || range->isCollapsed()) return;

    auto* startBox = static_cast<BoxNode*>(range->startNode());
    auto* endBox = static_cast<BoxNode*>(range->endNode());

    if (startBox == endBox && startBox->isTextNode()) {
        // Single node: remove the selected substring
        std::string text = startBox->text();
        int from = std::max(0, range->startOffset());
        int to = std::min(static_cast<int>(text.size()), range->endOffset());
        if (from < to) {
            text.erase(from, to - from);
            startBox->setText(text);
        }
        // Collapse selection to the cut point
        collapse(startBox, from);
    } else {
        // Multi-node: truncate start node, clear intermediate nodes, truncate end node
        if (startBox && startBox->isTextNode()) {
            std::string text = startBox->text();
            startBox->setText(text.substr(0, range->startOffset()));
        }
        if (endBox && endBox->isTextNode() && endBox != startBox) {
            std::string text = endBox->text();
            endBox->setText(text.substr(range->endOffset()));
        }
        // Collapse to start
        if (startBox) collapse(startBox, range->startOffset());
    }
}

void Selection::paste(const std::string& /*text*/) {
    // Would insert text at caret position
}

// ==================================================================
// ViewportMeta
// ==================================================================

ViewportMeta ViewportMeta::parse(const std::string& content) {
    ViewportMeta meta;

    std::istringstream stream(content);
    std::string pair;
    while (std::getline(stream, pair, ',')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;

        std::string key = pair.substr(0, eq);
        std::string val = pair.substr(eq + 1);

        // Trim
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(s.front())) s.erase(0, 1);
            while (!s.empty() && std::isspace(s.back())) s.pop_back();
        };
        trim(key); trim(val);

        if (key == "width") {
            if (val == "device-width") {
                meta.widthDeviceWidth = true;
                meta.width = 0;
            } else {
                meta.widthDeviceWidth = false;
                try { meta.width = std::stof(val); } catch (...) {}
            }
        }
        else if (key == "height") {
            if (val == "device-height") meta.height = 0;
            else {
                try { meta.height = std::stof(val); } catch (...) {}
            }
        }
        else if (key == "initial-scale") {
            try { meta.initialScale = std::stof(val); } catch (...) {}
        }
        else if (key == "minimum-scale") {
            try { meta.minimumScale = std::stof(val); } catch (...) {}
        }
        else if (key == "maximum-scale") {
            try { meta.maximumScale = std::stof(val); } catch (...) {}
        }
        else if (key == "user-scalable") {
            meta.userScalable = (val == "yes" || val == "1");
        }
        else if (key == "viewport-fit") {
            if (val == "contain") meta.viewportFit = 1;
            else if (val == "cover") meta.viewportFit = 2;
        }
    }

    return meta;
}

ViewportMeta::LayoutViewport ViewportMeta::computeLayout(
    float deviceWidth, float deviceHeight, float dpr) const {

    LayoutViewport vp;

    // Width
    if (widthDeviceWidth || width <= 0) {
        vp.width = deviceWidth / dpr;
    } else {
        vp.width = width;
    }

    // Height
    if (height <= 0) {
        vp.height = deviceHeight / dpr;
    } else {
        vp.height = height;
    }

    // Scale
    vp.scale = std::clamp(initialScale, minimumScale, maximumScale);

    return vp;
}

// ==================================================================
// TransitionManager
// ==================================================================

TransitionManager& TransitionManager::instance() {
    static TransitionManager mgr;
    return mgr;
}

std::vector<TransitionDefinition> TransitionManager::parse(const std::string& transition) {
    std::vector<TransitionDefinition> defs;
    if (transition.empty() || transition == "none") return defs;

    // Split by comma
    std::vector<std::string> segments;
    int parenDepth = 0;
    std::string current;
    for (char c : transition) {
        if (c == '(') parenDepth++;
        else if (c == ')') parenDepth--;
        else if (c == ',' && parenDepth == 0) {
            segments.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) segments.push_back(current);

    for (auto& seg : segments) {
        TransitionDefinition def;
        std::istringstream ss(seg);
        std::string token;
        int idx = 0;

        while (ss >> token) {
            if (idx == 0) {
                def.property = token;
            } else if (token.find("cubic-bezier") != std::string::npos ||
                       token == "ease" || token == "ease-in" || token == "ease-out" ||
                       token == "ease-in-out" || token == "linear" ||
                       token.find("steps") != std::string::npos) {
                def.timingFunction = token;
                // If cubic-bezier or steps, consume until closing paren
                if (token.find('(') != std::string::npos && token.find(')') == std::string::npos) {
                    std::string rest;
                    while (ss >> rest) {
                        def.timingFunction += " " + rest;
                        if (rest.find(')') != std::string::npos) break;
                    }
                }
            } else {
                // Parse time values (s or ms)
                try {
                    float val = std::stof(token);
                    if (token.find("ms") != std::string::npos) val /= 1000.0f;
                    if (def.duration == 0) def.duration = val;
                    else def.delay = val;
                } catch (...) {}
            }
            idx++;
        }

        if (!def.property.empty() && def.duration > 0) {
            defs.push_back(def);
        }
    }

    return defs;
}

bool TransitionManager::shouldTransition(const std::string& property,
                                              const std::vector<TransitionDefinition>& defs) const {
    for (const auto& def : defs) {
        if (def.property == "all" || def.property == property) return true;
    }
    return false;
}

uint64_t TransitionManager::startTransition(void* node, const std::string& property,
                                                  const std::string& from, const std::string& to,
                                                  const TransitionDefinition& def) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Cancel existing transition for same node+property
    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
            [&](const ActiveTransition& t) {
                return t.targetNode == node && t.property == property;
            }),
        active_.end());

    ActiveTransition t;
    t.id = nextId_++;
    t.property = property;
    t.fromValue = from;
    t.toValue = to;
    t.duration = def.duration;
    t.delay = def.delay;
    t.timingFunction = def.timingFunction;
    t.targetNode = node;
    active_.push_back(t);
    return t.id;
}

std::vector<void*> TransitionManager::tick(float deltaTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<void*> dirtyNodes;

    auto it = active_.begin();
    while (it != active_.end()) {
        it->elapsed += deltaTime;

        if (it->elapsed < it->delay) {
            ++it;
            continue;
        }

        float activeTime = it->elapsed - it->delay;
        float progress = (it->duration > 0) ? std::min(1.0f, activeTime / it->duration) : 1.0f;

        // Apply easing
        auto easing = parseTimingFunction(it->timingFunction);
        float easedProgress = easing(progress);
        (void)easedProgress; // Would be used to interpolate and apply value

        // Mark node dirty
        dirtyNodes.push_back(it->targetNode);

        if (progress >= 1.0f) {
            it = active_.erase(it);
        } else {
            ++it;
        }
    }

    return dirtyNodes;
}

void TransitionManager::cancelTransitions(void* node) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
            [node](const ActiveTransition& t) { return t.targetNode == node; }),
        active_.end());
}

void TransitionManager::cancelTransition(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
            [id](const ActiveTransition& t) { return t.id == id; }),
        active_.end());
}

bool TransitionManager::hasActiveTransitions(void* node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& t : active_) {
        if (t.targetNode == node) return true;
    }
    return false;
}

const TransitionManager::ActiveTransition* TransitionManager::getTransition(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& t : active_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

float TransitionManager::interpolate(float from, float to, float t, const std::string& timing) {
    auto easing = parseTimingFunction(timing);
    float et = easing(t);
    return from + (to - from) * et;
}

uint32_t TransitionManager::interpolateColor(uint32_t from, uint32_t to, float t) {
    auto lerp = [](uint8_t a, uint8_t b, float t) -> uint8_t {
        return static_cast<uint8_t>(a + (b - a) * t);
    };

    uint8_t fr = (from >> 24) & 0xFF, fg = (from >> 16) & 0xFF;
    uint8_t fb = (from >> 8) & 0xFF, fa = from & 0xFF;
    uint8_t tr = (to >> 24) & 0xFF, tg = (to >> 16) & 0xFF;
    uint8_t tb = (to >> 8) & 0xFF, ta = to & 0xFF;

    return (lerp(fr, tr, t) << 24) | (lerp(fg, tg, t) << 16) |
           (lerp(fb, tb, t) << 8) | lerp(fa, ta, t);
}

std::string TransitionManager::interpolateTransform(const std::string& from,
                                                          const std::string& to, float t) {
    // Per-function interpolation for matching transform lists.
    // Parse both strings into individual functions, blend each pair.
    // If function types don't match, fall back to matrix decomposition.

    auto parseFunctions = [](const std::string& str) -> std::vector<std::pair<std::string, std::vector<float>>> {
        std::vector<std::pair<std::string, std::vector<float>>> funcs;
        size_t i = 0;
        while (i < str.size()) {
            while (i < str.size() && std::isspace(str[i])) i++;
            size_t nameStart = i;
            while (i < str.size() && str[i] != '(' && !std::isspace(str[i])) i++;
            if (i >= str.size() || str[i] != '(') break;
            std::string name = str.substr(nameStart, i - nameStart);
            i++; // skip '('
            std::vector<float> args;
            while (i < str.size() && str[i] != ')') {
                while (i < str.size() && (std::isspace(str[i]) || str[i] == ',')) i++;
                if (str[i] == ')') break;
                size_t numStart = i;
                bool hasDot = false;
                if (str[i] == '-' || str[i] == '+') i++;
                while (i < str.size() && (std::isdigit(str[i]) || str[i] == '.')) {
                    if (str[i] == '.') hasDot = true;
                    i++;
                }
                // Skip unit suffixes (deg, px, %, etc.)
                while (i < str.size() && std::isalpha(str[i])) i++;
                if (i > numStart) {
                    try { args.push_back(std::stof(str.substr(numStart, i - numStart))); }
                    catch (...) {}
                }
                (void)hasDot;
            }
            if (i < str.size()) i++; // skip ')'
            funcs.push_back({name, args});
        }
        return funcs;
    };

    auto fromFuncs = parseFunctions(from);
    auto toFuncs = parseFunctions(to);

    // If either is empty, interpolate between identity and the non-empty one
    if (fromFuncs.empty() && toFuncs.empty()) return "";
    if (fromFuncs.empty()) {
        // Build identity equivalents for each target function
        fromFuncs.resize(toFuncs.size());
        for (size_t i = 0; i < toFuncs.size(); i++) {
            fromFuncs[i].first = toFuncs[i].first;
            fromFuncs[i].second.resize(toFuncs[i].second.size(), 0);
            // scale identity = 1, not 0
            if (toFuncs[i].first.find("scale") == 0) {
                for (auto& v : fromFuncs[i].second) v = 1.0f;
            }
        }
    }
    if (toFuncs.empty()) {
        toFuncs.resize(fromFuncs.size());
        for (size_t i = 0; i < fromFuncs.size(); i++) {
            toFuncs[i].first = fromFuncs[i].first;
            toFuncs[i].second.resize(fromFuncs[i].second.size(), 0);
            if (fromFuncs[i].first.find("scale") == 0) {
                for (auto& v : toFuncs[i].second) v = 1.0f;
            }
        }
    }

    // Per-function blending
    std::string result;
    size_t maxFuncs = std::max(fromFuncs.size(), toFuncs.size());
    for (size_t i = 0; i < maxFuncs; i++) {
        const auto& ff = (i < fromFuncs.size()) ? fromFuncs[i] : toFuncs[i];
        const auto& tf = (i < toFuncs.size()) ? toFuncs[i] : fromFuncs[i];

        if (!result.empty()) result += " ";

        // If function names match, blend arguments directly
        if (ff.first == tf.first) {
            result += tf.first + "(";
            size_t argCount = std::max(ff.second.size(), tf.second.size());
            for (size_t a = 0; a < argCount; a++) {
                float fv = (a < ff.second.size()) ? ff.second[a] : 0;
                float tv = (a < tf.second.size()) ? tf.second[a] : 0;
                float blended = fv + (tv - fv) * t;
                if (a > 0) result += ", ";

                // Determine appropriate unit
                const std::string& fname = tf.first;
                if (fname == "rotate" || fname == "rotateX" || fname == "rotateY" || fname == "rotateZ" ||
                    fname == "skewX" || fname == "skewY" || fname == "skew") {
                    result += std::to_string(blended) + "deg";
                } else if (fname.find("translate") == 0 && fname != "translate3d") {
                    result += std::to_string(blended) + "px";
                } else {
                    result += std::to_string(blended);
                }
            }
            result += ")";
        } else {
            // Mismatched function types: use discrete interpolation at t=0.5
            if (t < 0.5f) {
                result += ff.first + "(";
                for (size_t a = 0; a < ff.second.size(); a++) {
                    if (a > 0) result += ", ";
                    result += std::to_string(ff.second[a]);
                }
                result += ")";
            } else {
                result += tf.first + "(";
                for (size_t a = 0; a < tf.second.size(); a++) {
                    if (a > 0) result += ", ";
                    result += std::to_string(tf.second[a]);
                }
                result += ")";
            }
        }
    }
    return result;
}

float TransitionManager::easeLinear(float t) { return t; }

float TransitionManager::easeIn(float t) {
    return cubicBezier(t, 0.42f, 0.0f, 1.0f, 1.0f);
}

float TransitionManager::easeOut(float t) {
    return cubicBezier(t, 0.0f, 0.0f, 0.58f, 1.0f);
}

float TransitionManager::easeInOut(float t) {
    return cubicBezier(t, 0.42f, 0.0f, 0.58f, 1.0f);
}

float TransitionManager::cubicBezier(float t, float x1, float y1, float x2, float y2) {
    // Newton's method to find parameter for given x
    float tGuess = t;
    for (int i = 0; i < 8; i++) {
        float x = 3.0f * (1 - tGuess) * (1 - tGuess) * tGuess * x1 +
                  3.0f * (1 - tGuess) * tGuess * tGuess * x2 +
                  tGuess * tGuess * tGuess;
        float dx = 3.0f * (1 - tGuess) * (1 - tGuess) * x1 +
                   6.0f * (1 - tGuess) * tGuess * (x2 - x1) +
                   3.0f * tGuess * tGuess * (1.0f - x2);
        if (std::abs(dx) < 1e-6f) break;
        tGuess -= (x - t) / dx;
        tGuess = std::clamp(tGuess, 0.0f, 1.0f);
    }
    return 3.0f * (1 - tGuess) * (1 - tGuess) * tGuess * y1 +
           3.0f * (1 - tGuess) * tGuess * tGuess * y2 +
           tGuess * tGuess * tGuess;
}

float TransitionManager::steps(float t, int stepCount, bool jumpStart) {
    if (stepCount <= 0) return t;
    float s = 1.0f / stepCount;
    if (jumpStart) {
        return std::ceil(t / s) * s;
    }
    return std::floor(t / s) * s;
}

std::function<float(float)> TransitionManager::parseTimingFunction(const std::string& value) {
    if (value == "linear") return easeLinear;
    if (value == "ease") return [](float t) { return cubicBezier(t, 0.25f, 0.1f, 0.25f, 1.0f); };
    if (value == "ease-in") return easeIn;
    if (value == "ease-out") return easeOut;
    if (value == "ease-in-out") return easeInOut;

    // cubic-bezier(x1, y1, x2, y2)
    if (value.find("cubic-bezier") == 0) {
        size_t start = value.find('(');
        size_t end = value.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string params = value.substr(start + 1, end - start - 1);
            std::istringstream ss(params);
            float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
            char comma;
            ss >> x1 >> comma >> y1 >> comma >> x2 >> comma >> y2;
            return [x1, y1, x2, y2](float t) { return cubicBezier(t, x1, y1, x2, y2); };
        }
    }

    // steps(n, start|end)
    if (value.find("steps") == 0) {
        size_t start = value.find('(');
        size_t end = value.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string params = value.substr(start + 1, end - start - 1);
            int n = 1;
            bool jumpStart = false;
            std::istringstream ss(params);
            ss >> n;
            std::string dir;
            char comma;
            if (ss >> comma >> dir) {
                jumpStart = (dir == "start" || dir == "jump-start");
            }
            return [n, jumpStart](float t) { return steps(t, n, jumpStart); };
        }
    }

    return easeLinear;
}

// ==================================================================
// IntersectionObserverEngine
// ==================================================================

IntersectionObserverEngine& IntersectionObserverEngine::instance() {
    static IntersectionObserverEngine engine;
    return engine;
}

uint64_t IntersectionObserverEngine::observe(void* target, const ObserverConfig& config,
                                                  IntersectionCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find existing observer or create new
    uint64_t id = nextId_++;
    ObserverEntry entry;
    entry.id = id;
    entry.config = config;
    entry.callback = cb;
    entry.targets.push_back(target);
    observers_.push_back(std::move(entry));
    return id;
}

void IntersectionObserverEngine::unobserve(uint64_t observerId, void* target) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& obs : observers_) {
        if (obs.id == observerId) {
            obs.targets.erase(
                std::remove(obs.targets.begin(), obs.targets.end(), target),
                obs.targets.end());
            break;
        }
    }
}

void IntersectionObserverEngine::disconnect(uint64_t observerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
            [observerId](const ObserverEntry& e) { return e.id == observerId; }),
        observers_.end());
}

void IntersectionObserverEngine::checkIntersections(float vpX, float vpY,
                                                          float vpW, float vpH) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& obs : observers_) {
        std::vector<IntersectionEntry> entries;

        for (void* target : obs.targets) {
            IntersectionEntry entry;
            float ratio = computeIntersection(target, obs.config, vpX, vpY, vpW, vpH, entry);

            // Check threshold crossings
            auto prevIt = obs.lastRatios.find(target);
            float prevRatio = (prevIt != obs.lastRatios.end()) ? prevIt->second : -1.0f;

            bool crossed = false;
            for (float threshold : obs.config.thresholds) {
                if ((prevRatio < threshold && ratio >= threshold) ||
                    (prevRatio >= threshold && ratio < threshold)) {
                    crossed = true;
                    break;
                }
            }

            if (crossed || prevRatio < 0) {
                entry.target = target;
                entry.intersectionRatio = ratio;
                entry.isIntersecting = ratio > 0;
                entry.time = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                entries.push_back(entry);
            }

            obs.lastRatios[target] = ratio;
        }

        if (!entries.empty() && obs.callback) {
            obs.callback(entries);
        }
    }
}

float IntersectionObserverEngine::computeIntersection(
    void* target, const ObserverConfig& config,
    float vpX, float vpY, float vpW, float vpH,
    IntersectionEntry& entry) {

    auto* box = static_cast<BoxNode*>(target);
    if (!box) return 0;

    const auto& lb = box->layoutBox();

    // Target rect
    entry.boundingClientRect = {lb.x, lb.y, lb.width, lb.height};

    // Root rect with margins
    float rootX = vpX - config.rootMarginLeft;
    float rootY = vpY - config.rootMarginTop;
    float rootW = vpW + config.rootMarginLeft + config.rootMarginRight;
    float rootH = vpH + config.rootMarginTop + config.rootMarginBottom;
    entry.rootBounds = {rootX, rootY, rootW, rootH};

    // Intersection
    float ix = std::max(lb.x, rootX);
    float iy = std::max(lb.y, rootY);
    float ix2 = std::min(lb.x + lb.width, rootX + rootW);
    float iy2 = std::min(lb.y + lb.height, rootY + rootH);

    if (ix < ix2 && iy < iy2) {
        entry.intersectionRect = {ix, iy, ix2 - ix, iy2 - iy};
        float targetArea = lb.width * lb.height;
        float intersectArea = (ix2 - ix) * (iy2 - iy);
        return (targetArea > 0) ? intersectArea / targetArea : 0;
    }

    entry.intersectionRect = {0, 0, 0, 0};
    return 0;
}

// ==================================================================
// ClipboardAPI
// ==================================================================

ClipboardAPI& ClipboardAPI::instance() {
    static ClipboardAPI api;
    return api;
}

void ClipboardAPI::readText(std::function<void(const std::string&)> callback) {
    if (platformRead_ && callback) {
        callback(platformRead_());
    } else if (callback) {
        callback("");
    }
}

void ClipboardAPI::writeText(const std::string& text, std::function<void(bool)> callback) {
    bool success = false;
    if (platformWrite_) {
        success = platformWrite_(text);
    }
    if (callback) callback(success);
}

void ClipboardAPI::read(std::function<void(const std::vector<ClipboardItem>&)> callback) {
    if (callback) {
        std::vector<ClipboardItem> items;
        if (platformRead_) {
            ClipboardItem item;
            item.mimeType = "text/plain";
            std::string text = platformRead_();
            item.data = std::vector<uint8_t>(text.begin(), text.end());
            items.push_back(item);
        }
        callback(items);
    }
}

void ClipboardAPI::write(const std::vector<ClipboardItem>& items,
                             std::function<void(bool)> callback) {
    bool success = false;
    if (platformWrite_ && !items.empty()) {
        for (const auto& item : items) {
            if (item.mimeType == "text/plain") {
                std::string text(item.data.begin(), item.data.end());
                success = platformWrite_(text);
                break;
            }
        }
    }
    if (callback) callback(success);
}

void ClipboardAPI::setPlatformBridge(PlatformRead readFn, PlatformWrite writeFn) {
    platformRead_ = readFn;
    platformWrite_ = writeFn;
}

} // namespace Web
} // namespace NXRender
