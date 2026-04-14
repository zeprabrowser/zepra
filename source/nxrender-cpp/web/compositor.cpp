// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "compositor.h"
#include "web/input.h"
#include "web/paint/paint_ops.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>
#include <chrono>

namespace NXRender {
namespace Web {

// ==================================================================
// CSSOMQuery
// ==================================================================

CSSOMQuery::DOMRect CSSOMQuery::getBoundingClientRect(const BoxNode* node, float scrollX, float scrollY) {
    if (!node) return {};
    const auto& lb = node->layoutBox();
    return {lb.x - scrollX, lb.y - scrollY, lb.width, lb.height};
}

std::vector<CSSOMQuery::DOMRect> CSSOMQuery::getClientRects(const BoxNode* node, float scrollX, float scrollY) {
    std::vector<DOMRect> rects;
    if (!node) return rects;

    // For block elements, single rect
    if (!node->isInline()) {
        rects.push_back(getBoundingClientRect(node, scrollX, scrollY));
        return rects;
    }

    // For inline elements, collect rects from line box fragments
    // (simplified: single rect from layout box)
    rects.push_back(getBoundingClientRect(node, scrollX, scrollY));
    return rects;
}

float CSSOMQuery::clientWidth(const BoxNode* node) {
    if (!node) return 0;
    const auto& lb = node->layoutBox();
    const auto& cv = node->computed();
    return lb.width - cv.borderLeftWidth - cv.borderRightWidth;
}

float CSSOMQuery::clientHeight(const BoxNode* node) {
    if (!node) return 0;
    const auto& lb = node->layoutBox();
    const auto& cv = node->computed();
    return lb.height - cv.borderTopWidth - cv.borderBottomWidth;
}

float CSSOMQuery::clientTop(const BoxNode* node) {
    return node ? node->computed().borderTopWidth : 0;
}

float CSSOMQuery::clientLeft(const BoxNode* node) {
    return node ? node->computed().borderLeftWidth : 0;
}

float CSSOMQuery::offsetWidth(const BoxNode* node) {
    return node ? node->layoutBox().width : 0;
}

float CSSOMQuery::offsetHeight(const BoxNode* node) {
    return node ? node->layoutBox().height : 0;
}

float CSSOMQuery::offsetTop(const BoxNode* node) {
    if (!node) return 0;
    const BoxNode* op = offsetParent(node);
    float parentY = op ? op->layoutBox().y : 0;
    return node->layoutBox().y - parentY;
}

float CSSOMQuery::offsetLeft(const BoxNode* node) {
    if (!node) return 0;
    const BoxNode* op = offsetParent(node);
    float parentX = op ? op->layoutBox().x : 0;
    return node->layoutBox().x - parentX;
}

const BoxNode* CSSOMQuery::offsetParent(const BoxNode* node) {
    if (!node) return nullptr;
    const BoxNode* parent = node->parent();
    while (parent) {
        uint8_t pos = parent->computed().position;
        // positioned ancestor (relative, absolute, fixed, sticky)
        if (pos >= 1) return parent;
        // table, th, td
        const std::string& tag = parent->tag();
        if (tag == "table" || tag == "td" || tag == "th") return parent;
        parent = parent->parent();
    }
    return nullptr; // body or null
}

float CSSOMQuery::scrollWidth(const BoxNode* node) {
    if (!node) return 0;
    auto* state = ScrollManager::instance().getScrollState(const_cast<BoxNode*>(node));
    return state ? state->scrollWidth : node->layoutBox().width;
}

float CSSOMQuery::scrollHeight(const BoxNode* node) {
    if (!node) return 0;
    auto* state = ScrollManager::instance().getScrollState(const_cast<BoxNode*>(node));
    return state ? state->scrollHeight : node->layoutBox().height;
}

float CSSOMQuery::scrollTop(const BoxNode* node) {
    if (!node) return 0;
    auto* state = ScrollManager::instance().getScrollState(const_cast<BoxNode*>(node));
    return state ? state->scrollTop : 0;
}

float CSSOMQuery::scrollLeft(const BoxNode* node) {
    if (!node) return 0;
    auto* state = ScrollManager::instance().getScrollState(const_cast<BoxNode*>(node));
    return state ? state->scrollLeft : 0;
}

float CSSOMQuery::innerWidth(float viewportWidth) { return viewportWidth; }
float CSSOMQuery::innerHeight(float viewportHeight) { return viewportHeight; }

std::string CSSOMQuery::getComputedStyleProperty(const BoxNode* node, const std::string& property) {
    if (!node) return "";
    const auto& cv = node->computed();

    auto px = [](float v) -> std::string {
        if (v == 0) return "0px";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4gpx", v);
        return buf;
    };
    auto colorStr = [](uint32_t c) -> std::string {
        uint8_t r = (c >> 24) & 0xFF;
        uint8_t g = (c >> 16) & 0xFF;
        uint8_t b = (c >> 8) & 0xFF;
        uint8_t a = c & 0xFF;
        char buf[64];
        if (a == 255) snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", r, g, b);
        else snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.3g)", r, g, b, a / 255.0f);
        return buf;
    };

