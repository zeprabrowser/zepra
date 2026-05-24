// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file auth_ui.cpp
 * @brief Authentication UI — fully wired to NXRender UIDrawer.
 *        SDL2 removed. Every render() call uses GpuContext via UIDrawer.
 */

#include "settings/auth_ui.h"
#include "nxgfx/ui_draw.h"
#include <algorithm>

// Helper: convert pixel coords to NXRender::Rect
static NXRender::Rect toRect(int x, int y, int w, int h) {
    return NXRender::Rect(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(w), static_cast<float>(h));
}

// Helper: check mouse position hit
static bool hitTest(const NXRender::Event& e, int x, int y, int w, int h) {
    if (!e.isMouse()) return false;
    return (e.mouse.x >= x && e.mouse.x < x + w &&
            e.mouse.y >= y && e.mouse.y < y + h);
}

static bool pointInRect(float px, float py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

namespace ZepraUI {

// ============================================================================
// UIElement base
// ============================================================================

UIElement::UIElement(int x, int y, int w, int h)
    : m_x(x), m_y(y), m_width(w), m_height(h) {}

void UIElement::setPosition(int x, int y) { m_x = x; m_y = y; }
void UIElement::setSize(int w, int h)     { m_width = w; m_height = h; }
void UIElement::setVisible(bool v)        { m_visible = v; }
void UIElement::setEnabled(bool e)        { m_enabled = e; }

// ============================================================================
// Button — renders via UIDrawer::drawButton
// ============================================================================

Button::Button(int x, int y, int w, int h, const std::string& text)
    : UIElement(x, y, w, h), m_text(text) {}

Button::~Button() = default;

void Button::render(NXRenderCtx& ctx) {
    if (!m_visible || !ctx.gpu) return;

    NXRender::UIDrawer::ButtonState state;
    if (!m_enabled) {
        state = NXRender::UIDrawer::ButtonState::Disabled;
    } else if (m_pressed) {
        state = NXRender::UIDrawer::ButtonState::Pressed;
    } else if (m_hovered) {
        state = NXRender::UIDrawer::ButtonState::Hover;
    } else {
        state = NXRender::UIDrawer::ButtonState::Normal;
    }

    NXRender::UIDrawer::drawButton(ctx, toRect(m_x, m_y, m_width, m_height),
                                   m_text, state, m_primary);
}

bool Button::handleEvent(const NXEvent& e) {
    if (!m_enabled || !m_visible) return false;

    if (e.type == NXRender::EventType::MouseMove) {
        m_hovered = pointInRect(e.mouse.x, e.mouse.y, m_x, m_y, m_width, m_height);
    }
    if (e.type == NXRender::EventType::MouseDown &&
        pointInRect(e.mouse.x, e.mouse.y, m_x, m_y, m_width, m_height)) {
        m_pressed = true;
        return true;
    }
    if (e.type == NXRender::EventType::MouseUp && m_pressed) {
        bool wasInside = pointInRect(e.mouse.x, e.mouse.y, m_x, m_y, m_width, m_height);
        m_pressed = false;
        if (wasInside && m_onClick) m_onClick();
        return wasInside;
    }
    return false;
}

void Button::update() {}
void Button::setText(const std::string& t) { m_text = t; }
void Button::setOnClick(std::function<void()> cb) { m_onClick = std::move(cb); }
void Button::setColors(NXColor n, NXColor h, NXColor p) {
    m_normalColor = n; m_hoverColor = h; m_pressedColor = p;
}

// ============================================================================
// TextInput — renders via UIDrawer::drawInputField
// ============================================================================

TextInput::TextInput(int x, int y, int w, int h, const std::string& placeholder)
    : UIElement(x, y, w, h), m_placeholder(placeholder) {}

TextInput::~TextInput() = default;

void TextInput::render(NXRenderCtx& ctx) {
    if (!m_visible || !ctx.gpu) return;

    NXRender::UIDrawer::drawInputField(
        ctx,
        toRect(m_x, m_y, m_width, m_height),
        m_text,
        m_placeholder,
        m_focused,
        m_passwordMode,
        m_focused ? m_cursorPos : -1
    );
}

bool TextInput::handleEvent(const NXEvent& e) {
    if (!m_enabled || !m_visible) return false;

    if (e.type == NXRender::EventType::MouseDown) {
        m_focused = pointInRect(e.mouse.x, e.mouse.y, m_x, m_y, m_width, m_height);
        return m_focused;
    }
    if (m_focused && e.type == NXRender::EventType::TextInput) {
        m_text += e.textInput;
        m_cursorPos = static_cast<int>(m_text.size());
        if (m_onTextChange) m_onTextChange(m_text);
        return true;
    }
    if (m_focused && e.type == NXRender::EventType::KeyDown) {
        switch (e.key.key) {
            case NXRender::KeyCode::Backspace:
                if (!m_text.empty()) {
                    m_text.pop_back();
                    m_cursorPos = static_cast<int>(m_text.size());
                    if (m_onTextChange) m_onTextChange(m_text);
                }
                return true;
            case NXRender::KeyCode::Enter:
                if (m_onEnter) m_onEnter();
                return true;
            case NXRender::KeyCode::Left:
                m_cursorPos = std::max(0, m_cursorPos - 1);
                return true;
            case NXRender::KeyCode::Right:
                m_cursorPos = std::min(static_cast<int>(m_text.size()), m_cursorPos + 1);
                return true;
            default: break;
        }
    }
    return false;
}

void TextInput::update() {
    // Blink cursor every 500ms (requires external time source)
    m_cursorBlinkTime++;
    if (m_cursorBlinkTime % 30 == 0) m_showCursor = !m_showCursor;
}

void TextInput::setText(const std::string& t) {
    m_text = t;
    m_cursorPos = static_cast<int>(t.size());
}
void TextInput::setPlaceholder(const std::string& p) { m_placeholder = p; }
void TextInput::setPasswordMode(bool pw) { m_passwordMode = pw; }
void TextInput::setOnTextChange(std::function<void(const std::string&)> cb) { m_onTextChange = std::move(cb); }
void TextInput::setOnEnter(std::function<void()> cb) { m_onEnter = std::move(cb); }

// ============================================================================
// Label — renders via UIDrawer::drawLabel
// ============================================================================

Label::Label(int x, int y, const std::string& text)
    : UIElement(x, y, 0, 0), m_text(text) {}

Label::~Label() = default;

void Label::render(NXRenderCtx& ctx) {
    if (!m_visible || !ctx.gpu) return;
    NXRender::UIDrawer::drawLabel(
        ctx,
        static_cast<float>(m_x),
        static_cast<float>(m_y),
        m_text,
        m_color,
        static_cast<float>(m_fontSize)
    );
}

bool Label::handleEvent(const NXEvent&) { return false; }
void Label::update() {}
void Label::setText(const std::string& t) { m_text = t; }
void Label::setColor(NXColor c) { m_color = c; }
void Label::setFontSize(int s) { m_fontSize = s; }

// ============================================================================
// ModalDialog base
// ============================================================================

ModalDialog::ModalDialog(int x, int y, int w, int h, const std::string& title)
    : m_x(x), m_y(y), m_width(w), m_height(h), m_title(title) {}

void ModalDialog::render(NXRenderCtx& ctx) {
    if (!m_visible || !ctx.gpu) return;

    // Semi-transparent backdrop
    NXRender::UIDrawer::drawBackdrop(ctx, 0.55f);

    // Modal panel
    NXRender::UIDrawer::drawPanel(ctx, toRect(m_x, m_y, m_width, m_height));

    // Title
    NXRender::UIDrawer::drawLabel(
        ctx,
        static_cast<float>(m_x) + 20.0f,
        static_cast<float>(m_y) + 28.0f,
        m_title,
        NXRender::Color(255, 255, 255),
        17.0f
    );

    // Separator below title
    NXRender::UIDrawer::drawSeparator(
        ctx,
        static_cast<float>(m_x) + 20.0f,
        static_cast<float>(m_y) + 48.0f,
        static_cast<float>(m_width) - 40.0f
    );

    // Render child elements
    for (auto& el : m_elements)
        if (el && el->isVisible()) el->render(ctx);
}

bool ModalDialog::handleEvent(const NXEvent& e) {
    if (!m_visible) return false;
    for (auto& el : m_elements)
        if (el && el->isEnabled() && el->isVisible())
            if (el->handleEvent(e)) return true;
    return true;  // Consume all events when visible (modal)
}

void ModalDialog::update() {
    for (auto& el : m_elements) if (el) el->update();
}

void ModalDialog::setVisible(bool v) { m_visible = v; }
void ModalDialog::setOnClose(std::function<void()> cb) { m_onClose = std::move(cb); }
void ModalDialog::addElement(std::unique_ptr<UIElement> el) { m_elements.push_back(std::move(el)); }
void ModalDialog::clearElements() { m_elements.clear(); }

// ============================================================================
// LoginDialog
// ============================================================================

LoginDialog::LoginDialog() : ModalDialog(340, 170, 600, 420, "Sign in to Zepra") {
    // Email label + input
    auto eLabel = std::make_unique<Label>(m_x + 20, m_y + 68, "Email");
    m_emailLabel = eLabel.get();
    addElement(std::move(eLabel));

    auto email = std::make_unique<TextInput>(m_x + 20, m_y + 88, 560, 44, "name@example.com");
    m_emailInput = email.get();
    addElement(std::move(email));

    // Password label + input
    auto pLabel = std::make_unique<Label>(m_x + 20, m_y + 148, "Password");
    m_passwordLabel = pLabel.get();
    addElement(std::move(pLabel));

    auto pass = std::make_unique<TextInput>(m_x + 20, m_y + 168, 560, 44, "••••••••");
    pass->setPasswordMode(true);
    pass->setOnEnter([this] {
        if (m_onLogin && m_emailInput && m_passwordInput)
            m_onLogin(m_emailInput->getText(), m_passwordInput->getText());
    });
    m_passwordInput = pass.get();
    addElement(std::move(pass));

    // Error label (hidden initially)
    auto err = std::make_unique<Label>(m_x + 20, m_y + 228, "");
    err->setColor(NXColor(255, 69, 58));
    m_errorLabel = err.get();
    addElement(std::move(err));

    // Buttons
    auto loginBtn = std::make_unique<Button>(m_x + 20, m_y + 256, 560, 46, "Sign In");
    loginBtn->setOnClick([this] {
        if (m_onLogin && m_emailInput && m_passwordInput)
            m_onLogin(m_emailInput->getText(), m_passwordInput->getText());
    });
    m_loginButton = loginBtn.get();
    addElement(std::move(loginBtn));

    auto cancelBtn = std::make_unique<Button>(m_x + 20, m_y + 312, 560, 40, "Cancel");
    cancelBtn->setPrimary(false);
    cancelBtn->setOnClick([this] { if (m_onCancel) m_onCancel(); });
    m_cancelButton = cancelBtn.get();
    addElement(std::move(cancelBtn));
}

LoginDialog::~LoginDialog() = default;

void LoginDialog::render(NXRenderCtx& ctx) {
    ModalDialog::render(ctx);
    // Draw error if any
    if (!m_errorMessage.empty() && ctx.gpu)
        NXRender::UIDrawer::drawErrorLabel(ctx,
            static_cast<float>(m_x + 20),
            static_cast<float>(m_y + 228),
            m_errorMessage);
}

bool LoginDialog::handleEvent(const NXEvent& e) { return ModalDialog::handleEvent(e); }
void LoginDialog::update() { ModalDialog::update(); }
void LoginDialog::setOnLogin(std::function<void(const std::string&, const std::string&)> cb) { m_onLogin = std::move(cb); }
void LoginDialog::setOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }
void LoginDialog::setError(const std::string& err) { m_errorMessage = err; }
void LoginDialog::clearError() { m_errorMessage.clear(); }
void LoginDialog::setEmail(const std::string& e) { if (m_emailInput) m_emailInput->setText(e); }
void LoginDialog::setPassword(const std::string& p) { if (m_passwordInput) m_passwordInput->setText(p); }

// ============================================================================
// TwoFactorDialog
// ============================================================================

TwoFactorDialog::TwoFactorDialog()
    : ModalDialog(390, 220, 500, 300, "Two-Factor Authentication") {
    auto instr = std::make_unique<Label>(m_x + 20, m_y + 68, "Enter the 6-digit code from your authenticator app.");
    instr->setColor(NXColor(174, 174, 178));
    m_instructionLabel = instr.get();
    addElement(std::move(instr));

    auto code = std::make_unique<TextInput>(m_x + 20, m_y + 100, 460, 44, "000000");
    code->setOnEnter([this] {
        if (m_onVerify && m_codeInput) m_onVerify(m_codeInput->getText());
    });
    m_codeInput = code.get();
    addElement(std::move(code));

    auto verifyBtn = std::make_unique<Button>(m_x + 20, m_y + 164, 460, 46, "Verify");
    verifyBtn->setOnClick([this] {
        if (m_onVerify && m_codeInput) m_onVerify(m_codeInput->getText());
    });
    m_verifyButton = verifyBtn.get();
    addElement(std::move(verifyBtn));

    auto cancelBtn = std::make_unique<Button>(m_x + 20, m_y + 220, 460, 38, "Cancel");
    cancelBtn->setPrimary(false);
    cancelBtn->setOnClick([this] { if (m_onCancel) m_onCancel(); });
    m_cancelButton = cancelBtn.get();
    addElement(std::move(cancelBtn));
}

TwoFactorDialog::~TwoFactorDialog() = default;

void TwoFactorDialog::render(NXRenderCtx& ctx) {
    ModalDialog::render(ctx);
    if (!m_errorMessage.empty() && ctx.gpu)
        NXRender::UIDrawer::drawErrorLabel(ctx,
            static_cast<float>(m_x + 20),
            static_cast<float>(m_y + 152),
            m_errorMessage);
}
bool TwoFactorDialog::handleEvent(const NXEvent& e) { return ModalDialog::handleEvent(e); }
void TwoFactorDialog::update() { ModalDialog::update(); }
void TwoFactorDialog::setOnVerify(std::function<void(const std::string&)> cb) { m_onVerify = std::move(cb); }
void TwoFactorDialog::setOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }
void TwoFactorDialog::setError(const std::string& err) { m_errorMessage = err; }
void TwoFactorDialog::clearError() { m_errorMessage.clear(); }
void TwoFactorDialog::setTempToken(const std::string& t) { m_tempToken = t; }

