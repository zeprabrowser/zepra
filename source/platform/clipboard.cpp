// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file clipboard.cpp
 * @brief Cross-platform clipboard implementation
 *
 * Windows: Uses Win32 OpenClipboard/SetClipboardData/GetClipboardData
 * Linux/NeolyxOS: Stub — integrate with X11/Wayland via platform layer
 */

#include "platform/clipboard.hpp"
#include "platform/platform_compat.h"
#include <cstdint>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace Zepra::Platform {

Clipboard& Clipboard::instance() {
    static Clipboard instance;
    return instance;
}

bool Clipboard::setText(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    // Allocate global memory for the text (CF_TEXT expects null-terminated)
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hMem) { CloseClipboard(); return false; }

    char* ptr = static_cast<char*>(GlobalLock(hMem));
    if (!ptr) { GlobalFree(hMem); CloseClipboard(); return false; }

    memcpy(ptr, text.c_str(), text.size() + 1);
    GlobalUnlock(hMem);

    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
    return true;
#else
    // Linux/NeolyxOS: delegate to X11/Wayland platform layer when available
    (void)text;
    return false;
#endif
}

std::optional<std::string> Clipboard::getText() {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return std::nullopt;

    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return std::nullopt; }

    const char* ptr = static_cast<const char*>(GlobalLock(hData));
    if (!ptr) { CloseClipboard(); return std::nullopt; }

    std::string result(ptr);
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
#else
    return std::nullopt;
#endif
}

bool Clipboard::setHtml(const std::string& html) {
#ifdef _WIN32
    // Windows HTML clipboard format requires a specific header
    static const UINT CF_HTML = RegisterClipboardFormatA("HTML Format");
    if (!CF_HTML) return false;

    // Build the HTML clipboard format header
    std::string header =
        "Version:0.9\r\n"
        "StartHTML:00000097\r\n"
        "EndHTML:XXXXXXXX\r\n"
        "StartFragment:00000097\r\n"
        "EndFragment:XXXXXXXX\r\n";
    std::string full = header + html;

    // Patch end offsets
    auto patchOffset = [&](const std::string& tag, size_t offset) {
        size_t pos = full.find(tag);
        if (pos == std::string::npos) return;
        char buf[9];
        snprintf(buf, sizeof(buf), "%08zu", offset);
        full.replace(pos + tag.size(), 8, buf);
    };
    patchOffset("EndHTML:", full.size());
    patchOffset("EndFragment:", full.size());

    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, full.size() + 1);
    if (!hMem) { CloseClipboard(); return false; }

    char* ptr = static_cast<char*>(GlobalLock(hMem));
    if (!ptr) { GlobalFree(hMem); CloseClipboard(); return false; }

    memcpy(ptr, full.c_str(), full.size() + 1);
    GlobalUnlock(hMem);
    SetClipboardData(CF_HTML, hMem);
    CloseClipboard();
    return true;
#else
    (void)html;
    return false;
#endif
}

std::optional<std::string> Clipboard::getHtml() {
    return std::nullopt;
}

bool Clipboard::setImage(const std::vector<uint8_t>& pngData) {
    (void)pngData;
    return false;
}

std::optional<std::vector<uint8_t>> Clipboard::getImage() {
    return std::nullopt;
}

bool Clipboard::setFiles(const std::vector<std::string>& paths) {
    (void)paths;
    return false;
}

std::optional<std::vector<std::string>> Clipboard::getFiles() {
    return std::nullopt;
}

bool Clipboard::hasFormat(const char* format) {
    (void)format;
    return false;
}

std::vector<std::string> Clipboard::availableFormats() {
    return {};
}

void Clipboard::clear() {
#ifdef _WIN32
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
#endif
}

} // namespace Zepra::Platform
