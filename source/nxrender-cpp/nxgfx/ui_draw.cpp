// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ui_draw.cpp
 * @brief NXRender UI Draw Layer implementation.
 *
 * All drawing is forwarded to NXRender::GpuContext which handles
 * OpenGL batching, clipping, and text rasterization via FreeType.
 */

#include "nxgfx/ui_draw.h"
#include "nxrender_cpp.h"
#include <algorithm>
#include <cstring>

namespace NXRender {

// ============================================================================
// UIPalette
// ============================================================================

const UIPalette& UIPalette::dark() {
    static const UIPalette p;  // Dark palette is default-constructed
    return p;
}

const UIPalette& UIPalette::light() {
    // Light theme overrides
    static UIPalette p = [] {
        UIPalette l;
        l.background      = Color(242, 242, 247);
        l.surface         = Color(255, 255, 255);
        l.surfaceHigh     = Color(230, 230, 235);
        l.surfaceBorder   = Color(198, 198, 200);
        l.textPrimary     = Color(  0,   0,   0);
        l.textSecondary   = Color( 60,  60,  67);
        l.textPlaceholder = Color(180, 180, 184);
        l.inputBackground = Color(255, 255, 255);
        l.inputBorder     = Color(198, 198, 200);
        l.modalBackground = Color(255, 255, 255);
        l.modalBorder     = Color(198, 198, 200);
        l.modalBackdrop   = Color(  0,   0,   0, 120);
        return l;
    }();
    return p;
}

// ============================================================================
// UIDrawContext
// ============================================================================

UIDrawContext UIDrawContext::fromGlobal() {
    UIDrawContext ctx;
    ctx.gpu     = NXRender::gpuContext();
    ctx.palette = &UIPalette::dark();
    ctx.width   = NXRender::viewportWidth();
    ctx.height  = NXRender::viewportHeight();
    return ctx;
}

// ============================================================================
// UIDrawer implementation
// ============================================================================

namespace UIDrawer {

// ----------------------------------------------------------------------------
// Backdrop — full-screen semi-transparent overlay for modals
// ----------------------------------------------------------------------------
void drawBackdrop(UIDrawContext& ctx, float opacity) {
    if (!ctx.gpu) return;
    uint8_t alpha = static_cast<uint8_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
    ctx.gpu->fillRect(
        Rect(0.0f, 0.0f,
             static_cast<float>(ctx.width),
             static_cast<float>(ctx.height)),
        Color(0, 0, 0, alpha)
    );
}

// ----------------------------------------------------------------------------
// Panel / Card — rounded rect with border and drop shadow
// ----------------------------------------------------------------------------
void drawPanel(UIDrawContext& ctx, const Rect& rect, float cornerRadius,
               const Color& bg, const Color& border) {
    if (!ctx.gpu) return;

    // Subtle shadow
    ctx.gpu->drawShadow(
        rect.offset(0.0f, 2.0f),
        Color(0, 0, 0, 80),
        /*blur=*/16.0f,
        /*offsetX=*/0.0f,
        /*offsetY=*/4.0f
    );

    // Background fill
    ctx.gpu->fillRoundedRect(rect, bg, cornerRadius);

    // Border stroke
    ctx.gpu->strokeRoundedRect(rect, border, cornerRadius, 1.0f);
}

// ----------------------------------------------------------------------------
// Button
// ----------------------------------------------------------------------------
void drawButton(UIDrawContext& ctx, const Rect& rect,
                const std::string& label,
                ButtonState state, bool primary,
                float fontSize, float cornerRadius) {
    if (!ctx.gpu) return;

    Color bg, textColor;

    if (primary) {
        switch (state) {
            case ButtonState::Hover:    bg = Color( 32, 148, 255); break;
            case ButtonState::Pressed:  bg = Color(  0, 112, 230); break;
            case ButtonState::Disabled: bg = Color( 80, 120, 180, 128); break;
            default:                    bg = Color( 10, 132, 255); break;
        }
        textColor = Color(255, 255, 255);
    } else {
        // Secondary (ghost) button
        switch (state) {
            case ButtonState::Hover:    bg = Color(255, 255, 255,  30); break;
            case ButtonState::Pressed:  bg = Color(255, 255, 255,  50); break;
            case ButtonState::Disabled: bg = Color(255, 255, 255,  10); break;
            default:                    bg = Color(255, 255, 255,  15); break;
        }
        textColor = (state == ButtonState::Disabled)
                    ? Color(174, 174, 178, 128)
                    : Color(174, 174, 178);
        ctx.gpu->strokeRoundedRect(rect, Color(72, 72, 74), cornerRadius, 1.0f);
    }

    ctx.gpu->fillRoundedRect(rect, bg, cornerRadius);

    // Centered label
    Size textSize = ctx.gpu->measureText(label, fontSize);
    float textX = rect.x + (rect.width  - textSize.width)  * 0.5f;
    float textY = rect.y + (rect.height - textSize.height) * 0.5f + textSize.height * 0.75f;
    ctx.gpu->drawText(label, textX, textY, textColor, fontSize);
}

// ----------------------------------------------------------------------------
// Input Field
// ----------------------------------------------------------------------------
void drawInputField(UIDrawContext& ctx,
                    const Rect& rect,
                    const std::string& text,
                    const std::string& placeholder,
                    bool focused,
                    bool passwordMode,
                    int cursorPos,
                    float fontSize,
                    float cornerRadius) {
    if (!ctx.gpu) return;

    // Background
    ctx.gpu->fillRoundedRect(rect, Color(44, 44, 46), cornerRadius);

    // Border — accent when focused, dim otherwise
    Color borderColor = focused ? Color(10, 132, 255) : Color(72, 72, 74);
    float borderWidth = focused ? 2.0f : 1.0f;
    ctx.gpu->strokeRoundedRect(rect, borderColor, cornerRadius, borderWidth);

    // Clip text to field bounds (with padding)
    const float padX = 12.0f;
    const float padY =  8.0f;
    ctx.gpu->pushClip(rect.inset(2.0f));

    std::string display = text;
    Color textColor = Color(255, 255, 255);

    if (text.empty()) {
        // Placeholder
        display   = placeholder;
        textColor = Color(99, 99, 102);
    } else if (passwordMode) {
        display = maskText(text);
    }

    float textX = rect.x + padX;
    float textY = rect.y + padY + fontSize;
    ctx.gpu->drawText(display, textX, textY, textColor, fontSize);

    // Cursor
    if (focused && cursorPos >= 0 && !text.empty()) {
        std::string before = passwordMode
            ? std::string(cursorPos, 0x2022)
            : text.substr(0, static_cast<size_t>(cursorPos));
        Size measured = ctx.gpu->measureText(before, fontSize);
        float cursorX = textX + measured.width;
        ctx.gpu->drawLine(
            cursorX, rect.y + padY,
            cursorX, rect.y + rect.height - padY,
            Color(10, 132, 255), 2.0f
        );
    } else if (focused && text.empty()) {
        // Cursor at start when empty
        ctx.gpu->drawLine(
            textX, rect.y + padY,
            textX, rect.y + rect.height - padY,
            Color(10, 132, 255), 2.0f
        );
    }

    ctx.gpu->popClip();
}

// ----------------------------------------------------------------------------
// Label
// ----------------------------------------------------------------------------
void drawLabel(UIDrawContext& ctx,
               float x, float y,
               const std::string& text,
               const Color& color,
               float fontSize,
               UITextAlign align,
               float maxWidth) {
    if (!ctx.gpu || text.empty()) return;

    float drawX = x;
    if (align != UITextAlign::Left && maxWidth > 0.0f) {
        Size s = ctx.gpu->measureText(text, fontSize);
        if (align == UITextAlign::Center)
            drawX = x + (maxWidth - s.width) * 0.5f;
        else if (align == UITextAlign::Right)
            drawX = x + maxWidth - s.width;
    }

    ctx.gpu->drawText(text, drawX, y, color, fontSize);
}

void drawErrorLabel(UIDrawContext& ctx,
                    float x, float y,
                    const std::string& text,
                    float fontSize) {
    drawLabel(ctx, x, y, text, Color(255, 69, 58), fontSize);
}

// ----------------------------------------------------------------------------
// Separator
// ----------------------------------------------------------------------------
void drawSeparator(UIDrawContext& ctx,
                   float x, float y,
                   float width,
                   const Color& color) {
    if (!ctx.gpu) return;
    ctx.gpu->drawLine(x, y, x + width, y, color, 1.0f);
}

// ----------------------------------------------------------------------------
// Password mask
// ----------------------------------------------------------------------------
std::string maskText(const std::string& text, char maskChar) {
    // Use bullet U+2022 (UTF-8: E2 80 A2) if maskChar == 0x2022 sentinel
    const char bullet[] = {'\xe2', '\x80', '\xa2', '\0'};
    std::string result;
    result.reserve(text.size() * 3);
    for (size_t i = 0; i < text.size(); ++i)
        result += bullet;
    return result;
}

// ----------------------------------------------------------------------------
// Focus ring
// ----------------------------------------------------------------------------
void drawFocusRing(UIDrawContext& ctx,
                   const Rect& rect,
                   float cornerRadius,
                   float lineWidth) {
    if (!ctx.gpu) return;
    // Slightly expanded accent-colored stroke
    ctx.gpu->strokeRoundedRect(
        rect.inset(-2.0f),
        Color(10, 132, 255, 180),
        cornerRadius + 2.0f,
        lineWidth
    );
}

} // namespace UIDrawer

} // namespace NXRender