    // Display
    if (property == "display") {
        const char* displays[] = {"none","block","inline","inline-block","flex","inline-flex",
                                    "grid","inline-grid","table","table-row","table-cell",
                                    "list-item","contents"};
        return (cv.display < 13) ? displays[cv.display] : "block";
    }
    if (property == "position") {
        const char* positions[] = {"static","relative","absolute","fixed","sticky"};
        return (cv.position < 5) ? positions[cv.position] : "static";
    }
    if (property == "visibility") {
        return cv.visibility == 0 ? "visible" : cv.visibility == 1 ? "hidden" : "collapse";
    }

    // Dimensions
    if (property == "width") return cv.widthAuto ? "auto" : px(node->layoutBox().width);
    if (property == "height") return cv.heightAuto ? "auto" : px(node->layoutBox().height);
    if (property == "min-width") return px(cv.minWidth);
    if (property == "min-height") return px(cv.minHeight);
    if (property == "max-width") return cv.maxWidth >= 1e8f ? "none" : px(cv.maxWidth);
    if (property == "max-height") return cv.maxHeight >= 1e8f ? "none" : px(cv.maxHeight);

    // Margin
    if (property == "margin-top") return cv.marginTopAuto ? "auto" : px(cv.marginTop);
    if (property == "margin-right") return cv.marginRightAuto ? "auto" : px(cv.marginRight);
    if (property == "margin-bottom") return cv.marginBottomAuto ? "auto" : px(cv.marginBottom);
    if (property == "margin-left") return cv.marginLeftAuto ? "auto" : px(cv.marginLeft);

    // Padding
    if (property == "padding-top") return px(cv.paddingTop);
    if (property == "padding-right") return px(cv.paddingRight);
    if (property == "padding-bottom") return px(cv.paddingBottom);
    if (property == "padding-left") return px(cv.paddingLeft);

    // Border width
    if (property == "border-top-width") return px(cv.borderTopWidth);
    if (property == "border-right-width") return px(cv.borderRightWidth);
    if (property == "border-bottom-width") return px(cv.borderBottomWidth);
    if (property == "border-left-width") return px(cv.borderLeftWidth);

    // Border color
    if (property == "border-top-color") return colorStr(cv.borderTopColor);
    if (property == "border-right-color") return colorStr(cv.borderRightColor);
    if (property == "border-bottom-color") return colorStr(cv.borderBottomColor);
    if (property == "border-left-color") return colorStr(cv.borderLeftColor);

    // Border radius
    if (property == "border-top-left-radius") return px(cv.borderTopLeftRadius);
    if (property == "border-top-right-radius") return px(cv.borderTopRightRadius);
    if (property == "border-bottom-right-radius") return px(cv.borderBottomRightRadius);
    if (property == "border-bottom-left-radius") return px(cv.borderBottomLeftRadius);

    // Colors
    if (property == "color") return colorStr(cv.color);
    if (property == "background-color") return colorStr(cv.backgroundColor);

    // Typography
    if (property == "font-size") return px(cv.fontSize);
    if (property == "font-family") return cv.fontFamily;
    if (property == "font-weight") return std::to_string(cv.fontWeight);
    if (property == "line-height") return std::to_string(cv.lineHeight);
    if (property == "opacity") return std::to_string(cv.opacity);

    // Overflow
    if (property == "overflow-x" || property == "overflow-y") {
        uint8_t ov = (property == "overflow-x") ? cv.overflowX : cv.overflowY;
        const char* vals[] = {"visible", "hidden", "scroll", "auto", "clip"};
        return (ov < 5) ? vals[ov] : "visible";
    }

    // Z-index
    if (property == "z-index") return cv.zIndexAuto ? "auto" : std::to_string(cv.zIndex);

    // Flex
    if (property == "flex-grow") return std::to_string(cv.flexGrow);
    if (property == "flex-shrink") return std::to_string(cv.flexShrink);
    if (property == "flex-basis") return cv.flexBasisAuto ? "auto" : px(cv.flexBasis);
    if (property == "order") return std::to_string(cv.order);

    // Positioning
    if (property == "top") return cv.topAuto ? "auto" : px(cv.top);
    if (property == "right") return cv.rightAuto ? "auto" : px(cv.right);
    if (property == "bottom") return cv.bottomAuto ? "auto" : px(cv.bottom);
    if (property == "left") return cv.leftAuto ? "auto" : px(cv.left);