// ============================================================================
// PasswordPromptDialog
// ============================================================================

PasswordPromptDialog::PasswordPromptDialog(const std::string& url, const std::string& domain)
    : ModalDialog(290, 180, 700, 380, "Sign in to " + domain)
    , m_websiteUrl(url), m_domain(domain) {
    auto uLabel = std::make_unique<Label>(m_x + 20, m_y + 68, "Username");
    m_usernameLabel = uLabel.get();
    addElement(std::move(uLabel));

    auto uInput = std::make_unique<TextInput>(m_x + 20, m_y + 88, 660, 44, "Username");
    m_usernameInput = uInput.get();
    addElement(std::move(uInput));

    auto pLabel = std::make_unique<Label>(m_x + 20, m_y + 148, "Password");
    m_passwordLabel = pLabel.get();
    addElement(std::move(pLabel));

    auto pInput = std::make_unique<TextInput>(m_x + 20, m_y + 168, 660, 44, "Password");
    pInput->setPasswordMode(true);
    m_passwordInput = pInput.get();
    addElement(std::move(pInput));

    auto submitBtn = std::make_unique<Button>(m_x + 20, m_y + 236, 320, 46, "Sign In");
    submitBtn->setOnClick([this] {
        if (m_onSubmit && m_usernameInput && m_passwordInput)
            m_onSubmit(m_usernameInput->getText(), m_passwordInput->getText());
    });
    m_submitButton = submitBtn.get();
    addElement(std::move(submitBtn));

    auto cancelBtn = std::make_unique<Button>(m_x + 360, m_y + 236, 320, 46, "Cancel");
    cancelBtn->setPrimary(false);
    cancelBtn->setOnClick([this] { if (m_onCancel) m_onCancel(); });
    m_cancelButton = cancelBtn.get();
    addElement(std::move(cancelBtn));
}

