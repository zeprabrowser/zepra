// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "frame_orchestrator.h"
#include "web/paint/painters.h"
#include "web/animation.h"
#include "web/input.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace NXRender {
namespace Web {

// ==================================================================
// FrameOrchestrator
// ==================================================================

FrameOrchestrator::FrameOrchestrator() {}
FrameOrchestrator::~FrameOrchestrator() {}

void FrameOrchestrator::setViewport(float width, float height, float dpr) {
    if (viewportWidth_ != width || viewportHeight_ != height || dpr_ != dpr) {
        viewportWidth_ = width;
        viewportHeight_ = height;
        dpr_ = dpr;
        needsLayout_ = true;
        needsPaint_ = true;
    }
}

void FrameOrchestrator::setScrollOffset(float x, float y) {
    if (scrollX_ != x || scrollY_ != y) {
        scrollX_ = x;
        scrollY_ = y;
        needsPaint_ = true;
    }
}

void FrameOrchestrator::renderFrame() {
    if (!root_) return;

    auto frameStart = std::chrono::steady_clock::now();
    FrameStats stats{};

    double timestamp = std::chrono::duration<double, std::milli>(
        frameStart.time_since_epoch()).count();

    // Run RAF callbacks before layout
    runRAFCallbacks(timestamp);

    // Tick animations — applies animated values to ComputedValues
    AnimationTimeline::instance().tick(timestamp);
    if (AnimationTimeline::instance().hasActiveAnimations()) {
        needsStyleRecalc_ = true; // Animated values need re-layout
    }
    AnimationTimeline::instance().collectGarbage();

    // Tick smooth scroll animations
    ScrollManager::instance().tick(timestamp);

    // Phase 1: Style recalculation
    if (needsStyleRecalc_) {
        auto t0 = std::chrono::steady_clock::now();
        runStyleRecalc();
        auto t1 = std::chrono::steady_clock::now();
        stats.styleRecalcMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        needsStyleRecalc_ = false;
        needsLayout_ = true;
    }

    // Phase 2: Layout
    if (needsLayout_) {
        auto t0 = std::chrono::steady_clock::now();
        runLayout();
        auto t1 = std::chrono::steady_clock::now();
        stats.layoutMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats.fullLayout = true;
        needsLayout_ = false;
        needsPaint_ = true;

        runPostLayoutCallbacks();
    }

    // Phase 3: Paint
    if (needsPaint_) {
        auto t0 = std::chrono::steady_clock::now();
        runPaint();
        auto t1 = std::chrono::steady_clock::now();
        stats.paintMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats.fullPaint = true;
        needsPaint_ = false;
    }

    // Phase 4: Composite
    if (pipeline_) {
        auto t0 = std::chrono::steady_clock::now();
        runComposite();
        auto t1 = std::chrono::steady_clock::now();
        stats.compositeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    // Flush MutationObserver microtask queue
    MutationObserverRegistry::instance().notifyAll();

    auto frameEnd = std::chrono::steady_clock::now();
    stats.totalMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
    lastStats_ = stats;
}

void FrameOrchestrator::scheduleStyleRecalc() { needsStyleRecalc_ = true; }
void FrameOrchestrator::scheduleLayout() { needsLayout_ = true; }
void FrameOrchestrator::schedulePaint() { needsPaint_ = true; }

void FrameOrchestrator::forceStyleRecalc() {
    needsStyleRecalc_ = true;
    runStyleRecalc();
    needsStyleRecalc_ = false;
    needsLayout_ = true;
}

void FrameOrchestrator::forceLayout() {
    if (needsStyleRecalc_) forceStyleRecalc();
    needsLayout_ = true;
    runLayout();
    needsLayout_ = false;
    needsPaint_ = true;
    runPostLayoutCallbacks();
}

void FrameOrchestrator::markStyleDirty(BoxNode* /*node*/) {
    needsStyleRecalc_ = true;
}

void FrameOrchestrator::markLayoutDirty(BoxNode* /*node*/) {
    needsLayout_ = true;
}

void FrameOrchestrator::markPaintDirty(BoxNode* /*node*/) {
    needsPaint_ = true;
}

void FrameOrchestrator::markSubtreeDirty(BoxNode* /*node*/) {
    needsStyleRecalc_ = true;
    needsLayout_ = true;
    needsPaint_ = true;
}

int FrameOrchestrator::requestAnimationFrame(FrameCallback cb) {
    int id = nextRAFId_++;
    rafCallbacks_.push_back({id, std::move(cb)});
    return id;
}

void FrameOrchestrator::cancelAnimationFrame(int id) {
    rafCallbacks_.erase(
        std::remove_if(rafCallbacks_.begin(), rafCallbacks_.end(),
                        [id](const RAFEntry& e) { return e.id == id; }),
        rafCallbacks_.end());
}

void FrameOrchestrator::addPostLayoutCallback(PostLayoutCallback cb) {
    postLayoutCallbacks_.push_back(std::move(cb));
}

void FrameOrchestrator::runRAFCallbacks(double timestamp) {
    auto callbacks = std::move(rafCallbacks_);
    rafCallbacks_.clear();
    for (auto& entry : callbacks) {
        if (entry.callback) entry.callback(timestamp);
    }
}

void FrameOrchestrator::runPostLayoutCallbacks() {
    for (auto& cb : postLayoutCallbacks_) {
        if (cb) cb();
    }
}

void FrameOrchestrator::runStyleRecalc() {
    // Walk the tree and recompute styles from CSS cascade
    // In a full implementation, this resolves:
    // - Inherited properties (color, font, direction)
    // - Specified → computed value conversion
    // - Custom properties (var() substitution)
    // - Cascade layer ordering
    // Currently delegated to the CSS cascade engine (cascade.h)
    if (!root_) return;

    std::function<void(BoxNode*)> walk = [&](BoxNode* node) {
        // Computed values are already on the node from cascade.h
        // This pass handles inheritance propagation:
        if (node->parent()) {
            auto& cv = node->computed();
            auto& parent = node->parent()->computed();

            // Inherit color if not explicitly set
            if (cv.color == 0 && parent.color != 0) {
                cv.color = parent.color;
            }
            // Inherit font properties
            if (cv.fontSize <= 0) cv.fontSize = parent.fontSize;
            if (cv.fontFamily.empty()) cv.fontFamily = parent.fontFamily;
            if (cv.lineHeight <= 0) cv.lineHeight = parent.lineHeight;
        }

        // Default font size
        auto& cv = node->computed();
        if (cv.fontSize <= 0) cv.fontSize = 16.0f;
        if (cv.fontFamily.empty()) cv.fontFamily = "sans-serif";
        if (cv.lineHeight <= 0) cv.lineHeight = 1.5f;

        for (auto& child : node->children()) {
            walk(child.get());
        }
    };
    walk(root_);
}

void FrameOrchestrator::runLayout() {
    if (!root_) return;
    LayoutEngine engine;
    lastStats_.layoutCount = engine.layoutTree(root_, viewportWidth_, viewportHeight_, dpr_);
}

void FrameOrchestrator::runPaint() {
    if (!root_) return;
    PaintTreeBuilder builder;
    auto paintList = builder.build(root_);
    lastStats_.paintOpCount = static_cast<int>(paintList.ops().size());

    // If we have a render pipeline, record paint ops into the root layer's display list
    if (pipeline_) {
        // Ensure we have at least one layer
        static uint32_t rootLayerId = 0;
        if (rootLayerId == 0) {
            rootLayerId = pipeline_->addLayer(
                Rect(0, 0, viewportWidth_, viewportHeight_), 0);
        }

        auto* dl = pipeline_->layerDisplayList(rootLayerId);
        if (dl) {
            dl->clear();

            // Convert PaintOps → DisplayList entries
            for (const auto& op : paintList.ops()) {
                switch (op.type) {
                    case PaintOpType::FillRect:
                        dl->fillRect(
                            Rect(op.x - scrollX_, op.y - scrollY_, op.width, op.height),
                            Color(
                                static_cast<uint8_t>((op.color >> 24) & 0xFF),
                                static_cast<uint8_t>((op.color >> 16) & 0xFF),
                                static_cast<uint8_t>((op.color >> 8) & 0xFF),
                                static_cast<uint8_t>(op.color & 0xFF)
                            ));
                        break;

                    case PaintOpType::FillRoundedRect:
                        dl->fillRoundedRect(
                            Rect(op.x - scrollX_, op.y - scrollY_, op.width, op.height),
                            Color(
                                static_cast<uint8_t>((op.color >> 24) & 0xFF),
                                static_cast<uint8_t>((op.color >> 16) & 0xFF),
                                static_cast<uint8_t>((op.color >> 8) & 0xFF),
                                static_cast<uint8_t>(op.color & 0xFF)
                            ),
                            CornerRadii{op.radiusTL, op.radiusTR, op.radiusBR, op.radiusBL});
                        break;

                    case PaintOpType::DrawText:
                        dl->drawText(op.text, op.x - scrollX_, op.y - scrollY_,
                                      Color(
                                          static_cast<uint8_t>((op.color >> 24) & 0xFF),
                                          static_cast<uint8_t>((op.color >> 16) & 0xFF),
                                          static_cast<uint8_t>((op.color >> 8) & 0xFF),
                                          static_cast<uint8_t>(op.color & 0xFF)
                                      ), op.fontSize);
                        break;

                    case PaintOpType::DrawImage:
                        if (op.textureId > 0) {
                            dl->drawTexture(op.textureId,
                                Rect(op.x - scrollX_, op.y - scrollY_, op.width, op.height));
                        }
                        break;

                    case PaintOpType::DrawShadow:
                        dl->drawShadow(
                            Rect(op.x - scrollX_, op.y - scrollY_, op.width, op.height),
                            Color(
                                static_cast<uint8_t>((op.color >> 24) & 0xFF),
                                static_cast<uint8_t>((op.color >> 16) & 0xFF),
                                static_cast<uint8_t>((op.color >> 8) & 0xFF),
                                static_cast<uint8_t>(op.color & 0xFF)
                            ),
                            op.shadowBlur, op.shadowOffsetX, op.shadowOffsetY);
                        break;

                    case PaintOpType::PushClip:
                        dl->pushClip(Rect(op.x - scrollX_, op.y - scrollY_, op.width, op.height));
                        break;

                    case PaintOpType::PopClip:
                        dl->popClip();
                        break;

                    case PaintOpType::PushOpacity:
                        // Opacity needs render target — translate to alpha on upcoming ops
                        break;

                    case PaintOpType::PopOpacity:
                        break;

                    default:
                        break;
                }
            }
        }

        pipeline_->invalidateLayer(rootLayerId);
    }
}

void FrameOrchestrator::runComposite() {
    if (pipeline_) {
        pipeline_->renderFrame();
    }
}

// ==================================================================
// LayoutEngine
// ==================================================================

LayoutEngine::LayoutEngine() {}
LayoutEngine::~LayoutEngine() {}

int LayoutEngine::layoutTree(BoxNode* root, float viewportWidth, float viewportHeight, float dpr) {
    if (!root) return 0;

    layoutCount_ = 0;

    LayoutContext ctx;
    ctx.containingBlockWidth = viewportWidth;
    ctx.containingBlockHeight = viewportHeight;
    ctx.viewportWidth = viewportWidth;
    ctx.viewportHeight = viewportHeight;
    ctx.dpr = dpr;

    // Root box gets viewport dimensions
    auto& lb = root->layoutBox();
    lb.x = 0; lb.y = 0;
    lb.width = viewportWidth;
    lb.height = viewportHeight;
    lb.contentX = 0; lb.contentY = 0;
    lb.contentWidth = viewportWidth;
    lb.contentHeight = viewportHeight;

    layoutNode(root, ctx);
    return layoutCount_;
}

int LayoutEngine::layoutSubtree(BoxNode* node, const LayoutContext& parentCtx) {
    layoutCount_ = 0;
    LayoutContext ctx = parentCtx;
    layoutNode(node, ctx);
    return layoutCount_;
}

void LayoutEngine::layoutNode(BoxNode* node, LayoutContext& ctx) {
    if (!node) return;
    if (node->boxType() == BoxType::None) return;

    layoutCount_++;

    // Resolve box model dimensions
    auto& cv = node->computed();
    auto& lb = node->layoutBox();

    // Resolve padding
    lb.paddingTop = resolveLength(std::to_string(cv.paddingTop), ctx.containingBlockWidth,
                                    ctx.viewportWidth, ctx.viewportHeight);
    lb.paddingRight = resolveLength(std::to_string(cv.paddingRight), ctx.containingBlockWidth,
                                      ctx.viewportWidth, ctx.viewportHeight);
    lb.paddingBottom = resolveLength(std::to_string(cv.paddingBottom), ctx.containingBlockWidth,
                                       ctx.viewportWidth, ctx.viewportHeight);
    lb.paddingLeft = resolveLength(std::to_string(cv.paddingLeft), ctx.containingBlockWidth,
                                     ctx.viewportWidth, ctx.viewportHeight);

    // Resolve border widths
    lb.borderTop = cv.borderTopWidth;
    lb.borderRight = cv.borderRightWidth;
    lb.borderBottom = cv.borderBottomWidth;
    lb.borderLeft = cv.borderLeftWidth;

    // Resolve margins
    lb.marginTop = resolveLength(std::to_string(cv.marginTop), ctx.containingBlockWidth,
                                   ctx.viewportWidth, ctx.viewportHeight);
    lb.marginRight = resolveLength(std::to_string(cv.marginRight), ctx.containingBlockWidth,
                                     ctx.viewportWidth, ctx.viewportHeight);
    lb.marginBottom = resolveLength(std::to_string(cv.marginBottom), ctx.containingBlockWidth,
                                      ctx.viewportWidth, ctx.viewportHeight);
    lb.marginLeft = resolveLength(std::to_string(cv.marginLeft), ctx.containingBlockWidth,
                                    ctx.viewportWidth, ctx.viewportHeight);

    // Dispatch layout by formatting context
    if (node->isPositioned() && cv.position >= 2) { // absolute/fixed
        layoutPositioned(node, ctx);
    } else if (node->isFloating()) {
        placeFloat(node, ctx);
    } else {
        switch (node->boxType()) {
            case BoxType::Block:
            case BoxType::ListItem:
            case BoxType::Anonymous:
                layoutBlock(node, ctx);
                break;
            case BoxType::Inline:
                layoutInline(node, ctx);
                break;
            case BoxType::InlineBlock:
                layoutBlock(node, ctx); // Block inside, inline outside
                break;
            case BoxType::Flex:
            case BoxType::InlineFlex:
                layoutFlex(node, ctx);
                break;
            case BoxType::Grid:
            case BoxType::InlineGrid:
                layoutGrid(node, ctx);
                break;
            case BoxType::Table:
                layoutTable(node, ctx);
                break;
            default:
                layoutBlock(node, ctx);
                break;
        }
    }

    // Set content box from padding box
    lb.contentX = lb.x + lb.borderLeft + lb.paddingLeft;
    lb.contentY = lb.y + lb.borderTop + lb.paddingTop;
    lb.contentWidth = lb.width - lb.borderLeft - lb.borderRight - lb.paddingLeft - lb.paddingRight;
    lb.contentHeight = lb.height - lb.borderTop - lb.borderBottom - lb.paddingTop - lb.paddingBottom;

    // Clamp content to non-negative
    if (lb.contentWidth < 0) lb.contentWidth = 0;
    if (lb.contentHeight < 0) lb.contentHeight = 0;
}

// ==================================================================
// Block formatting context
// ==================================================================

void LayoutEngine::layoutBlock(BoxNode* node, LayoutContext& ctx) {
    auto& lb = node->layoutBox();

    // Resolve width
    float resolvedWidth = resolveWidth(node, ctx);
    if (resolvedWidth >= 0) {
        lb.width = resolvedWidth + lb.paddingLeft + lb.paddingRight + lb.borderLeft + lb.borderRight;
    } else {
        // Auto width: fill containing block
        lb.width = ctx.containingBlockWidth - lb.marginLeft - lb.marginRight;
    }

    // Min/max width constraints
    float minW = resolveMinWidth(node, ctx);
    float maxW = resolveMaxWidth(node, ctx);
    if (minW >= 0 && lb.width < minW) lb.width = minW;
    if (maxW >= 0 && lb.width > maxW) lb.width = maxW;

    // Layout children in a new BFC
    float contentWidth = lb.width - lb.paddingLeft - lb.paddingRight - lb.borderLeft - lb.borderRight;
    float childY = 0;
    float prevMarginBottom = 0;

    LayoutContext childCtx;
    childCtx.containingBlockWidth = contentWidth;
    childCtx.containingBlockHeight = ctx.containingBlockHeight; // provisional
    childCtx.viewportWidth = ctx.viewportWidth;
    childCtx.viewportHeight = ctx.viewportHeight;
    childCtx.dpr = ctx.dpr;
    childCtx.atBFCStart = true;

    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;

        layoutNode(child.get(), childCtx);

        auto& childLB = child->layoutBox();

        // Margin collapse between siblings
        float effectiveTopMargin;
        if (childCtx.atBFCStart) {
            effectiveTopMargin = childLB.marginTop;
            childCtx.atBFCStart = false;
        } else {
            effectiveTopMargin = collapseMargins(prevMarginBottom, childLB.marginTop);
            childY -= prevMarginBottom;
        }
        childY += effectiveTopMargin;

        // Position child
        childLB.x = lb.x + lb.borderLeft + lb.paddingLeft + childLB.marginLeft;
        childLB.y = lb.y + lb.borderTop + lb.paddingTop + childY;

        childY += childLB.height + childLB.marginBottom;
        prevMarginBottom = childLB.marginBottom;
    }

    // Resolve height
    float resolvedHeight = resolveHeight(node, ctx);
    if (resolvedHeight >= 0) {
        lb.height = resolvedHeight + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    } else {
        // Auto height: content height
        lb.height = childY + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    }

    // Min/max height constraints
    float minH = resolveMinHeight(node, ctx);
    float maxH = resolveMaxHeight(node, ctx);
    if (minH >= 0 && lb.height < minH) lb.height = minH;
    if (maxH >= 0 && lb.height > maxH) lb.height = maxH;
}

void LayoutEngine::layoutBlockChildren(BoxNode* node, LayoutContext& ctx) {
    layoutBlock(node, ctx);
}

// ==================================================================
// Inline layout
// ==================================================================

void LayoutEngine::layoutInline(BoxNode* node, LayoutContext& ctx) {
    auto& lb = node->layoutBox();

    if (node->isTextNode()) {
        // Measure text
        float fontSize = node->computed().fontSize;
        if (fontSize <= 0) fontSize = 16.0f;
        float charWidth = fontSize * 0.55f;
        float textWidth = node->text().size() * charWidth;
        float lineHeight = fontSize * node->computed().lineHeight;

        // Word wrap
        if (textWidth > ctx.containingBlockWidth && ctx.containingBlockWidth > 0) {
            int charsPerLine = static_cast<int>(ctx.containingBlockWidth / charWidth);
            if (charsPerLine < 1) charsPerLine = 1;
            int lineCount = static_cast<int>(std::ceil(
                static_cast<float>(node->text().size()) / charsPerLine));
            lb.width = ctx.containingBlockWidth;
            lb.height = lineCount * lineHeight;
        } else {
            lb.width = textWidth;
            lb.height = lineHeight;
        }
    } else {
        // Inline container — sum of children
        float totalWidth = 0;
        float maxHeight = 0;

        LayoutContext childCtx = ctx;
        for (auto& child : node->children()) {
            layoutNode(child.get(), childCtx);
            auto& childLB = child->layoutBox();
            childLB.x = lb.x + totalWidth;
            childLB.y = lb.y;
            totalWidth += childLB.width;
            maxHeight = std::max(maxHeight, childLB.height);
        }
        lb.width = totalWidth;
        lb.height = maxHeight;
    }
}

void LayoutEngine::layoutInlineChildren(BoxNode* node, LayoutContext& ctx) {
    layoutInline(node, ctx);
}

// ==================================================================
// Flex layout
// ==================================================================

void LayoutEngine::layoutFlex(BoxNode* node, LayoutContext& ctx) {
    auto& lb = node->layoutBox();
    auto& cv = node->computed();

    float resolvedWidth = resolveWidth(node, ctx);
    if (resolvedWidth >= 0) {
        lb.width = resolvedWidth + lb.paddingLeft + lb.paddingRight + lb.borderLeft + lb.borderRight;
    } else {
        lb.width = ctx.containingBlockWidth - lb.marginLeft - lb.marginRight;
    }

    float contentWidth = lb.width - lb.paddingLeft - lb.paddingRight - lb.borderLeft - lb.borderRight;
    bool isRow = (cv.flexDirection == 0 || cv.flexDirection == 1); // row/row-reverse
    bool isReverse = (cv.flexDirection == 1 || cv.flexDirection == 3);
    float gap = cv.rowGap;

    // Measure all flex items
    struct FlexItem {
        BoxNode* node;
        float flexGrow;
        float flexShrink;
        float flexBasis;
        float hypotheticalMainSize;
        float crossSize;
    };

    std::vector<FlexItem> items;
    float totalBasis = 0;
    int flexibleCount = 0;

    LayoutContext childCtx;
    childCtx.containingBlockWidth = contentWidth;
    childCtx.containingBlockHeight = ctx.containingBlockHeight;
    childCtx.viewportWidth = ctx.viewportWidth;
    childCtx.viewportHeight = ctx.viewportHeight;
    childCtx.dpr = ctx.dpr;

    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;

        layoutNode(child.get(), childCtx);

        FlexItem item;
        item.node = child.get();
        auto& childCV = child->computed();
        item.flexGrow = childCV.flexGrow;
        item.flexShrink = childCV.flexShrink;

        if (childCV.flexBasis > 0) {
            item.flexBasis = childCV.flexBasis;
        } else {
            item.flexBasis = isRow ? child->layoutBox().width : child->layoutBox().height;
        }

        item.hypotheticalMainSize = item.flexBasis;
        item.crossSize = isRow ? child->layoutBox().height : child->layoutBox().width;

        totalBasis += item.flexBasis;
        if (item.flexGrow > 0) flexibleCount++;
        items.push_back(item);
    }

