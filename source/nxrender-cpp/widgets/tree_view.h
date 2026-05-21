// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "widgets/widget.h"
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace NXRender {
namespace Widgets {

class TreeNode {
public:
    explicit TreeNode(const std::string& label);
    ~TreeNode() = default;

    std::string label;
    bool expanded = false;
    bool selected = false;
    
    // User pointer for associating with external models
    void* userData = nullptr;

    void addChild(std::unique_ptr<TreeNode> child) {
        children_.push_back(std::move(child));
    }

    const std::vector<std::unique_ptr<TreeNode>>& children() const { return children_; }
    std::vector<std::unique_ptr<TreeNode>>& children() { return children_; }

private:
    std::vector<std::unique_ptr<TreeNode>> children_;
};

class TreeViewWidget : public Widget {
public:
    TreeViewWidget();
    ~TreeViewWidget() override;

    void setRoot(std::unique_ptr<TreeNode> node);
    TreeNode* root() const { return root_.get(); }
    
    void setItemHeight(float height) { itemHeight_ = height; }

    void render(GpuContext* ctx) override;
    EventResult handleRoutedEvent(const Input::Event& event) override;

private:
    std::unique_ptr<TreeNode> root_;
    float itemHeight_ = 24.0f;
    float indentWidth_ = 20.0f;
    
    float scrollY_ = 0.0f;
    float cumulativeY_ = 0.0f;

    TreeNode* hoveredNode_ = nullptr;
    TreeNode* selectedNode_ = nullptr;

    // Helper: recursively renders and measures nodes
    void renderNode(GpuContext* ctx, TreeNode* node, int depth, float& currentY);
    
    // Helper: recursively checks clicks
    TreeNode* hitTestNode(TreeNode* node, int depth, float localX, float localY, float& currentY);
    
    void toggleNodeSelection(TreeNode* node);
};

} // namespace Widgets
} // namespace NXRender