PasswordPromptDialog::~PasswordPromptDialog() = default;

void PasswordPromptDialog::render(NXRenderCtx& ctx) {
    ModalDialog::render(ctx);
    if (!m_websiteUrl.empty() && ctx.gpu)
        NXRender::UIDrawer::drawLabel(ctx,
            static_cast<float>(m_x + 20),
            static_cast<float>(m_y + 58),
            m_websiteUrl,
            NXRender::Color(99, 99, 102),
            12.0f);
    if (!m_errorMessage.empty() && ctx.gpu)
        NXRender::UIDrawer::drawErrorLabel(ctx,
            static_cast<float>(m_x + 20),
            static_cast<float>(m_y + 224),
            m_errorMessage);
}
bool PasswordPromptDialog::handleEvent(const NXEvent& e) { return ModalDialog::handleEvent(e); }
void PasswordPromptDialog::update() { ModalDialog::update(); }
void PasswordPromptDialog::setOnSubmit(std::function<void(const std::string&, const std::string&)> cb) { m_onSubmit = std::move(cb); }
void PasswordPromptDialog::setOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }
void PasswordPromptDialog::setError(const std::string& err) { m_errorMessage = err; }
void PasswordPromptDialog::clearError() { m_errorMessage.clear(); }

// ============================================================================
// AuthUIManager singleton
// ============================================================================