    // Distribute free space
    float mainSize = isRow ? contentWidth : (lb.height - lb.paddingTop - lb.paddingBottom - lb.borderTop - lb.borderBottom);
    if (mainSize <= 0) mainSize = contentWidth;

    float totalGaps = gap * std::max(0, static_cast<int>(items.size()) - 1);
    float freeSpace = mainSize - totalBasis - totalGaps;

    if (freeSpace > 0) {
        // Grow
        float totalGrow = 0;
        for (auto& item : items) totalGrow += item.flexGrow;
        if (totalGrow > 0) {
            for (auto& item : items) {
                item.hypotheticalMainSize += freeSpace * (item.flexGrow / totalGrow);
            }
        }
    } else if (freeSpace < 0) {
        // Shrink
        float totalShrink = 0;
        for (auto& item : items) totalShrink += item.flexShrink * item.flexBasis;
        if (totalShrink > 0) {
            for (auto& item : items) {
                float ratio = (item.flexShrink * item.flexBasis) / totalShrink;
                item.hypotheticalMainSize += freeSpace * ratio;
                item.hypotheticalMainSize = std::max(0.0f, item.hypotheticalMainSize);
            }
        }
    }

    // Position items
    float mainOffset = 0;
    float maxCross = 0;

    auto positionItems = [&](bool reverse) {
        for (size_t i = 0; i < items.size(); i++) {
            size_t idx = reverse ? (items.size() - 1 - i) : i;
            auto& item = items[idx];
            auto& childLB = item.node->layoutBox();

            if (isRow) {
                childLB.width = item.hypotheticalMainSize;
                childLB.x = lb.x + lb.borderLeft + lb.paddingLeft + mainOffset;
                childLB.y = lb.y + lb.borderTop + lb.paddingTop;
            } else {
                childLB.height = item.hypotheticalMainSize;
                childLB.x = lb.x + lb.borderLeft + lb.paddingLeft;
                childLB.y = lb.y + lb.borderTop + lb.paddingTop + mainOffset;
            }

            mainOffset += item.hypotheticalMainSize + gap;
            maxCross = std::max(maxCross, item.crossSize);
        }
    };
    positionItems(isReverse);