    // Background
    if (property == "background-image") return cv.backgroundImage.empty() ? "none" : cv.backgroundImage;

    // Transform
    if (property == "transform") return cv.transform.empty() ? "none" : cv.transform;
    if (property == "filter") return cv.filter.empty() ? "none" : cv.filter;
    if (property == "cursor") return cv.cursor.empty() ? "auto" : cv.cursor;
    if (property == "pointer-events") return cv.pointerEvents.empty() ? "auto" : cv.pointerEvents;

    return "";
}

bool CSSOMQuery::matchMedia(const std::string& query, float viewportW, float viewportH, float dpr) {
    MediaQueryEvaluator eval;
    MediaQueryEvaluator::Environment env;
    env.viewportWidth = viewportW;
    env.viewportHeight = viewportH;
    env.devicePixelRatio = dpr;
    env.orientation = (viewportW >= viewportH) ? "landscape" : "portrait";
    eval.setEnvironment(env);
    return eval.evaluate(query);
}

BoxNode* CSSOMQuery::elementFromPoint(BoxNode* root, float x, float y) {
    HitTester tester;
    auto result = tester.hitTest(root, x, y);
    return result.node;
}

std::vector<BoxNode*> CSSOMQuery::elementsFromPoint(BoxNode* root, float x, float y) {
    HitTester tester;
    auto results = tester.hitTestAll(root, x, y);
    std::vector<BoxNode*> nodes;
    for (auto& r : results) nodes.push_back(r.node);
    return nodes;
}

CSSOMQuery::CaretPosition CSSOMQuery::caretPositionFromPoint(BoxNode* root, float x, float y) {
    HitTester tester;
    auto result = tester.hitTest(root, x, y);
    return {result.node, result.textOffset};
}

// ==================================================================
// MediaQueryEvaluator
// ==================================================================

MediaQueryEvaluator::MediaQueryEvaluator() {}

float MediaQueryEvaluator::parseLength(const std::string& value) const {
    if (value.empty()) return 0;

    std::string lower = value;
    float num = 0;
    try { num = std::stof(value); } catch (...) { return 0; }

    if (value.find("px") != std::string::npos) return num;
    if (value.find("em") != std::string::npos) return num * 16;
    if (value.find("rem") != std::string::npos) return num * 16;
    if (value.find("vw") != std::string::npos) return num * env_.viewportWidth / 100;
    if (value.find("vh") != std::string::npos) return num * env_.viewportHeight / 100;

    return num;
}

std::vector<MediaQueryEvaluator::MediaQuery> MediaQueryEvaluator::parse(const std::string& queryList) const {
    std::vector<MediaQuery> queries;

    // Split by comma
    std::istringstream stream(queryList);
    std::string segment;
    while (std::getline(stream, segment, ',')) {
        // Trim
        while (!segment.empty() && std::isspace(segment.front())) segment.erase(0, 1);
        while (!segment.empty() && std::isspace(segment.back())) segment.pop_back();
        if (segment.empty()) continue;

        MediaQuery mq;

        // Check for "not"
        if (segment.find("not ") == 0) {
            mq.negated = true;
            segment = segment.substr(4);
            while (!segment.empty() && std::isspace(segment.front())) segment.erase(0, 1);
        }

        // Check for media type
        if (segment.find("screen") == 0) { mq.mediaType = "screen"; segment = segment.substr(6); }
        else if (segment.find("print") == 0) { mq.mediaType = "print"; segment = segment.substr(5); }
        else if (segment.find("all") == 0) { mq.mediaType = "all"; segment = segment.substr(3); }
        else { mq.mediaType = "all"; }

        // Skip "and"
        while (!segment.empty() && std::isspace(segment.front())) segment.erase(0, 1);
        if (segment.find("and ") == 0) segment = segment.substr(4);

        // Parse features in parentheses
        size_t pos = 0;
        while ((pos = segment.find('(', pos)) != std::string::npos) {
            size_t end = segment.find(')', pos);
            if (end == std::string::npos) break;

            std::string feature = segment.substr(pos + 1, end - pos - 1);
            while (!feature.empty() && std::isspace(feature.front())) feature.erase(0, 1);
            while (!feature.empty() && std::isspace(feature.back())) feature.pop_back();

            MediaQuery::Feature f;

            // Check for colon (name: value)
            size_t colon = feature.find(':');
            if (colon != std::string::npos) {
                f.name = feature.substr(0, colon);
                f.value = feature.substr(colon + 1);
                while (!f.name.empty() && std::isspace(f.name.back())) f.name.pop_back();
                while (!f.value.empty() && std::isspace(f.value.front())) f.value.erase(0, 1);

                // Handle min-/max- prefix
                if (f.name.find("min-") == 0) {
                    f.isRange = true;
                    f.hasMin = true;
                    f.minValue = parseLength(f.value);
                    f.name = f.name.substr(4);
                } else if (f.name.find("max-") == 0) {
                    f.isRange = true;
                    f.hasMax = true;
                    f.maxValue = parseLength(f.value);
                    f.name = f.name.substr(4);
                }
            } else {
                // MQ Level 4 range syntax: e.g. "width >= 768px", "width < 1024px"
                // Parse: name operator value
                size_t opPos = std::string::npos;
                std::string op;
                for (size_t p = 0; p < feature.size(); p++) {
                    if (feature[p] == '>' || feature[p] == '<') {
                        opPos = p;
                        op = feature[p];
                        if (p + 1 < feature.size() && feature[p+1] == '=') {
                            op += '=';
                        }
                        break;
                    }
                }
                if (opPos != std::string::npos) {
                    f.name = feature.substr(0, opPos);
                    while (!f.name.empty() && std::isspace(f.name.back())) f.name.pop_back();
                    std::string valStr = feature.substr(opPos + op.size());
                    while (!valStr.empty() && std::isspace(valStr.front())) valStr.erase(0, 1);
                    f.value = valStr;
                    f.isRange = true;
                    float val = parseLength(valStr);
                    if (op == ">=" || op == ">") { f.hasMin = true; f.minValue = val; }
                    else if (op == "<=" || op == "<") { f.hasMax = true; f.maxValue = val; }
                } else {
                    // Boolean feature (e.g. just "color" with no value)
                    f.name = feature;
                }
            }

            mq.features.push_back(f);
            pos = end + 1;
        }

        queries.push_back(mq);
    }

    return queries;
}

bool MediaQueryEvaluator::evaluate(const std::string& query) const {
    auto queries = parse(query);
    // Media query list: any match = true
    for (const auto& mq : queries) {
        if (evaluateQuery(mq)) return true;
    }
    return queries.empty(); // empty query = match all
}

bool MediaQueryEvaluator::evaluateQuery(const MediaQuery& query) const {
    // Media type match
    if (query.mediaType != "all" && query.mediaType != "screen") {
        return query.negated;
    }

    // Evaluate features
    bool allMatch = true;
    for (const auto& f : query.features) {
        if (!evaluateFeature(f)) {
            allMatch = false;
            break;
        }
    }

    return query.negated ? !allMatch : allMatch;
}

bool MediaQueryEvaluator::evaluateFeature(const MediaQuery::Feature& f) const {
    // Width
    if (f.name == "width") {
        if (f.hasMin) return env_.viewportWidth >= f.minValue;
        if (f.hasMax) return env_.viewportWidth <= f.maxValue;
        return env_.viewportWidth == parseLength(f.value);
    }
    // Height
    if (f.name == "height") {
        if (f.hasMin) return env_.viewportHeight >= f.minValue;
        if (f.hasMax) return env_.viewportHeight <= f.maxValue;
        return env_.viewportHeight == parseLength(f.value);
    }
    // Orientation
    if (f.name == "orientation") {
        return f.value == env_.orientation;
    }
    // Resolution / DPR
    if (f.name == "resolution" || f.name == "device-pixel-ratio") {
        float dpr = 0;
        try { dpr = std::stof(f.value); } catch (...) { return false; }
        if (f.hasMin) return env_.devicePixelRatio >= dpr;
        if (f.hasMax) return env_.devicePixelRatio <= dpr;
        return std::abs(env_.devicePixelRatio - dpr) < 0.01f;
    }
    // Color scheme
    if (f.name == "prefers-color-scheme") {
        return f.value == env_.colorScheme;
    }
    // Reduced motion
    if (f.name == "prefers-reduced-motion") {
        return (f.value == "reduce") == env_.reducedMotion;
    }
    // Reduced transparency
    if (f.name == "prefers-reduced-transparency") {
        return (f.value == "reduce") == env_.reducedTransparency;
    }
    // Hover
    if (f.name == "hover") {
        return (f.value == "hover") == env_.hover;
    }
    if (f.name == "any-hover") {
        return (f.value == "hover") == env_.anyHover;
    }
    // Pointer
    if (f.name == "pointer") {
        return f.value == env_.pointer;
    }
    if (f.name == "any-pointer") {
        return f.value == env_.anyPointer;
    }
    // Forced colors
    if (f.name == "forced-colors") {
        return (f.value == "active") == env_.forcedColors;
    }
    // Aspect ratio
    if (f.name == "aspect-ratio") {
        // Parse ratio (e.g., "16/9")
        size_t slash = f.value.find('/');
        if (slash != std::string::npos) {
            float num = std::stof(f.value.substr(0, slash));
            float den = std::stof(f.value.substr(slash + 1));
            float targetRatio = num / den;
            float actualRatio = env_.aspectRatio();
            if (f.hasMin) return actualRatio >= targetRatio;
            if (f.hasMax) return actualRatio <= targetRatio;
            return std::abs(actualRatio - targetRatio) < 0.01f;
        }
    }
    // Color
    if (f.name == "color") {
        if (f.value.empty()) return env_.colorBitDepth > 0;
        int bits = std::stoi(f.value);
        if (f.hasMin) return env_.colorBitDepth >= bits;
        if (f.hasMax) return env_.colorBitDepth <= bits;
        return env_.colorBitDepth == bits;
    }
    // Monochrome
    if (f.name == "monochrome") {
        if (f.value.empty()) return env_.monochrome > 0;
        return env_.monochrome > 0;
    }
    // Inverted colors
    if (f.name == "inverted-colors") {
        return (f.value == "inverted") == env_.invertedColors;
    }

    // Unknown feature — pass
    return true;
}

int MediaQueryEvaluator::addChangeListener(const std::string& query, ChangeCallback cb) {
    int id = nextWatchId_++;
    bool result = evaluate(query);
    watched_.push_back({id, query, result, std::move(cb)});
    return id;
}

void MediaQueryEvaluator::removeChangeListener(int id) {
    watched_.erase(
        std::remove_if(watched_.begin(), watched_.end(),
            [id](const WatchedQuery& w) { return w.id == id; }),
        watched_.end());
}

void MediaQueryEvaluator::reevaluate() {
    for (auto& w : watched_) {
        bool result = evaluate(w.query);
        if (result != w.lastResult) {
            w.lastResult = result;
            if (w.callback) w.callback(w.query, result);
        }
    }
}

// ==================================================================
// Compositor
// ==================================================================

Compositor::Compositor() {}
Compositor::~Compositor() {}

int Compositor::createLayer(BoxNode* owner) {
    Layer layer;
    layer.id = nextLayerId_++;
    layer.owner = owner;
    if (owner) {
        const auto& lb = owner->layoutBox();
        layer.contentWidth = lb.width;
        layer.contentHeight = lb.height;
    }
    layers_.push_back(layer);
    return layer.id;
}

void Compositor::destroyLayer(int id) {
    layers_.erase(
        std::remove_if(layers_.begin(), layers_.end(),
            [id](const Layer& l) { return l.id == id; }),
        layers_.end());
}

Compositor::Layer* Compositor::getLayer(int id) {
    for (auto& l : layers_) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

const Compositor::Layer* Compositor::getLayer(int id) const {
    for (const auto& l : layers_) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

void Compositor::setLayerParent(int childId, int parentId) {
    Layer* parent = getLayer(parentId);
    if (parent) {
        // Remove from existing parent
        for (auto& l : layers_) {
            l.childIds.erase(
                std::remove(l.childIds.begin(), l.childIds.end(), childId),
                l.childIds.end());
        }
        parent->childIds.push_back(childId);
    }
}

void Compositor::removeLayerFromParent(int childId) {
    for (auto& l : layers_) {
        l.childIds.erase(
            std::remove(l.childIds.begin(), l.childIds.end(), childId),
            l.childIds.end());
    }
}

void Compositor::setLayerTransform(int id, float tx, float ty, float sx, float sy, float rotation) {
    Layer* l = getLayer(id);
    if (l) {
        l->translateX = tx;
        l->translateY = ty;
        l->scaleX = sx;
        l->scaleY = sy;
        l->rotation = rotation;
    }
}

void Compositor::setLayerOpacity(int id, float opacity) {
    Layer* l = getLayer(id);
    if (l) l->opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void Compositor::setLayerScrollOffset(int id, float ox, float oy) {
    Layer* l = getLayer(id);
    if (l) {
        l->scrollOffsetX = ox;
        l->scrollOffsetY = oy;
    }
}

void Compositor::setLayerClip(int id, float x, float y, float w, float h) {
    Layer* l = getLayer(id);
    if (l) {
        l->clipX = x; l->clipY = y;
        l->clipWidth = w; l->clipHeight = h;
        l->hasClip = true;
    }
}

void Compositor::clearLayerClip(int id) {
    Layer* l = getLayer(id);
    if (l) l->hasClip = false;
}

bool Compositor::needsOwnLayer(const BoxNode* node) {
    if (!node) return false;
    const auto& cv = node->computed();

    // 3D transform
    if (!cv.transform.empty() &&
        (cv.transform.find("translate3d") != std::string::npos ||
         cv.transform.find("rotate3d") != std::string::npos ||
         cv.transform.find("perspective") != std::string::npos ||
         cv.transform.find("translateZ") != std::string::npos)) {
        return true;
    }

    // will-change: transform, opacity, etc.
    if (!cv.willChange.empty() && cv.willChange != "auto") return true;

    // backdrop-filter
    if (!cv.filter.empty()) return true;

    // Fixed position
    if (cv.position == 3) return true;

    // Video/canvas elements
    const std::string& tag = node->tag();
    if (tag == "video" || tag == "canvas") return true;

    // Opacity != 1 with children
    if (cv.opacity < 1.0f && node->childCount() > 0) return true;

    return false;
}

void Compositor::buildLayerTree(BoxNode* root) {
    layers_.clear();
    nextLayerId_ = 1;

    if (!root) return;

    int rootLayer = createLayer(root);
    rootLayerId_ = rootLayer;

    promoteLayer(root, rootLayer);
}

void Compositor::promoteLayer(BoxNode* node, int parentLayerId) {
    for (const auto& child : node->children()) {
        BoxNode* cn = child.get();
        if (needsOwnLayer(cn)) {
            int childLayer = createLayer(cn);
            setLayerParent(childLayer, parentLayerId);

            Layer* l = getLayer(childLayer);
            if (l) {
                const auto& lb = cn->layoutBox();
                l->translateX = lb.x;
                l->translateY = lb.y;
                l->contentWidth = lb.width;
                l->contentHeight = lb.height;
                l->opacity = cn->computed().opacity;

                // Check scroll container
                auto* scrollState = ScrollManager::instance().getScrollState(cn);
                if (scrollState) {
                    l->isScrollContainer = true;
                    l->scrollOffsetX = scrollState->scrollLeft;
                    l->scrollOffsetY = scrollState->scrollTop;
                }

                // Check 3D
                if (!cn->computed().transform.empty()) l->has3DTransform = true;
                if (!cn->computed().willChange.empty()) l->hasWillChange = true;
                if (cn->computed().position == 3) l->isFixedPosition = true;
            }

            promoteLayer(cn, childLayer);
        } else {
            promoteLayer(cn, parentLayerId);
        }
    }
}

void Compositor::composite() {
    auto start = std::chrono::high_resolution_clock::now();

    stats_.layerCount = static_cast<int>(layers_.size());
    stats_.animationCount = static_cast<int>(animations_.size());

    const Layer* root = getLayer(rootLayerId_);
    if (root) {
        compositeLayer(*root, 0, 0, 1.0f);
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.compositeTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
}

void Compositor::compositeLayer(const Layer& layer, float parentX, float parentY, float parentOpacity) {
    float x = parentX + layer.translateX - layer.scrollOffsetX;
    float y = parentY + layer.translateY - layer.scrollOffsetY;
    float opacity = parentOpacity * layer.opacity;

    if (opacity <= 0) return;

    // Composite children in order
    for (int childId : layer.childIds) {
        const Layer* child = getLayer(childId);
        if (child) {
            compositeLayer(*child, x, y, opacity);
        }
    }
}

int Compositor::addAnimation(const CompositorAnimation& anim) {
    CompositorAnimation a = anim;
    a.id = nextAnimId_++;
    animations_.push_back(a);
    return a.id;
}

void Compositor::removeAnimation(int id) {
    animations_.erase(
        std::remove_if(animations_.begin(), animations_.end(),
            [id](const CompositorAnimation& a) { return a.id == id; }),
        animations_.end());
}

void Compositor::pauseAnimation(int id) {
    for (auto& a : animations_) {
        if (a.id == id) { a.paused = true; break; }
    }
}

void Compositor::resumeAnimation(int id) {
    for (auto& a : animations_) {
        if (a.id == id) { a.paused = false; break; }
    }
}

void Compositor::tickAnimations(double timestamp) {
    for (auto it = animations_.begin(); it != animations_.end();) {
        auto& anim = *it;
        if (anim.paused) { ++it; continue; }

        double elapsed = timestamp - anim.startTime - anim.delay;
        if (elapsed < 0) { ++it; continue; }

        double duration = anim.duration;
        if (duration <= 0) { ++it; continue; }

        // Calculate iteration
        double iterationTime = std::fmod(elapsed, duration);
        int iteration = static_cast<int>(elapsed / duration);

        if (anim.iterations >= 0 && iteration >= anim.iterations) {
            // Animation finished
            it = animations_.erase(it);
            continue;
        }

        float progress = static_cast<float>(iterationTime / duration);

        // Reverse on alternate iterations
        if (anim.alternate && (iteration % 2 == 1)) {
            progress = 1.0f - progress;
        }

        // Apply easing
        if (anim.easing) {
            progress = anim.easing(progress);
        }

        // Interpolate keyframes
        if (anim.keyframes.size() >= 2) {
            float value = 0;
            for (size_t i = 0; i < anim.keyframes.size() - 1; i++) {
                if (progress >= anim.keyframes[i].offset && progress <= anim.keyframes[i+1].offset) {
                    float segProgress = (progress - anim.keyframes[i].offset) /
                                           (anim.keyframes[i+1].offset - anim.keyframes[i].offset);
                    value = anim.keyframes[i].value +
                              segProgress * (anim.keyframes[i+1].value - anim.keyframes[i].value);
                    break;
                }
            }

            // Apply to layer
            Layer* layer = getLayer(anim.layerId);
            if (layer) {
                if (anim.property == "opacity") {
                    layer->opacity = std::clamp(value, 0.0f, 1.0f);
                } else if (anim.property == "translateX") {
                    layer->translateX = value;
                } else if (anim.property == "translateY") {
                    layer->translateY = value;
                } else if (anim.property == "scaleX") {
                    layer->scaleX = value;
                } else if (anim.property == "scaleY") {
                    layer->scaleY = value;
                } else if (anim.property == "rotation") {
                    layer->rotation = value;
                }
            }
        }

        ++it;
    }
}

// ==================================================================
// TileRasterizer
// ==================================================================

TileRasterizer::TileRasterizer() {}
TileRasterizer::~TileRasterizer() {}

void TileRasterizer::createTilesForLayer(int layerId, float contentWidth, float contentHeight) {
    int cols = static_cast<int>(std::ceil(contentWidth / TILE_SIZE));
    int rows = static_cast<int>(std::ceil(contentHeight / TILE_SIZE));

    auto& tiles = layerTiles_[layerId];
    tiles.clear();

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            Tile tile;
            tile.id = nextTileId_++;
            tile.layerId = layerId;
            tile.col = c;
            tile.row = r;
            tile.dirty = true;
            tiles.push_back(tile);
        }
    }
}

void TileRasterizer::invalidateRegion(int layerId, float x, float y, float w, float h) {
    auto it = layerTiles_.find(layerId);
    if (it == layerTiles_.end()) return;

    int startCol = static_cast<int>(x / TILE_SIZE);
    int startRow = static_cast<int>(y / TILE_SIZE);
    int endCol = static_cast<int>((x + w) / TILE_SIZE);
    int endRow = static_cast<int>((y + h) / TILE_SIZE);

    for (auto& tile : it->second) {
        if (tile.col >= startCol && tile.col <= endCol &&
            tile.row >= startRow && tile.row <= endRow) {
            tile.dirty = true;
            tile.rasterized = false;
        }
    }
}

void TileRasterizer::invalidateLayer(int layerId) {
    auto it = layerTiles_.find(layerId);
    if (it == layerTiles_.end()) return;
    for (auto& tile : it->second) {
        tile.dirty = true;
        tile.rasterized = false;
    }
}

std::vector<TileRasterizer::Tile*> TileRasterizer::getVisibleTiles(
    int layerId, float vpX, float vpY, float vpW, float vpH) {

    std::vector<Tile*> visible;
    auto it = layerTiles_.find(layerId);
    if (it == layerTiles_.end()) return visible;

    int startCol = std::max(0, static_cast<int>(vpX / TILE_SIZE) - 1);
    int startRow = std::max(0, static_cast<int>(vpY / TILE_SIZE) - 1);
    int endCol = static_cast<int>((vpX + vpW) / TILE_SIZE) + 1;
    int endRow = static_cast<int>((vpY + vpH) / TILE_SIZE) + 1;

    for (auto& tile : it->second) {
        if (tile.col >= startCol && tile.col <= endCol &&
            tile.row >= startRow && tile.row <= endRow) {
            visible.push_back(&tile);
        }
    }

    return visible;
}

void TileRasterizer::rasterizeTile(Tile* tile, BoxNode* /*layerRoot*/) {
    if (!tile || !tile->dirty) return;
    // Actual rasterization would invoke the paint pipeline for the tile region
    // Here we mark it as rasterized
    tile->dirty = false;
    tile->rasterized = true;
}

void TileRasterizer::prioritizeTiles(float centerX, float centerY) {
    for (auto& [layerId, tiles] : layerTiles_) {
        for (auto& tile : tiles) {
            float tileCenterX = (tile.col + 0.5f) * TILE_SIZE;
            float tileCenterY = (tile.row + 0.5f) * TILE_SIZE;
            float dx = tileCenterX - centerX;
            float dy = tileCenterY - centerY;
            tile.priority = -(dx * dx + dy * dy); // Negative = higher priority for closer tiles
        }
        std::sort(tiles.begin(), tiles.end(),
            [](const Tile& a, const Tile& b) { return a.priority > b.priority; });
    }
}

int TileRasterizer::totalTileCount() const {
    int count = 0;
    for (const auto& [id, tiles] : layerTiles_) count += static_cast<int>(tiles.size());
    return count;
}

int TileRasterizer::dirtyTileCount() const {
    int count = 0;
    for (const auto& [id, tiles] : layerTiles_) {
        for (const auto& t : tiles) if (t.dirty) count++;
    }
    return count;
}

// ==================================================================
// ScrollSnap
// ==================================================================

float ScrollSnap::snap(float scrollPos, float velocity,
                          const std::vector<SnapPoint>& points,
                          SnapType type, float viewportSize) {

    if (points.empty() || type == SnapType::None) return scrollPos;

    float closest = scrollPos;
    float minDist = 1e9f;

    for (const auto& pt : points) {
        float snapTarget = pt.position;
        // Adjust for alignment
        if (pt.alignX == SnapAlign::Center) snapTarget -= viewportSize * 0.5f;
        else if (pt.alignX == SnapAlign::End) snapTarget -= viewportSize;

        float dist = std::abs(scrollPos - snapTarget);
        if (dist < minDist) {
            minDist = dist;
            closest = snapTarget;
        }
    }

    // For proximity type, only snap if close enough
    if (type == SnapType::Proximity && minDist > viewportSize * 0.3f) {
        return scrollPos;
    }

    return closest;
}

std::vector<ScrollSnap::SnapPoint> ScrollSnap::collectSnapPoints(BoxNode* scrollContainer, bool horizontal) {
    std::vector<SnapPoint> points;
    if (!scrollContainer) return points;

    for (const auto& child : scrollContainer->children()) {
        const auto& lb = child->layoutBox();
        SnapPoint pt;
        pt.position = horizontal ? lb.x : lb.y;
        points.push_back(pt);
    }

    return points;
}

ScrollSnap::SnapType ScrollSnap::parseSnapType(const std::string& value) {
    if (value.find("mandatory") != std::string::npos) return SnapType::Mandatory;
    if (value.find("proximity") != std::string::npos) return SnapType::Proximity;
    return SnapType::None;
}

ScrollSnap::SnapAlign ScrollSnap::parseSnapAlign(const std::string& value) {
    if (value == "center") return SnapAlign::Center;
    if (value == "end") return SnapAlign::End;
    return SnapAlign::Start;
}

// ==================================================================
// ContainmentEngine
// ==================================================================

uint8_t ContainmentEngine::getContainment(const BoxNode* node) {
    if (!node) return 0;
    // Parse contain property from computed values
    // (Stored as a string in ComputedValues; map to flags)
    return 0; // Default: no containment
}

ContainmentEngine::ContentVisibility ContainmentEngine::getContentVisibility(const BoxNode* node) {
    if (!node) return ContentVisibility::Visible;
    // Parse content-visibility from computed values
    return ContentVisibility::Visible;
}

bool ContainmentEngine::isOffscreen(const BoxNode* node, float viewportTop, float viewportBottom) {
    if (!node) return true;
    const auto& lb = node->layoutBox();
    // Node is offscreen if its bottom is above viewport top or top is below viewport bottom
    return (lb.y + lb.height < viewportTop) || (lb.y > viewportBottom);
}

ContainmentEngine::ContainedSize ContainmentEngine::getContainedSize(const BoxNode* node) {
    ContainedSize cs;
    if (!node) return cs;
    uint8_t contain = getContainment(node);
    if (contain & static_cast<uint8_t>(ContainType::Size)) {
        cs.hasWidth = true;
        cs.hasHeight = true;
        // Use explicit dimensions or 0
        cs.width = node->computed().widthAuto ? 0 : node->computed().width;
        cs.height = node->computed().heightAuto ? 0 : node->computed().height;
    }
    return cs;
}

bool ContainmentEngine::shouldSkipLayout(const BoxNode* node, float viewportTop, float viewportBottom) {
    if (getContentVisibility(node) == ContentVisibility::Auto) {
        return isOffscreen(node, viewportTop, viewportBottom);
    }
    if (getContentVisibility(node) == ContentVisibility::Hidden) return true;
    return false;
}

bool ContainmentEngine::shouldSkipPaint(const BoxNode* node, float viewportTop, float viewportBottom) {
    return shouldSkipLayout(node, viewportTop, viewportBottom);
}

} // namespace Web
} // namespace NXRender