AuthUIManager& AuthUIManager::getInstance() {
    static AuthUIManager instance;
    return instance;
}

AuthUIManager::AuthUIManager() = default;
AuthUIManager::~AuthUIManager() { shutdown(); }

bool AuthUIManager::initialize(NXRenderCtx& ctx) {
    m_ctx = ctx;
    return ctx.gpu != nullptr;
}

void AuthUIManager::shutdown() {
    m_loginDialog.reset();
    m_twoFactorDialog.reset();
    m_passwordPromptDialog.reset();
}

void AuthUIManager::update() {
    if (m_loginDialog          && m_loginDialog->isVisible())          m_loginDialog->update();
    if (m_twoFactorDialog      && m_twoFactorDialog->isVisible())      m_twoFactorDialog->update();
    if (m_passwordPromptDialog && m_passwordPromptDialog->isVisible()) m_passwordPromptDialog->update();
}

void AuthUIManager::render(NXRenderCtx& ctx) {
    if (m_loginDialog          && m_loginDialog->isVisible())          m_loginDialog->render(ctx);
    if (m_twoFactorDialog      && m_twoFactorDialog->isVisible())      m_twoFactorDialog->render(ctx);
    if (m_passwordPromptDialog && m_passwordPromptDialog->isVisible()) m_passwordPromptDialog->render(ctx);
}