    // Resolve height
    float resolvedHeight = resolveHeight(node, ctx);
    if (resolvedHeight >= 0) {
        lb.height = resolvedHeight + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    } else if (isRow) {
        lb.height = maxCross + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    } else {
        lb.height = mainOffset - gap + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    }

    // Cross-axis alignment
    float crossSize = isRow
        ? (lb.height - lb.paddingTop - lb.paddingBottom - lb.borderTop - lb.borderBottom)
        : (lb.width - lb.paddingLeft - lb.paddingRight - lb.borderLeft - lb.borderRight);

    for (auto& item : items) {
        auto& childLB = item.node->layoutBox();
        auto& childCV = item.node->computed();
        int alignSelf = childCV.alignSelf;
        if (alignSelf == 0) alignSelf = cv.alignItems; // inherit from container

        float itemCross = item.crossSize;
        float space = crossSize - itemCross;

        if (space > 0) {
            switch (alignSelf) {
                case 1: // flex-start (default)
                    break;
                case 2: // flex-end
                    if (isRow) childLB.y += space;
                    else childLB.x += space;
                    break;
                case 3: // center
                    if (isRow) childLB.y += space / 2;
                    else childLB.x += space / 2;
                    break;
                case 4: // stretch
                    if (isRow) childLB.height = crossSize;
                    else childLB.width = crossSize;
                    break;
                default:
                    break;
            }
        }
    }
}

