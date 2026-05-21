// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/box/box_tree.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace NXRender {
namespace Web {

// ==================================================================
// ARIA Role (WAI-ARIA 1.2 roles)
// ==================================================================

enum class AriaRole : uint8_t {
    None,
    // Landmark
    Banner, Complementary, ContentInfo, Form, Main, Navigation, Region, Search,
    // Widget
    Alert, AlertDialog, Button, Checkbox, Combobox, Dialog, Grid, GridCell,
    Link, ListBox, ListItem, Log, Marquee, Menu, MenuBar, MenuItem,
    MenuItemCheckbox, MenuItemRadio, Option, ProgressBar, Radio, RadioGroup,
    ScrollBar, Separator, Slider, SpinButton, Status, Switch, Tab, TabList,
    TabPanel, TextBox, Timer, Toolbar, Tooltip, Tree, TreeGrid, TreeItem,
    // Document structure
    Application, Article, Cell, ColumnHeader, Definition, Directory, Document,
    Feed, Figure, Group, Heading, Img, List, Math, Note, Presentation,
    Row, RowGroup, RowHeader, Table, Term,
    // Generic
    Generic, Text
};

// ==================================================================
// Accessibility node properties
// ==================================================================

struct AccessibilityProperties {
    AriaRole role = AriaRole::None;
    std::string name;               // accessible name
    std::string description;        // accessible description
    std::string value;              // current value
    std::string placeholder;
    std::string label;              // aria-label
    std::string labelledBy;         // aria-labelledby (IDs)
    std::string describedBy;        // aria-describedby (IDs)
    std::string owns;               // aria-owns (IDs)
    std::string controls;           // aria-controls (IDs)
    std::string flowTo;             // aria-flowto (IDs)
    std::string live;               // aria-live: off, polite, assertive
    std::string relevant;           // aria-relevant
    bool atomic = false;            // aria-atomic
    bool busy = false;              // aria-busy

    // States
    bool hidden = false;            // aria-hidden
    bool disabled = false;          // aria-disabled
    bool required = false;          // aria-required
    bool readOnly = false;          // aria-readonly
    bool checked = false;           // aria-checked
    bool pressed = false;           // aria-pressed
    bool expanded = false;          // aria-expanded
    bool selected = false;          // aria-selected
    bool grabbed = false;           // aria-grabbed
    bool modal = false;             // aria-modal
    bool multiSelectable = false;   // aria-multiselectable
    bool multiLine = false;         // aria-multiline

    // Numeric
    float valueMin = 0;             // aria-valuemin
    float valueMax = 100;           // aria-valuemax
    float valueNow = 0;            // aria-valuenow
    std::string valueText;          // aria-valuetext
    int level = 0;                  // aria-level (heading level)
    int setSize = 0;                // aria-setsize
    int posInSet = 0;               // aria-posinset
    int colCount = 0;               // aria-colcount
    int colIndex = 0;               // aria-colindex
    int colSpan = 1;                // aria-colspan
    int rowCount = 0;               // aria-rowcount
    int rowIndex = 0;               // aria-rowindex
    int rowSpan = 1;                // aria-rowspan

    // Sort
    std::string sort;               // aria-sort: none, ascending, descending, other

    // Auto-complete
    std::string autoComplete;       // aria-autocomplete: none, inline, list, both

    // Has popup
    std::string hasPopup;           // aria-haspopup: true, menu, listbox, tree, grid, dialog

    // Drop effect
    std::string dropEffect;         // aria-dropeffect: copy, execute, link, move, none, popup

    // Current
    std::string current;            // aria-current: page, step, location, date, time, true

    // Error message
    std::string errorMessage;       // aria-errormessage (ID)
    std::string invalid;            // aria-invalid: grammar, spelling, true
};

// ==================================================================
// Accessibility tree node
// ==================================================================

class AccessibilityNode {
public:
    AccessibilityNode();
    ~AccessibilityNode();

    // Identity
    int id() const { return id_; }
    AriaRole role() const { return props_.role; }
    const std::string& name() const { return props_.name; }

    // Properties
    AccessibilityProperties& properties() { return props_; }
    const AccessibilityProperties& properties() const { return props_; }