bool AuthUIManager::handleEvent(const NXEvent& e) {
    // Top-most dialog gets events first
    if (m_passwordPromptDialog && m_passwordPromptDialog->isVisible())
        return m_passwordPromptDialog->handleEvent(e);
    if (m_twoFactorDialog && m_twoFactorDialog->isVisible())
        return m_twoFactorDialog->handleEvent(e);
    if (m_loginDialog && m_loginDialog->isVisible())
        return m_loginDialog->handleEvent(e);
    return false;
}

void AuthUIManager::showLoginDialog() {
    if (!m_loginDialog) {
        m_loginDialog = std::make_unique<LoginDialog>();
        if (m_onLogin) m_loginDialog->setOnLogin(m_onLogin);
        m_loginDialog->setOnCancel([this] { hideLoginDialog(); });
    }
    m_loginDialog->setVisible(true);
}
void AuthUIManager::hideLoginDialog() { if (m_loginDialog) m_loginDialog->setVisible(false); }

void AuthUIManager::showTwoFactorDialog(const std::string& tempToken) {
    if (!m_twoFactorDialog) {
        m_twoFactorDialog = std::make_unique<TwoFactorDialog>();
        if (m_onTwoFactor) m_twoFactorDialog->setOnVerify(m_onTwoFactor);
        m_twoFactorDialog->setOnCancel([this] { hideTwoFactorDialog(); });
    }
    m_twoFactorDialog->setTempToken(tempToken);
    m_twoFactorDialog->setVisible(true);
}
void AuthUIManager::hideTwoFactorDialog() { if (m_twoFactorDialog) m_twoFactorDialog->setVisible(false); }