// ==================================================================
// Grid layout (simplified)
// ==================================================================

void LayoutEngine::layoutGrid(BoxNode* node, LayoutContext& ctx) {
    // Delegate to block for now — full grid solver is in grid.cpp
    layoutBlock(node, ctx);
}

// ==================================================================
// Table layout (simplified)
// ==================================================================

void LayoutEngine::layoutTable(BoxNode* node, LayoutContext& ctx) {
    layoutBlock(node, ctx);
}

// ==================================================================
// Positioned elements
// ==================================================================

void LayoutEngine::layoutPositioned(BoxNode* node, LayoutContext& ctx) {
    auto& lb = node->layoutBox();
    auto& cv = node->computed();

    float cbWidth = ctx.containingBlockWidth;
    float cbHeight = ctx.containingBlockHeight;

    if (cv.position == 3) { // fixed
        cbWidth = ctx.viewportWidth;
        cbHeight = ctx.viewportHeight;
    }

    // Resolve dimensions first
    float resolvedWidth = resolveWidth(node, ctx);
    float resolvedHeight = resolveHeight(node, ctx);

    if (resolvedWidth >= 0) {
        lb.width = resolvedWidth + lb.paddingLeft + lb.paddingRight + lb.borderLeft + lb.borderRight;
    } else {
        lb.width = shrinkToFitWidth(node, ctx);
    }

    // Layout children to get content height if needed
    LayoutContext childCtx = ctx;
    childCtx.containingBlockWidth = lb.width - lb.paddingLeft - lb.paddingRight - lb.borderLeft - lb.borderRight;
    float childY = 0;
    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;
        layoutNode(child.get(), childCtx);
        auto& childLB = child->layoutBox();
        childLB.x = lb.x + lb.borderLeft + lb.paddingLeft + childLB.marginLeft;
        childLB.y = lb.y + lb.borderTop + lb.paddingTop + childY;
        childY += childLB.height + childLB.marginBottom;
    }

    if (resolvedHeight >= 0) {
        lb.height = resolvedHeight + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    } else {
        lb.height = childY + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;
    }

    // Position using inset properties (top/right/bottom/left)
    if (!cv.topAuto && cv.top >= 0) lb.y = cv.top + lb.marginTop;
    else if (!cv.bottomAuto && cv.bottom >= 0) lb.y = cbHeight - cv.bottom - lb.height - lb.marginBottom;

    if (!cv.leftAuto && cv.left >= 0) lb.x = cv.left + lb.marginLeft;
    else if (!cv.rightAuto && cv.right >= 0) lb.x = cbWidth - cv.right - lb.width - lb.marginRight;
}

