// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file auth_ui.h
 * @brief Authentication UI components — NXRender native backend.
 *        SDL2 fully removed. All drawing uses NXRender::UIDrawer + GpuContext.
 */

#ifndef ZEPRA_AUTH_UI_H
#define ZEPRA_AUTH_UI_H

#include "auth/zepra_auth.h"
// NXRender UI drawing layer (GpuContext, UIDrawContext, UIDrawer, UIPalette)
#include "nxgfx/ui_draw.h"
// NXRender unified event system (replaces SDL_Event)
#include "input/events.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

namespace ZepraUI {

// ============================================================================
// Type aliases — map ZepraUI names to NXRender equivalents (zero overhead)
//   NXColor      → NXRender::Color          (RGBA, same 4-byte layout)
//   NXRenderCtx  → NXRender::UIDrawContext   (GpuContext + palette + viewport)
//   NXEvent      → NXRender::Event           (unified mouse/key/text event)
// ============================================================================
using NXColor         = NXRender::Color;
using NXRenderCtx     = NXRender::UIDrawContext;
using NXRenderContext = NXRenderCtx;   // backward-compat alias
using NXEvent         = NXRender::Event;
using NXInputEvent    = NXEvent;       // backward-compat alias

// ============================================================================
// UIElement — base for all auth UI widgets
// ============================================================================
class UIElement {
public:
    UIElement(int x, int y, int width, int height);
    virtual ~UIElement() = default;

    virtual void render(NXRenderCtx& ctx) = 0;
    virtual bool handleEvent(const NXEvent& event) = 0;
    virtual void update() = 0;

    void setPosition(int x, int y);
    void setSize(int width, int height);
    void setVisible(bool visible);
    void setEnabled(bool enabled);

    int  getX()      const { return m_x; }
    int  getY()      const { return m_y; }
    int  getWidth()  const { return m_width; }
    int  getHeight() const { return m_height; }
    bool isVisible() const { return m_visible; }
    bool isEnabled() const { return m_enabled; }

protected:
    int  m_x, m_y, m_width, m_height;
    bool m_visible = true;
    bool m_enabled = true;
    bool m_hovered = false;
    bool m_focused = false;
};

// ============================================================================
// Button — calls UIDrawer::drawButton
// ============================================================================
class Button : public UIElement {
public:
    Button(int x, int y, int width, int height, const std::string& text);
    ~Button() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setText(const std::string& text);
    void setOnClick(std::function<void()> callback);
    void setColors(NXColor normal, NXColor hover, NXColor pressed);
    void setPrimary(bool primary) { m_primary = primary; }

    const std::string& getText() const { return m_text; }

private:
    std::string           m_text;
    std::function<void()> m_onClick;
    NXColor               m_normalColor  { 10, 132, 255, 255};
    NXColor               m_hoverColor   { 32, 148, 255, 255};
    NXColor               m_pressedColor {  0, 112, 230, 255};
    bool                  m_pressed = false;
    bool                  m_primary = true;
};

// ============================================================================
// TextInput — calls UIDrawer::drawInputField
// ============================================================================
class TextInput : public UIElement {
public:
    TextInput(int x, int y, int width, int height, const std::string& placeholder = "");
    ~TextInput() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setText(const std::string& text);
    void setPlaceholder(const std::string& placeholder);
    void setPasswordMode(bool password);
    void setOnTextChange(std::function<void(const std::string&)> callback);
    void setOnEnter(std::function<void()> callback);

    const std::string& getText()        const { return m_text; }
    bool               isPasswordMode() const { return m_passwordMode; }

private:
    std::string                             m_text;
    std::string                             m_placeholder;
    bool                                    m_passwordMode    = false;
    std::function<void(const std::string&)> m_onTextChange;
    std::function<void()>                   m_onEnter;
    int                                     m_cursorPos       = 0;
    bool                                    m_showCursor      = true;
    uint32_t                                m_cursorBlinkTime = 0;
};

// ============================================================================
// Label — calls UIDrawer::drawLabel
// ============================================================================
class Label : public UIElement {
public:
    Label(int x, int y, const std::string& text);
    ~Label() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setText(const std::string& text);
    void setColor(NXColor color);
    void setFontSize(int size);

    const std::string& getText() const { return m_text; }

private:
    std::string m_text;
    NXColor     m_color    {220, 220, 220, 255};
    int         m_fontSize = 14;
};

// ============================================================================
// ModalDialog base — draws panel + backdrop via UIDrawer
// ============================================================================
class ModalDialog {
public:
    ModalDialog(int x, int y, int width, int height, const std::string& title);
    virtual ~ModalDialog() = default;

    virtual void render(NXRenderCtx& ctx);
    virtual bool handleEvent(const NXEvent& event);
    virtual void update();

    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }
    void setOnClose(std::function<void()> callback);

protected:
    int         m_x, m_y, m_width, m_height;
    std::string m_title;
    bool        m_visible = false;
    std::function<void()> m_onClose;
    std::vector<std::unique_ptr<UIElement>> m_elements;

    void addElement(std::unique_ptr<UIElement> element);
    void clearElements();
};

// ============================================================================
// LoginDialog
// ============================================================================
class LoginDialog : public ModalDialog {
public:
    LoginDialog();
    ~LoginDialog() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setOnLogin(std::function<void(const std::string&, const std::string&)> callback);
    void setOnCancel(std::function<void()> callback);
    void setError(const std::string& error);
    void clearError();
    void setEmail(const std::string& email);
    void setPassword(const std::string& password);

private:
    std::function<void(const std::string&, const std::string&)> m_onLogin;
    std::function<void()> m_onCancel;
    std::string m_errorMessage;
    TextInput* m_emailInput    = nullptr;
    TextInput* m_passwordInput = nullptr;
    Button*    m_loginButton   = nullptr;
    Button*    m_cancelButton  = nullptr;
    Label*     m_errorLabel    = nullptr;
    Label*     m_titleLabel    = nullptr;
    Label*     m_emailLabel    = nullptr;
    Label*     m_passwordLabel = nullptr;
};