void AuthUIManager::showPasswordPromptDialog(const std::string& url, const std::string& domain) {
    m_passwordPromptDialog = std::make_unique<PasswordPromptDialog>(url, domain);
    if (m_onPasswordPrompt) m_passwordPromptDialog->setOnSubmit(m_onPasswordPrompt);
    m_passwordPromptDialog->setOnCancel([this] { hidePasswordPromptDialog(); });
    m_passwordPromptDialog->setVisible(true);
}
void AuthUIManager::hidePasswordPromptDialog() {
    if (m_passwordPromptDialog) m_passwordPromptDialog->setVisible(false);
}

void AuthUIManager::setLoginError(const std::string& e) { if (m_loginDialog) m_loginDialog->setError(e); }
void AuthUIManager::setTwoFactorError(const std::string& e) { if (m_twoFactorDialog) m_twoFactorDialog->setError(e); }
void AuthUIManager::setPasswordPromptError(const std::string& e) { if (m_passwordPromptDialog) m_passwordPromptDialog->setError(e); }

void AuthUIManager::setOnLogin(std::function<void(const std::string&, const std::string&)> cb) { m_onLogin = std::move(cb); }
void AuthUIManager::setOnTwoFactor(std::function<void(const std::string&)> cb) { m_onTwoFactor = std::move(cb); }
void AuthUIManager::setOnPasswordPrompt(std::function<void(const std::string&, const std::string&)> cb) { m_onPasswordPrompt = std::move(cb); }

// ============================================================================
// AuthUIUtils — delegate to UIDrawer
// ============================================================================

namespace AuthUIUtils {

NXColor createColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return NXColor(r, g, b, a);
}

void renderText(NXRenderCtx& ctx, const std::string& text,
                int x, int y, NXColor color, int fontSize) {
    NXRender::UIDrawer::drawLabel(ctx,
        static_cast<float>(x), static_cast<float>(y),
        text, color, static_cast<float>(fontSize));
}

void renderRect(NXRenderCtx& ctx, int x, int y, int w, int h,
                NXColor color, bool filled) {
    if (!ctx.gpu) return;
    NXRender::Rect rect(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(w), static_cast<float>(h));
    if (filled) ctx.gpu->fillRect(rect, color);
    else        ctx.gpu->strokeRect(rect, color);
}

void renderBorder(NXRenderCtx& ctx, int x, int y, int w, int h,
                  NXColor color, int thickness) {
    if (!ctx.gpu) return;
    ctx.gpu->strokeRect(
        NXRender::Rect(static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(w), static_cast<float>(h)),
        color,
        static_cast<float>(thickness)
    );
}

bool isPointInRect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

std::string maskPassword(const std::string& password) {
    return NXRender::UIDrawer::maskText(password);
}

} // namespace AuthUIUtils

} // namespace ZepraUI
