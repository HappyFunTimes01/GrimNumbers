#include "overlay.h"
#include "strutil.h"

// winuser.h defines DrawText as a macro resolving to DrawTextA/DrawTextW. It
// has to go before d2d1.h, or the macro renames ID2D1RenderTarget::DrawText
// at its declaration and the method becomes unreachable. Nothing here wants
// the GDI function.
#undef DrawText

#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>

namespace grimnumbers {
namespace {

const wchar_t kClassName[] = L"GrimNumbersOverlay";

// Direct2D wants colours as floats; the config files carry 0xRRGGBB.
D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f)
{
    return D2D1::ColorF((float)((rgb >> 16) & 0xFF) / 255.0f,
                        (float)((rgb >>  8) & 0xFF) / 255.0f,
                        (float)( rgb        & 0xFF) / 255.0f,
                        alpha);
}

template <typename T>
void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Picks the largest visible top-level window belonging to `pid`. Grim Dawn
// also owns small helper windows, and borderless mode has no caption to test
// for, so size is the one reliable discriminator.
struct FindWindowCtx {
    DWORD pid;
    HWND  best;
    LONG  bestArea;
};

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param)
{
    FindWindowCtx* ctx = (FindWindowCtx*)param;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;

    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) return TRUE;

    const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->best = hwnd; }
    return TRUE;
}

} // namespace

Overlay::Overlay(const Layout& layout, const ElementTable& elements)
    : layout_(layout), elements_(elements) {}

Overlay::~Overlay() { Destroy(); }

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // The window is click-through and never activated, so there is no input
    // to handle here at all.
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Overlay::Create(HINSTANCE instance)
{
    if (hwnd_) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kClassName;
    // Registering twice is expected when the app restarts the overlay.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED |      // per-pixel alpha via UpdateLayeredWindow
        WS_EX_TRANSPARENT |  // click-through: hit testing skips this window
        WS_EX_TOPMOST |
        WS_EX_NOACTIVATE |   // never steals focus from the game
        WS_EX_TOOLWINDOW,    // keeps it out of the taskbar and Alt-Tab
        kClassName, L"Grim Numbers Overlay",
        WS_POPUP,
        0, 0, 16, 16,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;

    if (!EnsureDevice()) { Destroy(); return false; }
    return true;
}

void Overlay::Destroy()
{
    ReleaseFormats();
    Release(brush_);
    Release(rt_);
    Release(dwrite_);
    Release(d2d_);

    if (memDc_) {
        if (oldDib_) SelectObject(memDc_, oldDib_);
        DeleteDC(memDc_);
        memDc_ = nullptr;
        oldDib_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    surfaceW_ = surfaceH_ = 0;

    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    visible_ = false;
}

void Overlay::ReleaseFormats()
{
    Release(fmtLeft_);
    Release(fmtRight_);
    Release(fmtTitle_);
}

void Overlay::OnLayoutChanged() { ReleaseFormats(); }

bool Overlay::EnsureDevice()
{
    if (!d2d_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_)))
            return false;
    }
    if (!dwrite_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory),
                                       (IUnknown**)&dwrite_)))
            return false;
    }
    if (!rt_) {
        // A DC render target is the one Direct2D target that can draw into a
        // GDI DC, which is what UpdateLayeredWindow needs. Premultiplied
        // alpha is mandatory for ULW_ALPHA.
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);

        if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt_))) return false;

        // ClearType reads the background it blends against, which a
        // transparent surface does not have; it would produce colour fringes
        // and wrong alpha. Greyscale is the correct mode for a layered window.
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        if (FAILED(rt_->CreateSolidColorBrush(Rgb(0xFFFFFF), &brush_)))
            return false;
    }
    return EnsureFormats();
}

