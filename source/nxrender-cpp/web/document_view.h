// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/box/box_tree.h"
#include <algorithm>
#include "web/css/cascade.h"
#include "web/frame_orchestrator.h"
#include "web/input.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <cstdint>

// Forward declarations — WebCore types (no hard dependency)
namespace Zepra::WebCore {
    class DOMDocument;
    class DOMNode;
    class DOMElement;
    class DOMText;
    class CSSEngine;
    class CSSComputedStyle;
}

namespace NXRender {
namespace Web {

// ==================================================================
// Style conversion: WebCore CSSComputedStyle → NXRender ComputedValues
// ==================================================================

class StyleBridge {
public:
    // Convert a WebCore computed style to NXRender ComputedValues
    static ComputedValues convert(const Zepra::WebCore::CSSComputedStyle* wcStyle,
                                    float viewportW, float viewportH,
                                    float parentFontSize = 16.0f,
                                    float rootFontSize = 16.0f);

    // Map WebCore display enum to NXRender display uint8_t
    static uint8_t mapDisplay(int wcDisplay);

    // Map WebCore position enum to NXRender position uint8_t
    static uint8_t mapPosition(int wcPosition);

    // Map WebCore overflow enum to NXRender overflow uint8_t
    static uint8_t mapOverflow(int wcOverflow);

    // Map WebCore flex direction enum
    static uint8_t mapFlexDirection(int wcDir);

    // Map WebCore justify/align enum
    static uint8_t mapJustifyAlign(int wcJA);

    // Resolve a WebCore CSSLength to pixels
    static float resolveLength(float value, int unit,
                                 float fontSize, float rootFontSize,
                                 float viewportW, float viewportH,
                                 float containerSize = 0);

    // Convert WebCore CSSColor (RGBA components) to packed uint32_t
    static uint32_t packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
};

// ==================================================================
// BoxTreeBuilder: DOMDocument → BoxNode tree
// ==================================================================

class DOMBoxBuilder {
public:
    DOMBoxBuilder();
    ~DOMBoxBuilder();

    struct Options {
        float viewportWidth = 1920;
        float viewportHeight = 1080;
        float rootFontSize = 16;
        bool debugBorders = false;
    };

    // Build the complete BoxNode tree from a DOM document
    std::unique_ptr<BoxNode> build(Zepra::WebCore::DOMDocument* doc,
                                     Zepra::WebCore::CSSEngine* css,
                                     const Options& opts);

    // Incrementally rebuild a subtree (for DOM mutations)
    void rebuildSubtree(BoxNode* existingBox,
                          Zepra::WebCore::DOMNode* domNode,
                          Zepra::WebCore::CSSEngine* css);

    // Get the BoxNode associated with a DOM node
    BoxNode* findBoxForDom(void* domNode);

    // Stats
    size_t nodeCount() const { return nodeCount_; }
    size_t textNodeCount() const { return textNodeCount_; }

private:
    Options opts_;
    size_t nodeCount_ = 0;
    size_t textNodeCount_ = 0;

    // DOM node → BoxNode mapping (for event dispatch, mutations)
    std::unordered_map<void*, BoxNode*> domToBox_;

    // Recursive tree construction
    std::unique_ptr<BoxNode> buildNode(Zepra::WebCore::DOMNode* node,
                                         Zepra::WebCore::CSSEngine* css,
                                         float parentFontSize);

    // Build a text box
    std::unique_ptr<BoxNode> buildTextNode(Zepra::WebCore::DOMText* textNode,
                                              float parentFontSize);

    // Build an element box
    std::unique_ptr<BoxNode> buildElementNode(Zepra::WebCore::DOMElement* element,
                                                 Zepra::WebCore::CSSEngine* css,
                                                 float parentFontSize);

    // Determine BoxType from display property
    BoxType boxTypeFromDisplay(uint8_t display);

    // Should this tag be skipped entirely? (script, style, head, etc.)
    static bool isInvisibleTag(const std::string& tag);

    // Is this a void element? (br, hr, img, input, etc.)
    static bool isVoidElement(const std::string& tag);

    // Register scroll container if overflow != visible
    void maybeRegisterScroller(BoxNode* box);
};

// ==================================================================
// DocumentView: owns the full rendering lifecycle for a document
// ==================================================================

class DocumentView {
public:
    DocumentView();
    ~DocumentView();

    // Attach a DOM document and CSS engine
    void attach(Zepra::WebCore::DOMDocument* doc,
                  Zepra::WebCore::CSSEngine* css);

    // Detach and release all resources
    void detach();

    // Configuration
    void setViewport(float width, float height);
    float viewportWidth() const { return viewportW_; }
    float viewportHeight() const { return viewportH_; }

    // Force full rebuild of the box tree
    void rebuild();

    // Mark dirty for next frame
    void invalidateStyle();
    void invalidateLayout();
    void invalidatePaint();

    // Render one frame (called per vsync)
    void renderFrame(double timestamp);

    // Input routing
    void onMouseDown(float x, float y, int button,
                       bool shift = false, bool ctrl = false, bool alt = false, bool meta = false);
    void onMouseUp(float x, float y, int button,
                     bool shift = false, bool ctrl = false, bool alt = false, bool meta = false);
    void onMouseMove(float x, float y,
                       bool shift = false, bool ctrl = false, bool alt = false, bool meta = false);
    void onDoubleClick(float x, float y);
    void onKeyDown(const std::string& key, const std::string& code,
                     bool shift = false, bool ctrl = false, bool alt = false, bool meta = false);
    void onKeyUp(const std::string& key, const std::string& code,
                   bool shift = false, bool ctrl = false, bool alt = false, bool meta = false);
    void onWheel(float x, float y, float deltaX, float deltaY);
    void onResize(float width, float height);

    // Scroll
    void scrollTo(float x, float y, bool smooth = false);
    float scrollX() const;
    float scrollY() const;

    // Query
    BoxNode* rootBox() const { return root_.get(); }
    FrameOrchestrator& orchestrator() { return orchestrator_; }
    const FrameOrchestrator::FrameStats& lastFrameStats() const;

    // Find element at point
    HitTestResult hitTestAt(float x, float y);

    // Selection
    std::string getSelectedText() const;
    void selectAll();

    // Navigation callbacks
    using LinkCallback = std::function<void(const std::string& href)>;
    using FormCallback = std::function<void(const std::string& action, const std::string& method)>;
    using TitleCallback = std::function<void(const std::string& title)>;

    void setLinkCallback(LinkCallback cb) { linkCallback_ = std::move(cb); }
    void setFormCallback(FormCallback cb) { formCallback_ = std::move(cb); }
    void setTitleCallback(TitleCallback cb) { titleCallback_ = std::move(cb); }

private:
    Zepra::WebCore::DOMDocument* doc_ = nullptr;
    Zepra::WebCore::CSSEngine* css_ = nullptr;

    float viewportW_ = 1920;
    float viewportH_ = 1080;

    std::unique_ptr<BoxNode> root_;
    DOMBoxBuilder builder_;
    FrameOrchestrator orchestrator_;
    InputHandler input_;

    bool attached_ = false;

    LinkCallback linkCallback_;
    FormCallback formCallback_;
    TitleCallback titleCallback_;

    // Extract page title from DOM
    void extractTitle();
};

} // namespace Web
} // namespace NXRender