    // Tree structure
    AccessibilityNode* parent() const { return parent_; }
    const std::vector<std::unique_ptr<AccessibilityNode>>& children() const { return children_; }
    void appendChild(std::unique_ptr<AccessibilityNode> child);
    size_t childCount() const { return children_.size(); }

    // DOM linkage
    void setBoxNode(const BoxNode* box) { boxNode_ = box; }
    const BoxNode* boxNode() const { return boxNode_; }

    // Geometry (from layout)
    struct Bounds {
        float x = 0, y = 0;
        float width = 0, height = 0;
    };
    const Bounds& bounds() const { return bounds_; }
    Bounds& bounds() { return bounds_; }

    // Actions
    bool isActionable() const;
    bool isFocusable() const;

private:
    static int nextId_;
    int id_;
    AccessibilityProperties props_;
    AccessibilityNode* parent_ = nullptr;
    std::vector<std::unique_ptr<AccessibilityNode>> children_;
    const BoxNode* boxNode_ = nullptr;
    Bounds bounds_;
};

// ==================================================================
// Accessibility tree builder
// ==================================================================

class AccessibilityTreeBuilder {
public:
    AccessibilityTreeBuilder();
    ~AccessibilityTreeBuilder();

    // Build accessibility tree from box tree
    std::unique_ptr<AccessibilityNode> build(const BoxNode* root);

    // Rebuild subtree (incremental)
    void rebuildSubtree(AccessibilityNode* axNode, const BoxNode* boxNode);

    // Node count
    size_t nodeCount() const { return nodeCount_; }

private:
    size_t nodeCount_ = 0;

    std::unique_ptr<AccessibilityNode> buildNode(const BoxNode* boxNode);

    // Determine ARIA role from tag name
    AriaRole roleFromTag(const std::string& tag) const;

    // Compute accessible name (name computation algorithm)
    std::string computeName(const BoxNode* node) const;

    // Check if node should be in accessibility tree
    bool isAccessible(const BoxNode* node) const;

    // Extract text content recursively
    std::string extractTextContent(const BoxNode* node) const;
};

// ==================================================================
// Accessibility manager — global accessibility state
// ==================================================================

class AccessibilityManager {
public:
    static AccessibilityManager& instance();

    // Build/rebuild the tree
    void buildTree(const BoxNode* rootBox);

    // Get the root of the accessibility tree
    AccessibilityNode* root() const { return root_.get(); }

    // Find node by box node
    AccessibilityNode* findByBox(const BoxNode* box) const;

    // Find node by ID
    AccessibilityNode* findById(int id) const;

    // Focus management
    void setFocusedNode(AccessibilityNode* node);
    AccessibilityNode* focusedNode() const { return focused_; }

    // Announcements (live regions)
    struct Announcement {
        std::string text;
        std::string priority; // polite, assertive
        double timestamp;
    };
    void announce(const std::string& text, const std::string& priority = "polite");
    const std::vector<Announcement>& pendingAnnouncements() const { return announcements_; }
    void clearAnnouncements() { announcements_.clear(); }

    // Platform bridge callbacks
    using NodeCallback = std::function<void(AccessibilityNode*)>;
    void setOnFocusChange(NodeCallback cb) { onFocusChange_ = std::move(cb); }
    void setOnTreeUpdate(std::function<void()> cb) { onTreeUpdate_ = std::move(cb); }
    void setOnAnnouncement(std::function<void(const Announcement&)> cb) { onAnnouncement_ = std::move(cb); }

private:
    AccessibilityManager();

    std::unique_ptr<AccessibilityNode> root_;
    AccessibilityNode* focused_ = nullptr;
    AccessibilityTreeBuilder builder_;
    std::vector<Announcement> announcements_;

    // Lookup
    mutable std::unordered_map<const BoxNode*, AccessibilityNode*> boxToAx_;
    mutable std::unordered_map<int, AccessibilityNode*> idToAx_;
    void buildLookup(AccessibilityNode* node);

    NodeCallback onFocusChange_;
    std::function<void()> onTreeUpdate_;
    std::function<void(const Announcement&)> onAnnouncement_;
};

} // namespace Web
} // namespace NXRender