// ============================================================================
// TwoFactorDialog
// ============================================================================
class TwoFactorDialog : public ModalDialog {
public:
    TwoFactorDialog();
    ~TwoFactorDialog() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setOnVerify(std::function<void(const std::string&)> callback);
    void setOnCancel(std::function<void()> callback);
    void setError(const std::string& error);
    void clearError();
    void setTempToken(const std::string& token);

private:
    std::function<void(const std::string&)> m_onVerify;
    std::function<void()> m_onCancel;
    std::string m_errorMessage;
    std::string m_tempToken;
    TextInput* m_codeInput        = nullptr;
    Button*    m_verifyButton     = nullptr;
    Button*    m_cancelButton     = nullptr;
    Label*     m_errorLabel       = nullptr;
    Label*     m_titleLabel       = nullptr;
    Label*     m_instructionLabel = nullptr;
};

// ============================================================================
// PasswordPromptDialog
// ============================================================================
class PasswordPromptDialog : public ModalDialog {
public:
    PasswordPromptDialog(const std::string& websiteUrl, const std::string& domain);
    ~PasswordPromptDialog() override;

    void render(NXRenderCtx& ctx) override;
    bool handleEvent(const NXEvent& event) override;
    void update() override;

    void setOnSubmit(std::function<void(const std::string&, const std::string&)> callback);
    void setOnCancel(std::function<void()> callback);
    void setError(const std::string& error);
    void clearError();

private:
    std::string m_websiteUrl, m_domain;
    std::function<void(const std::string&, const std::string&)> m_onSubmit;
    std::function<void()> m_onCancel;
    std::string m_errorMessage;
    TextInput* m_usernameInput = nullptr;
    TextInput* m_passwordInput = nullptr;
    Button*    m_submitButton  = nullptr;
    Button*    m_cancelButton  = nullptr;
    Label*     m_errorLabel    = nullptr;
    Label*     m_titleLabel    = nullptr;
    Label*     m_websiteLabel  = nullptr;
    Label*     m_usernameLabel = nullptr;
    Label*     m_passwordLabel = nullptr;
};

// ============================================================================
// AuthUIManager — singleton managing all auth dialogs
// ============================================================================
class AuthUIManager {
public:
    static AuthUIManager& getInstance();

    bool initialize(NXRenderCtx& ctx);
    void shutdown();

    void render(NXRenderCtx& ctx);
    bool handleEvent(const NXEvent& event);
    void update();

    void showLoginDialog();
    void hideLoginDialog();
    void showTwoFactorDialog(const std::string& tempToken);
    void hideTwoFactorDialog();
    void showPasswordPromptDialog(const std::string& websiteUrl, const std::string& domain);
    void hidePasswordPromptDialog();

    void setOnLogin(std::function<void(const std::string&, const std::string&)> callback);
    void setOnTwoFactor(std::function<void(const std::string&)> callback);
    void setOnPasswordPrompt(std::function<void(const std::string&, const std::string&)> callback);

    void setLoginError(const std::string& error);
    void setTwoFactorError(const std::string& error);
    void setPasswordPromptError(const std::string& error);

private:
    AuthUIManager();
    ~AuthUIManager();
    AuthUIManager(const AuthUIManager&) = delete;
    AuthUIManager& operator=(const AuthUIManager&) = delete;

    NXRenderCtx m_ctx;
    std::unique_ptr<LoginDialog>          m_loginDialog;
    std::unique_ptr<TwoFactorDialog>      m_twoFactorDialog;
    std::unique_ptr<PasswordPromptDialog> m_passwordPromptDialog;

    std::function<void(const std::string&, const std::string&)> m_onLogin;
    std::function<void(const std::string&)>                     m_onTwoFactor;
    std::function<void(const std::string&, const std::string&)> m_onPasswordPrompt;
};

// ============================================================================
// AuthUIUtils — thin wrappers around UIDrawer for convenience
// ============================================================================
namespace AuthUIUtils {
    NXColor createColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    void renderText(NXRenderCtx& ctx, const std::string& text,
                    int x, int y, NXColor color, int fontSize = 14);
    void renderRect(NXRenderCtx& ctx, int x, int y, int width, int height,
                    NXColor color, bool filled = true);
    void renderBorder(NXRenderCtx& ctx, int x, int y, int width, int height,
                      NXColor color, int thickness = 1);
    bool isPointInRect(int x, int y, int rx, int ry, int rw, int rh);
    std::string maskPassword(const std::string& password);
}

// ============================================================================
// Named colors (NXRender::Color)
// ============================================================================
namespace Colors {
    constexpr NXColor WHITE      {255, 255, 255, 255};
    constexpr NXColor BLACK      {  0,   0,   0, 255};
    constexpr NXColor GRAY       {128, 128, 128, 255};
    constexpr NXColor LIGHT_GRAY {192, 192, 192, 255};
    constexpr NXColor DARK_GRAY  { 64,  64,  64, 255};
    constexpr NXColor BLUE       { 10, 132, 255, 255};
    constexpr NXColor GREEN      { 48, 209,  88, 255};
    constexpr NXColor RED        {255,  69,  58, 255};
    constexpr NXColor YELLOW     {255, 214,  10, 255};
    constexpr NXColor TRANSPARENT{  0,   0,   0,   0};
}

} // namespace ZepraUI

#endif // ZEPRA_AUTH_UI_H