// ==================================================================
// Float placement
// ==================================================================

void LayoutEngine::placeFloat(BoxNode* node, LayoutContext& ctx) {
    auto& lb = node->layoutBox();

    // Shrink-to-fit width
    lb.width = shrinkToFitWidth(node, ctx);

    // Layout children
    LayoutContext childCtx = ctx;
    childCtx.containingBlockWidth = lb.width - lb.paddingLeft - lb.paddingRight - lb.borderLeft - lb.borderRight;
    float childY = 0;
    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;
        layoutNode(child.get(), childCtx);
        childY += child->layoutBox().height;
    }
    lb.height = childY + lb.paddingTop + lb.paddingBottom + lb.borderTop + lb.borderBottom;

    // Place at current BFC position
    bool isLeft = (node->computed().floatVal == 1);
    if (isLeft) {
        lb.x = ctx.leftEdgeAt(ctx.currentY);
    } else {
        lb.x = ctx.rightEdgeAt(ctx.currentY, ctx.containingBlockWidth) - lb.width;
    }
    lb.y = ctx.currentY;

    // Register float
    LayoutContext::FloatRect fr;
    fr.x = lb.x; fr.y = lb.y;
    fr.width = lb.width; fr.height = lb.height;
    fr.isLeft = isLeft;
    ctx.floats.push_back(fr);
}