bool Overlay::EnsureFormats()
{
    if (fmtLeft_ && fmtRight_ && fmtTitle_) return true;
    ReleaseFormats();

    const wchar_t* font = layout_.font.c_str();
    const float    size = layout_.fontSize;

    auto make = [&](DWRITE_FONT_WEIGHT weight, float px, IDWriteTextFormat** out) {
        return dwrite_->CreateTextFormat(font, nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, px,
                                         L"en-us", out);
    };

    if (FAILED(make(DWRITE_FONT_WEIGHT_NORMAL,    size, &fmtLeft_)))  return false;
    if (FAILED(make(DWRITE_FONT_WEIGHT_NORMAL,    size, &fmtRight_))) return false;
    if (FAILED(make(DWRITE_FONT_WEIGHT_SEMI_BOLD, size, &fmtTitle_))) return false;

    // Values sit in the right-hand column so the numbers line up regardless of
    // how long the label is.
    fmtRight_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    for (IDWriteTextFormat* f : {fmtLeft_, fmtRight_, fmtTitle_}) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

bool Overlay::EnsureSurface(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (dib_ && width == surfaceW_ && height == surfaceH_) return true;

    if (!memDc_) {
        HDC screen = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!memDc_) return false;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;   // top-down, as Direct2D expects
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return false;

    HGDIOBJ prev = SelectObject(memDc_, dib);
    if (oldDib_ == nullptr) oldDib_ = (HBITMAP)prev;   // the DC's original 1x1
    if (dib_) DeleteObject(dib_);

    dib_      = dib;
    surfaceW_ = width;
    surfaceH_ = height;
    return true;
}

void Overlay::SetVisible(bool visible)
{
    if (!hwnd_ || visible == visible_) return;
    visible_ = visible;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

int Overlay::LineHeight() const
{
    return (int)ceilf(layout_.fontSize * 1.55f);
}

// The rectangle anchors are measured from: the game's client area when its
// window can be found, otherwise the primary monitor. Using the game's own
// rectangle means "lower right" stays the lower right of the game in windowed
// mode, not of the desktop.
RECT Overlay::TrackRect() const
{
    if (gamePid_) {
        FindWindowCtx ctx = {gamePid_, nullptr, 0};
        EnumWindows(&FindGameWindow, (LPARAM)&ctx);
        if (ctx.best) {
            RECT rc = {};
            GetClientRect(ctx.best, &rc);
            POINT tl = {rc.left, rc.top};
            POINT br = {rc.right, rc.bottom};
            if (ClientToScreen(ctx.best, &tl) && ClientToScreen(ctx.best, &br))
                return RECT{tl.x, tl.y, br.x, br.y};
        }
    }

    RECT rc = {};
    rc.right  = GetSystemMetrics(SM_CXSCREEN);
    rc.bottom = GetSystemMetrics(SM_CYSCREEN);
    return rc;
}

void Overlay::BuildPanels(const Snapshot& s, const RECT& tracked,
                          std::vector<DrawPanel>& out, RECT& bounds) const
{
    const int lineH = LineHeight();
    const int sepH  = std::max(4, lineH / 2);

    // One stacking cursor per corner, so panels sharing an anchor queue up
    // away from it in the order the config lists them.
    int cursor[4] = {layout_.margin, layout_.margin, layout_.margin, layout_.margin};

    bool first = true;
    bounds = RECT{0, 0, 0, 0};

    for (const LayoutPanel& p : layout_.panels) {
        if (!p.visible || p.rows.empty()) continue;

        DrawPanel dp;
        dp.title = Widen(ExpandTokens(p.title, s));

        for (const LayoutRow& r : p.rows) {
            switch (r.kind) {
            case LayoutRow::Kind::Separator: {
                DrawRow row;
                row.separator = true;
                dp.rows.push_back(row);
                break;
            }
            case LayoutRow::Kind::Text: {
                DrawRow row;
                row.left     = Widen(ExpandTokens(r.label, s));
                row.right    = Widen(ExpandTokens(r.value, s));
                row.leftRgb  = layout_.labelText;
                row.rightRgb = layout_.valueText;
                dp.rows.push_back(row);
                break;
            }
            case LayoutRow::Kind::Elements: {
                if (s.elements.empty()) {
                    DrawRow row;
                    row.left    = L"no damage yet";
                    row.leftRgb = layout_.labelText;
                    dp.rows.push_back(row);
                    break;
                }
                const int n = std::min((int)s.elements.size(), r.count);
                for (int i = 0; i < n; ++i) {
                    const ElemRow& e = s.elements[(size_t)i];
                    const double share = s.totalDamage > 0.0
                                       ? 100.0 * e.total / s.totalDamage : 0.0;

                    char buf[96];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s/s  %.0f%%",
                                FormatShort(e.window / s.windowSec).c_str(), share);

                    DrawRow row;
                    row.left     = Widen(elements_.Label(e.type));
                    row.right    = Widen(buf);
                    // An unmapped damage type keeps the neutral colour, which
                    // is the visual cue that its id still needs naming.
                    row.leftRgb  = elements_.Color(e.type, layout_.elemFallback);
                    row.rightRgb = layout_.valueText;
                    dp.rows.push_back(row);
                }
                break;
            }
            case LayoutRow::Kind::Targets: {
                const int n = std::min((int)s.targets.size(), r.count);
                for (int i = 0; i < n; ++i) {
                    const TargetRow& t = s.targets[(size_t)i];

                    char buf[96];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s  x%llu",
                                FormatShort(t.total).c_str(),
                                (unsigned long long)t.hits);

                    DrawRow row;
                    row.left     = Widen(t.name.empty() ? "?" : t.name);
                    row.right    = Widen(buf);
                    row.leftRgb  = layout_.labelText;
                    row.rightRgb = layout_.valueText;
                    dp.rows.push_back(row);
                }
                break;
            }
            }
        }
        if (dp.rows.empty()) continue;

        int height = layout_.padding * 2;
        if (!dp.title.empty()) height += lineH;
        for (const DrawRow& r : dp.rows) height += r.separator ? sepH : lineH;

        const int width = p.width > 0 ? p.width : layout_.width;
        const Anchor a  = p.anchorSet ? p.anchor : layout_.anchor;
        const int idx   = (int)a;

        const bool rightSide  = (a == Anchor::TopRight || a == Anchor::BottomRight);
        const bool bottomSide = (a == Anchor::BottomLeft || a == Anchor::BottomRight);

        const int x = rightSide ? tracked.right - layout_.margin - width + p.dx
                                : tracked.left  + layout_.margin + p.dx;
        const int y = bottomSide ? tracked.bottom - cursor[idx] - height + p.dy
                                 : tracked.top    + cursor[idx] + p.dy;

        cursor[idx] += height + layout_.gap;

        dp.rc = RECT{x, y, x + width, y + height};
        if (first) {
            bounds = dp.rc;
            first  = false;
        } else {
            bounds.left   = std::min(bounds.left,   dp.rc.left);
            bounds.top    = std::min(bounds.top,    dp.rc.top);
            bounds.right  = std::max(bounds.right,  dp.rc.right);
            bounds.bottom = std::max(bounds.bottom, dp.rc.bottom);
        }
        out.push_back(std::move(dp));
    }
}

