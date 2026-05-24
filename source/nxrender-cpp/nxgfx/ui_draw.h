// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ui_draw.h
 * @brief Zepra UI Draw Layer — bridges ZepraUI rendering calls to NXRender GpuContext.
 *
 * Provides a stateless, flat API that auth_ui, settings_ui and any
 * future browser UI panels can use to draw:
 *   - Filled/stroked rectangles (flat + rounded)
 *   - Text (single-line, multi-line, clipped)
 *   - Input field backgrounds, cursors, placeholder text
 *   - Buttons with hover/pressed states
 *   - Shadows and borders
 *   - Modals with semi-transparent backdrop
 *
 * All calls forward directly to NXRender::GpuContext — zero extra allocations.
 */

#pragma once

#include "nxgfx/context.h"
#include "nxgfx/color.h"
#include "nxgfx/primitives.h"
#include <string>
#include <cstdint>

namespace NXRender {

// ============================================================================
// UIPalette — semantic colors for browser UI (dark theme)
// ============================================================================
struct UIPalette {
    // Surfaces
    Color background        {18,  18,  18,  255};  // Main window
    Color surface           {28,  28,  30,  255};  // Cards, panels
    Color surfaceHigh       {38,  38,  42,  255};  // Elevated elements
    Color surfaceBorder     {58,  58,  64,  255};  // Borders

    // Interactive
    Color accent            {10, 132, 255,  255};  // Primary blue (macOS-style)
    Color accentHover       {32, 148, 255,  255};
    Color accentPressed     { 0, 112, 230,  255};
    Color danger            {255,  69,  58,  255};  // Destructive action
    Color success           { 48, 209, 88,  255};   // Positive feedback

    // Text
    Color textPrimary       {255, 255, 255,  255};
    Color textSecondary     {174, 174, 178,  255};
    Color textPlaceholder   { 99,  99, 102,  255};
    Color textDisabled      { 72,  72,  74,  255};

    // Input fields
    Color inputBackground   { 44,  44,  46,  255};
    Color inputBorder       { 72,  72,  74,  255};
    Color inputFocusBorder  { 10, 132, 255,  255};

    // Modal
    Color modalBackdrop     {  0,   0,   0,  160};
    Color modalBackground   { 28,  28,  30,  255};
    Color modalBorder       { 58,  58,  64,  255};

    // Cursor/selection
    Color cursor            { 10, 132, 255,  255};
    Color selection         { 10, 132, 255,   80};

    static const UIPalette& dark();
    static const UIPalette& light();
};

// ============================================================================
// UIFont — font configuration
// ============================================================================
struct UIFont {
    std::string family = "system-ui";  // Resolved by NXRender font fallback
    float size         = 14.0f;
    bool  bold         = false;
    bool  italic       = false;
};

// ============================================================================
// UITextAlign
// ============================================================================
enum class UITextAlign { Left, Center, Right };

// ============================================================================
// UIDrawContext — wrapper passed to all UI draw calls
// Thin struct that holds GPU context pointer + palette reference.
// ============================================================================
struct UIDrawContext {
    GpuContext*       gpu     = nullptr;   // NXRender GPU context
    const UIPalette*  palette = nullptr;   // Current palette
    int               width   = 1280;      // Viewport width
    int               height  = 720;       // Viewport height

    // Convenience: build from NXRender global state
    static UIDrawContext fromGlobal();
};

// ============================================================================
// UIDrawer — stateless namespace with all UI drawing primitives
// ============================================================================
namespace UIDrawer {

    // -------------------------------------------------------------------------
    // Helpers — convert ZepraUI NXColor to NXRender Color
    // -------------------------------------------------------------------------
    inline Color toColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return Color(r, g, b, a);
    }

    // -------------------------------------------------------------------------
    // Backdrop (modal overlay)
    // -------------------------------------------------------------------------
    void drawBackdrop(UIDrawContext& ctx, float opacity = 0.65f);

    // -------------------------------------------------------------------------
    // Panel / Card
    // -------------------------------------------------------------------------
    void drawPanel(UIDrawContext& ctx,
                   const Rect& rect,
                   float cornerRadius = 12.0f,
                   const Color& bg = Color(28, 28, 30),
                   const Color& border = Color(58, 58, 64));

    // -------------------------------------------------------------------------
    // Button
    // -------------------------------------------------------------------------
    enum class ButtonState { Normal, Hover, Pressed, Disabled };

    void drawButton(UIDrawContext& ctx,
                    const Rect& rect,
                    const std::string& label,
                    ButtonState state = ButtonState::Normal,
                    bool primary = true,
                    float fontSize = 14.0f,
                    float cornerRadius = 8.0f);

    // -------------------------------------------------------------------------
    // Text Input Field
    // -------------------------------------------------------------------------
    void drawInputField(UIDrawContext& ctx,
                        const Rect& rect,
                        const std::string& text,
                        const std::string& placeholder,
                        bool focused = false,
                        bool passwordMode = false,
                        int cursorPos = -1,
                        float fontSize = 14.0f,
                        float cornerRadius = 8.0f);

    // -------------------------------------------------------------------------
    // Label / Text
    // -------------------------------------------------------------------------
    void drawLabel(UIDrawContext& ctx,
                   float x, float y,
                   const std::string& text,
                   const Color& color = Color(255, 255, 255),
                   float fontSize = 14.0f,
                   UITextAlign align = UITextAlign::Left,
                   float maxWidth = 0.0f);

    // Error label (red)
    void drawErrorLabel(UIDrawContext& ctx,
                        float x, float y,
                        const std::string& text,
                        float fontSize = 13.0f);

    // -------------------------------------------------------------------------
    // Separator
    // -------------------------------------------------------------------------
    void drawSeparator(UIDrawContext& ctx,
                       float x, float y,
                       float width,
                       const Color& color = Color(58, 58, 64));

    // -------------------------------------------------------------------------
    // Password mask helper
    // -------------------------------------------------------------------------
    std::string maskText(const std::string& text, char maskChar = 0x2022);

    // -------------------------------------------------------------------------
    // Focus ring (drawn around focused element)
    // -------------------------------------------------------------------------
    void drawFocusRing(UIDrawContext& ctx,
                       const Rect& rect,
                       float cornerRadius = 8.0f,
                       float lineWidth = 2.0f);

} // namespace UIDrawer

} // namespace NXRender