// ==================================================================
// CSS length resolution
// ==================================================================

float LayoutEngine::resolveLength(const std::string& value, float referenceSize,
                                     float viewportWidth, float viewportHeight) const {
    if (value.empty() || value == "auto" || value == "none") return -1;

    float num = std::strtof(value.c_str(), nullptr);

    if (value.back() == '%') return num * referenceSize / 100.0f;
    if (value.find("vw") != std::string::npos) return num * viewportWidth / 100.0f;
    if (value.find("vh") != std::string::npos) return num * viewportHeight / 100.0f;
    if (value.find("vmin") != std::string::npos) return num * std::min(viewportWidth, viewportHeight) / 100.0f;
    if (value.find("vmax") != std::string::npos) return num * std::max(viewportWidth, viewportHeight) / 100.0f;
    if (value.find("em") != std::string::npos) return num * 16.0f; // Base em; callers re-resolve via ComputedValues::fontSize
    if (value.find("rem") != std::string::npos) return num * 16.0f; // Root em; matches html default

    return num; // px or unitless
}

float LayoutEngine::resolveWidth(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.width <= 0) return -1;
    return cv.width; // Already in px from cascade
}

float LayoutEngine::resolveHeight(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.height <= 0) return -1;
    return cv.height;
}

float LayoutEngine::resolveMinWidth(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.minWidth <= 0) return -1;
    return cv.minWidth;
}

float LayoutEngine::resolveMaxWidth(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.maxWidth <= 0) return -1;
    return cv.maxWidth;
}

float LayoutEngine::resolveMinHeight(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.minHeight <= 0) return -1;
    return cv.minHeight;
}

float LayoutEngine::resolveMaxHeight(BoxNode* node, const LayoutContext& ctx) const {
    auto& cv = node->computed();
    if (cv.maxHeight <= 0) return -1;
    return cv.maxHeight;
}

float LayoutEngine::collapseMargins(float marginA, float marginB) const {
    if (marginA >= 0 && marginB >= 0) return std::max(marginA, marginB);
    if (marginA < 0 && marginB < 0) return std::min(marginA, marginB);
    return marginA + marginB;
}

float LayoutEngine::shrinkToFitWidth(BoxNode* node, const LayoutContext& ctx) {
    // Shrink-to-fit: max(min-content, min(max-content, available))
    float available = ctx.containingBlockWidth - node->layoutBox().marginLeft - node->layoutBox().marginRight;

    // Approximate max-content: layout with infinite width, measure
    float maxContent = 0;
    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;
        float childW = child->layoutBox().width + child->layoutBox().marginLeft + child->layoutBox().marginRight;
        maxContent = std::max(maxContent, childW);
    }

    // min-content: narrowest without overflow
    float minContent = 0;
    for (auto& child : node->children()) {
        if (child->isTextNode()) {
            // Approximate: longest word
            float fontSize = child->computed().fontSize;
            if (fontSize <= 0) fontSize = 16.0f;
            float charWidth = fontSize * 0.55f;

            const std::string& text = child->text();
            size_t maxWordLen = 0;
            size_t currentLen = 0;
            for (char c : text) {
                if (c == ' ' || c == '\n' || c == '\t') {
                    maxWordLen = std::max(maxWordLen, currentLen);
                    currentLen = 0;
                } else {
                    currentLen++;
                }
            }
            maxWordLen = std::max(maxWordLen, currentLen);
            minContent = std::max(minContent, maxWordLen * charWidth);
        } else {
            minContent = std::max(minContent, child->layoutBox().width);
        }
    }

    float result = std::max(minContent, std::min(maxContent, available));
    auto& lb = node->layoutBox();
    return result + lb.paddingLeft + lb.paddingRight + lb.borderLeft + lb.borderRight;
}

