// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/box/box_tree.h"
#include "web/events.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace NXRender {
namespace Web {

// ==================================================================
// FormControlState — shared state for all form controls
// ==================================================================

struct FormControlState {
    std::string value;
    std::string defaultValue;
    std::string name;
    std::string type;
    bool disabled = false;
    bool readOnly = false;
    bool required = false;
    bool checked = false;
    bool indeterminate = false;
    bool valid = true;
    std::string validationMessage;
    std::string placeholder;
    int maxLength = -1;
    int minLength = -1;
    std::string pattern;
    std::string autocomplete = "on";
};

// ==================================================================
// TextInputState — caret, selection, composition for text inputs
// ==================================================================

struct TextInputState {
    std::string text;
    int caretPosition = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
    float scrollOffset = 0;
    bool focused = false;
    bool selecting = false;

    // IME composition
    std::string compositionText;
    int compositionStart = -1;
    int compositionEnd = -1;

    bool hasSelection() const { return selectionStart != selectionEnd; }
    std::string selectedText() const;
    void insertText(const std::string& text);
    void deleteBackward();
    void deleteForward();
    void moveCaret(int delta, bool extend = false);
    void moveToStart(bool extend = false);
    void moveToEnd(bool extend = false);
    void selectAll();
    void deleteSelection();
    void moveWordLeft(bool extend = false);
    void moveWordRight(bool extend = false);
};

// ==================================================================
// FormControlRenderer — renders native form controls into BoxNode tree
// ==================================================================

class FormControlRenderer {
public:
    FormControlRenderer();
    ~FormControlRenderer();

    // Render <input> element
    std::unique_ptr<BoxNode> renderInput(const FormControlState& state,
                                            const ComputedValues& cv,
                                            const TextInputState* textState = nullptr);

    // Render <textarea>
    std::unique_ptr<BoxNode> renderTextarea(const FormControlState& state,
                                               const ComputedValues& cv,
                                               const TextInputState* textState = nullptr);

    // Render <select>
    struct SelectOption {
        std::string value;
        std::string label;
        bool selected = false;
        bool disabled = false;
        std::string groupLabel;
    };
    std::unique_ptr<BoxNode> renderSelect(const FormControlState& state,
                                             const ComputedValues& cv,
                                             const std::vector<SelectOption>& options,
                                             bool open = false);

    // Render <button>
    std::unique_ptr<BoxNode> renderButton(const FormControlState& state,
                                             const ComputedValues& cv,
                                             const std::string& label);

    // Render <input type="checkbox">
    std::unique_ptr<BoxNode> renderCheckbox(const FormControlState& state,
                                               const ComputedValues& cv);

    // Render <input type="radio">
    std::unique_ptr<BoxNode> renderRadio(const FormControlState& state,
                                            const ComputedValues& cv);

    // Render <input type="range">
    struct RangeState {
        float min = 0;
        float max = 100;
        float value = 50;
        float step = 1;
    };
    std::unique_ptr<BoxNode> renderRange(const FormControlState& state,
                                            const ComputedValues& cv,
                                            const RangeState& range);

    // Render <progress>
    std::unique_ptr<BoxNode> renderProgress(float value, float max,
                                               const ComputedValues& cv);

    // Render <meter>
    struct MeterState {
        float value = 0;
        float min = 0;
        float max = 1;
        float low = 0;
        float high = 1;
        float optimum = 0.5f;
    };
    std::unique_ptr<BoxNode> renderMeter(const MeterState& state,
                                            const ComputedValues& cv);

    // Render <input type="color">
    std::unique_ptr<BoxNode> renderColorPicker(const std::string& color,
                                                  const ComputedValues& cv);

    // Render <input type="date"> / <input type="time">
    std::unique_ptr<BoxNode> renderDatePicker(const FormControlState& state,
                                                 const ComputedValues& cv);

    // Render <input type="file">
    std::unique_ptr<BoxNode> renderFileInput(const FormControlState& state,
                                                const ComputedValues& cv,
                                                const std::vector<std::string>& files);

private:
    // Shared styling helpers
    ComputedValues baseInputStyle(const ComputedValues& cv);
    ComputedValues focusRingStyle(const ComputedValues& cv);
    ComputedValues disabledStyle(const ComputedValues& cv);

    // Themed colors
    struct ThemeColors {
        uint32_t controlBg = 0xFFFFFFFF;
        uint32_t controlBorder = 0x767676FF;
        uint32_t controlFocusBorder = 0x0078D7FF;
        uint32_t controlFocusRing = 0x0078D740;
        uint32_t controlDisabledBg = 0xF0F0F0FF;
        uint32_t controlDisabledText = 0x999999FF;
        uint32_t placeholderText = 0xA0A0A0FF;
        uint32_t checkboxCheck = 0x0078D7FF;
        uint32_t rangeTrack = 0xD0D0D0FF;
        uint32_t rangeThumb = 0x0078D7FF;
        uint32_t progressBar = 0x0078D7FF;
        uint32_t progressTrack = 0xE0E0E0FF;
        uint32_t selectArrow = 0x404040FF;
        uint32_t buttonBg = 0xF0F0F0FF;
        uint32_t buttonHover = 0xE5E5E5FF;
        uint32_t buttonActiveBg = 0xD0D0D0FF;
    } theme_;
};

// ==================================================================
// FormDataCollector — collects form data for submission
// ==================================================================

class FormDataCollector {
public:
    struct FormEntry {
        std::string name;
        std::string value;
        std::string type;
        std::string filename;
    };

    // Collect all form data from a <form> subtree
    static std::vector<FormEntry> collect(const BoxNode* formRoot);

    // URL-encode form data
    static std::string urlEncode(const std::vector<FormEntry>& entries);

    // Multipart-encode form data
    static std::string multipartEncode(const std::vector<FormEntry>& entries,
                                         std::string& boundary);

    // JSON-encode form data
    static std::string jsonEncode(const std::vector<FormEntry>& entries);

private:
    // Extract form field name from a BoxNode.
    // The DOMBoxBuilder stores the name attribute in ComputedValues::content
    // using the convention "name=fieldname" for form controls.
    static std::string extractFormFieldName(const BoxNode* node);
};

// ==================================================================
// FormValidator — client-side constraint validation API
// ==================================================================

class FormValidator {
public:
    struct ValidityState {
        bool valueMissing = false;
        bool typeMismatch = false;
        bool patternMismatch = false;
        bool tooLong = false;
        bool tooShort = false;
        bool rangeUnderflow = false;
        bool rangeOverflow = false;
        bool stepMismatch = false;
        bool badInput = false;
        bool customError = false;
        bool valid = true;
        std::string message;
    };

    // Validate a single form control
    static ValidityState validate(const FormControlState& state);

    // Check email format
    static bool isValidEmail(const std::string& value);

    // Check URL format
    static bool isValidURL(const std::string& value);

    // Check against pattern
    static bool matchesPattern(const std::string& value, const std::string& pattern);

    // Validate number range
    static bool inRange(float value, float min, float max, float step);
};

} // namespace Web
} // namespace NXRender
