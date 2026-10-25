// The in-game display: a click-through, always-on-top layered window drawn
// with Direct2D and DirectWrite (both ship with Windows - no third-party
// renderer, and nothing is hooked inside the game process).
//
// Click-through comes from WS_EX_TRANSPARENT, which makes the window
// invisible to hit testing, so every click, drag and hover goes straight to
// Grim Dawn. WS_EX_NOACTIVATE keeps it from ever taking focus, so the game
// never sees a deactivation.
//
// What it draws is entirely described by a Layout, which is read from a text
// file, so panels can be moved, retitled and re-populated without a rebuild.
//
// Caveat worth knowing: a layered window cannot composite over an
// exclusive-fullscreen swapchain. Run Grim Dawn windowed or borderless.
#pragma once

#include "fields.h"
#include "layout.h"
#include "stats.h"

#include <windows.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct ID2D1SolidColorBrush;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace grimnumbers {

class Overlay {
public:
    Overlay(const Layout& layout, const ElementTable& elements);
    ~Overlay();

    Overlay(const Overlay&)            = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();

    bool Visible() const { return visible_; }
    void SetVisible(bool visible);
    void Toggle() { SetVisible(!visible_); }

    // Redraws from this snapshot. A no-op while hidden.
    void Render(const Snapshot& snapshot);

    // The game's pid, from the protocol HELLO. When it is known the overlay
    // positions itself inside the game's own window instead of the primary
    // monitor, so anchors mean the game's corners rather than the screen's.
    void SetGamePid(uint32_t pid) { gamePid_ = pid; }

    // Re-creates the text formats after the layout's font or size changed.
    // Call after reloading the layout file.
    void OnLayoutChanged();

private:
    struct DrawRow {
        bool         separator = false;
        std::wstring left;
        std::wstring right;
        uint32_t     leftRgb  = 0;
        uint32_t     rightRgb = 0;
    };

    struct DrawPanel {
        RECT                 rc = {};
        std::wstring         title;
        std::vector<DrawRow> rows;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureDevice();
    bool EnsureFormats();
    bool EnsureSurface(int width, int height);
    void ReleaseFormats();

    RECT TrackRect() const;
    void BuildPanels(const Snapshot& s, const RECT& tracked,
                     std::vector<DrawPanel>& out, RECT& bounds) const;
    void RenderPanel(const DrawPanel& p, const POINT& origin) const;
    void SetBrush(uint32_t rgb) const;

    int LineHeight() const;

    const Layout&       layout_;
    const ElementTable& elements_;

    HWND      hwnd_    = nullptr;
    HDC       memDc_   = nullptr;
    HBITMAP   dib_     = nullptr;
    HBITMAP   oldDib_  = nullptr;
    int       surfaceW_ = 0;
    int       surfaceH_ = 0;
    bool      visible_  = false;
    uint32_t  gamePid_  = 0;

    ID2D1Factory*         d2d_        = nullptr;
    ID2D1DCRenderTarget*  rt_         = nullptr;
    ID2D1SolidColorBrush* brush_      = nullptr;
    IDWriteFactory*       dwrite_     = nullptr;
    IDWriteTextFormat*    fmtLeft_    = nullptr;
    IDWriteTextFormat*    fmtRight_   = nullptr;
    IDWriteTextFormat*    fmtTitle_   = nullptr;
};

} // namespace grimnumbers