// ==================================================================
// LayoutContext float tracking
// ==================================================================

float LayoutEngine::LayoutContext::availableWidthAt(float y, float blockWidth) const {
    return rightEdgeAt(y, blockWidth) - leftEdgeAt(y);
}

float LayoutEngine::LayoutContext::leftEdgeAt(float y) const {
    float edge = 0;
    for (const auto& f : floats) {
        if (f.isLeft && y >= f.y && y < f.y + f.height) {
            edge = std::max(edge, f.x + f.width);
        }
    }
    return edge;
}

float LayoutEngine::LayoutContext::rightEdgeAt(float y, float blockWidth) const {
    float edge = blockWidth;
    for (const auto& f : floats) {
        if (!f.isLeft && y >= f.y && y < f.y + f.height) {
            edge = std::min(edge, f.x);
        }
    }
    return edge;
}

float LayoutEngine::LayoutContext::clearance(bool left, bool right) const {
    float clear = 0;
    for (const auto& f : floats) {
        if ((left && f.isLeft) || (right && !f.isLeft)) {
            clear = std::max(clear, f.y + f.height);
        }
    }
    return clear;
}

// ==================================================================
// InlineLayoutEngine
// ==================================================================

std::vector<InlineLayoutEngine::LineBox> InlineLayoutEngine::layout(
    BoxNode* inlineContainer, float availableWidth, float startY) {

    std::vector<LineBox> lines;
    auto items = generateInlineItems(inlineContainer);
    if (items.empty()) return lines;

    LineBox currentLine;
    currentLine.y = startY;
    currentLine.availableWidth = availableWidth;
    float xPos = 0;

    for (size_t i = 0; i < items.size(); i++) {
        auto& item = items[i];

        if (item.type == InlineItem::Type::LineBreak) {
            computeLineMetrics(currentLine);
            lines.push_back(currentLine);
            currentLine = LineBox();
            currentLine.y = lines.back().y + lines.back().height;
            currentLine.availableWidth = availableWidth;
            xPos = 0;
            continue;
        }

        if (item.type == InlineItem::Type::Text) {
            float fontSize = item.node ? item.node->computed().fontSize : 16.0f;
            if (fontSize <= 0) fontSize = 16.0f;
            measureText(item, fontSize);
        }

        // Check if item fits on current line
        if (xPos + item.width > availableWidth && xPos > 0 && item.breakable) {
            // Wrap to new line
            computeLineMetrics(currentLine);
            lines.push_back(currentLine);
            currentLine = LineBox();
            currentLine.y = lines.back().y + lines.back().height;
            currentLine.availableWidth = availableWidth;
            xPos = 0;

            // If text item is too wide, break it
            if (item.type == InlineItem::Type::Text && item.width > availableWidth) {
                float fontSize = item.node ? item.node->computed().fontSize : 16.0f;
                size_t breakIdx = findBreakPoint(item.text, availableWidth, fontSize, 0);
                if (breakIdx > 0 && breakIdx < item.text.size()) {
                    // Split item
                    InlineItem remainder = item;
                    remainder.text = item.text.substr(breakIdx);
                    remainder.textStartIndex = item.textStartIndex + breakIdx;
                    item.text = item.text.substr(0, breakIdx);
                    item.textEndIndex = item.textStartIndex + breakIdx;
                    measureText(item, fontSize);
                    measureText(remainder, fontSize);
                    // Insert remainder back
                    items.insert(items.begin() + static_cast<ptrdiff_t>(i + 1), remainder);
                }
            }
        }

        item.width = std::max(0.0f, item.width);
        currentLine.items.push_back(item);
        currentLine.width += item.width;
        xPos += item.width;
    }

    // Final line
    if (!currentLine.items.empty()) {
        computeLineMetrics(currentLine);
        lines.push_back(currentLine);
    }

    // Apply text-align to all lines
    std::string textAlign = "left";
    if (inlineContainer) {
        auto& cv = inlineContainer->computed();
        // textAlign is uint8_t: 0=left, 1=right, 2=center, 3=justify
        switch (cv.textAlign) {
            case 1: textAlign = "right"; break;
            case 2: textAlign = "center"; break;
            case 3: textAlign = "justify"; break;
            default: textAlign = "left"; break;
        }
    }
    for (size_t i = 0; i < lines.size(); i++) {
        alignLine(lines[i], textAlign, i == lines.size() - 1);
    }

    return lines;
}