void Overlay::SetBrush(uint32_t rgb) const
{
    brush_->SetColor(Rgb(rgb));
}

void Overlay::RenderPanel(const DrawPanel& p, const POINT& origin) const
{
    const int lineH = LineHeight();
    const int sepH  = std::max(4, lineH / 2);

    const float left   = (float)(p.rc.left   - origin.x);
    const float top    = (float)(p.rc.top    - origin.y);
    const float right  = (float)(p.rc.right  - origin.x);
    const float bottom = (float)(p.rc.bottom - origin.y);

    const D2D1_ROUNDED_RECT box =
        D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), 6.0f, 6.0f);

    // The panel's own translucency, so the game stays readable underneath.
    brush_->SetColor(Rgb(layout_.bg, 0.82f));
    rt_->FillRoundedRectangle(box, brush_);
    SetBrush(layout_.border);
    rt_->DrawRoundedRectangle(box, brush_, 1.0f);

    const float textLeft  = left  + layout_.padding;
    const float textRight = right - layout_.padding;
    float y = top + layout_.padding;

    if (!p.title.empty()) {
        SetBrush(layout_.titleText);
        rt_->DrawText(p.title.c_str(), (UINT32)p.title.size(), fmtTitle_,
                       D2D1::RectF(textLeft, y, textRight, y + lineH), brush_);
        y += lineH;
    }

    for (const DrawRow& r : p.rows) {
        if (r.separator) {
            const float mid = y + sepH * 0.5f;
            SetBrush(layout_.border);
            rt_->DrawLine(D2D1::Point2F(textLeft, mid),
                          D2D1::Point2F(textRight, mid), brush_, 1.0f);
            y += sepH;
            continue;
        }

        const D2D1_RECT_F line = D2D1::RectF(textLeft, y, textRight, y + lineH);
        if (!r.left.empty()) {
            SetBrush(r.leftRgb);
            rt_->DrawText(r.left.c_str(), (UINT32)r.left.size(), fmtLeft_,
                           line, brush_);
        }
        if (!r.right.empty()) {
            SetBrush(r.rightRgb);
            rt_->DrawText(r.right.c_str(), (UINT32)r.right.size(), fmtRight_,
                           line, brush_);
        }
        y += lineH;
    }
}

void Overlay::Render(const Snapshot& s)
{
    if (!visible_ || !hwnd_ || !EnsureDevice()) return;

    std::vector<DrawPanel> panels;
    RECT bounds = {};
    BuildPanels(s, TrackRect(), panels, bounds);
    if (panels.empty()) return;

    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    if (!EnsureSurface(w, h)) return;

    const RECT bind = {0, 0, w, h};
    if (FAILED(rt_->BindDC(memDc_, &bind))) return;

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0.0f));   // fully transparent, not black

    const POINT origin = {bounds.left, bounds.top};
    for (const DrawPanel& p : panels) RenderPanel(p, origin);

    if (FAILED(rt_->EndDraw())) {
        // The target is unrecoverable after a device loss; drop it and let the
        // next frame rebuild everything.
        Release(brush_);
        Release(rt_);
        ReleaseFormats();
        return;
    }

    // ULW moves, resizes and blits in one call, so the window never shows a
    // half-drawn frame and needs no WM_PAINT.
    POINT dst = {bounds.left, bounds.top};
    POINT src = {0, 0};
    SIZE  size = {w, h};

    BLENDFUNCTION blend = {};
    blend.BlendOp             = AC_SRC_OVER;
    blend.SourceConstantAlpha = (BYTE)(layout_.opacity * 255.0f);
    blend.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDc_, &src, 0,
                        &blend, ULW_ALPHA);
}

} // namespace grimnumbers
