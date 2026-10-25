// The desktop window: plain Win32 controls whose text is replaced in place on
// a timer, so figures update where they stand instead of scrolling past.
//
// Deliberately minimal for now - it is the place graphs and session analysis
// will go, so it owns the app-level wiring (hotkeys, layout reload, the
// overlay's lifetime) rather than main().
#pragma once

#include "fields.h"
#include "layout.h"
#include "overlay.h"
#include "pipe_client.h"
#include "stats.h"

#include <windows.h>
#include <vector>

namespace grimnumbers {

class AppWindow {
public:
    AppWindow(Stats& stats, PipeClient& pipe, Overlay& overlay,
              Layout& layout, ElementTable& elements);

    AppWindow(const AppWindow&)            = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool Create(HINSTANCE instance, int show);
    HWND Handle() const { return hwnd_; }

    // Paths are remembered so F10 can re-read them without re-deriving the
    // executable's directory.
    void SetConfigPaths(const std::wstring& layoutPath,
                        const std::wstring& elementsPath);

    int RunMessageLoop();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void Refresh();
    void ReloadConfig();
    void ToggleOverlay();

    struct Field {
        const wchar_t* label;
        const char*    format;   // {token} text, expanded every refresh
        HWND           value;
    };

    Stats&        stats_;
    PipeClient&   pipe_;
    Overlay&      overlay_;
    Layout&       layout_;
    ElementTable& elements_;

    HWND  hwnd_       = nullptr;
    HWND  elementBox_ = nullptr;
    HWND  targetBox_  = nullptr;
    HWND  statusBar_  = nullptr;
    HWND  overlayBtn_ = nullptr;
    HFONT font_       = nullptr;

    std::vector<Field> fields_;
    std::wstring layoutPath_;
    std::wstring elementsPath_;
};

} // namespace grimnumbers