std::vector<InlineLayoutEngine::InlineItem> InlineLayoutEngine::generateInlineItems(BoxNode* node) {
    std::vector<InlineItem> items;
    if (!node) return items;

    for (auto& child : node->children()) {
        if (child->boxType() == BoxType::None) continue;

        if (child->isTextNode()) {
            std::string whiteSpace = "normal";
            if (!child->computed().whiteSpace.empty()) {
                whiteSpace = child->computed().whiteSpace;
            }

            std::string text = collapseWhiteSpace(child->text(), whiteSpace);
            if (text.empty()) continue;

            // Split on existing line breaks
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();

                if (nl > pos) {
                    InlineItem item;
                    item.type = InlineItem::Type::Text;
                    item.node = child.get();
                    item.text = text.substr(pos, nl - pos);
                    item.textStartIndex = pos;
                    item.textEndIndex = nl;
                    item.breakable = (whiteSpace != "nowrap" && whiteSpace != "pre");
                    items.push_back(item);
                }

                if (nl < text.size()) {
                    InlineItem br;
                    br.type = InlineItem::Type::LineBreak;
                    br.node = child.get();
                    items.push_back(br);
                }
                pos = nl + 1;
            }
        } else if (child->isInline()) {
            // Inline container — recurse
            auto childItems = generateInlineItems(child.get());
            items.insert(items.end(), childItems.begin(), childItems.end());
        } else if (child->boxType() == BoxType::InlineBlock) {
            InlineItem item;
            item.type = InlineItem::Type::InlineBlockBox;
            item.node = child.get();
            item.width = child->layoutBox().width;
            item.height = child->layoutBox().height;
            item.breakable = true;
            items.push_back(item);
        } else if (child->isReplaced()) {
            InlineItem item;
            item.type = InlineItem::Type::ReplacedElement;
            item.node = child.get();
            item.width = child->layoutBox().width;
            item.height = child->layoutBox().height;
            item.breakable = true;
            items.push_back(item);
        }
    }

    return items;
}

void InlineLayoutEngine::measureText(InlineItem& item, float fontSize) {
    float charWidth = fontSize * 0.55f;
    item.width = item.text.size() * charWidth;
    item.height = fontSize * 1.4f;
    item.baseline = fontSize * 0.8f;
}

size_t InlineLayoutEngine::findBreakPoint(const std::string& text, float maxWidth,
                                              float fontSize, size_t startIndex) {
    float charWidth = fontSize * 0.55f;
    int maxChars = static_cast<int>(maxWidth / charWidth);
    if (maxChars < 1) maxChars = 1;

    size_t end = std::min(startIndex + static_cast<size_t>(maxChars), text.size());

    // Walk back to find a word boundary
    size_t breakPoint = end;
    while (breakPoint > startIndex && text[breakPoint] != ' ' &&
           text[breakPoint] != '-' && text[breakPoint] != '\t') {
        breakPoint--;
    }

    if (breakPoint <= startIndex) {
        // No word boundary — force break
        return end;
    }

    return breakPoint + 1; // Break after space/hyphen
}

void InlineLayoutEngine::computeLineMetrics(LineBox& line) {
    line.maxAscent = 0;
    line.maxDescent = 0;

    for (const auto& item : line.items) {
        float ascent = item.baseline;
        float descent = item.height - item.baseline;
        line.maxAscent = std::max(line.maxAscent, ascent);
        line.maxDescent = std::max(line.maxDescent, descent);
    }

    line.height = line.maxAscent + line.maxDescent;
    line.baseline = line.maxAscent;

    // Position items on baseline
    float x = line.x;
    for (auto& item : line.items) {
        // Align to baseline
        float yOffset = line.maxAscent - item.baseline;
        item.width = item.width; // already set
        x += item.width;
    }
}

void InlineLayoutEngine::alignLine(LineBox& line, const std::string& textAlign, bool isLastLine) {
    float freeSpace = line.availableWidth - line.width;
    if (freeSpace <= 0) return;

    if (textAlign == "center") {
        line.x += freeSpace / 2;
    } else if (textAlign == "right" || textAlign == "end") {
        line.x += freeSpace;
    } else if (textAlign == "justify" && !isLastLine) {
        // Distribute space between words
        int spaceCount = 0;
        for (const auto& item : line.items) {
            if (item.type == InlineItem::Type::WhiteSpace) spaceCount++;
        }
        if (spaceCount > 0) {
            float extraPerSpace = freeSpace / spaceCount;
            float xOffset = 0;
            for (auto& item : line.items) {
                if (item.type == InlineItem::Type::WhiteSpace) {
                    xOffset += extraPerSpace;
                }
                // Items would need x position tracking
            }
        }
    }
    // left/start = default, no adjustment
}

void InlineLayoutEngine::verticalAlignItems(LineBox& line) {
    // vertical-align resolution: all items baseline-aligned by default
    // (handled in computeLineMetrics). Sub/super/top/bottom/middle
    // require vertical-align on ComputedValues — baseline is sufficient
    // for initial rendering.
    (void)line;
}

std::string InlineLayoutEngine::collapseWhiteSpace(const std::string& text,
                                                       const std::string& whiteSpaceProperty) {
    if (whiteSpaceProperty == "pre" || whiteSpaceProperty == "pre-wrap" ||
        whiteSpaceProperty == "break-spaces") {
        return text; // Preserve all whitespace
    }

    std::string result;
    result.reserve(text.size());
    bool lastWasSpace = false;

    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!lastWasSpace) {
                result += ' ';
                lastWasSpace = true;
            }
        } else if (c == '\n') {
            if (whiteSpaceProperty == "pre-line") {
                result += '\n';
                lastWasSpace = false;
            } else {
                if (!lastWasSpace) {
                    result += ' ';
                    lastWasSpace = true;
                }
            }
        } else {
            result += c;
            lastWasSpace = false;
        }
    }

    return result;
}

} // namespace Web
} // namespace NXRender
