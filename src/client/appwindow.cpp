#include "appwindow.h"
#include "strutil.h"

#include <stdio.h>

namespace grimnumbers {
namespace {

const wchar_t kClassName[] = L"GrimNumbersApp";

enum : int {
    kIdOverlayBtn = 1001,
    kIdReloadBtn  = 1002,
    kIdResetBtn   = 1003,
};

enum : int {
    kHotkeyOverlay = 1,   // F9
    kHotkeyReload  = 2,   // F10
};

enum : UINT {
    kTimerRefresh = 1,
};

// Geometry. Fixed rather than computed: the window is not resizable yet, and
// a layout engine here would be more code than the window it serves.
enum : int {
    kPad      = 12,
    kLabelW   = 118,
    kValueW   = 230,
    kLineH    = 21,
    kClientW  = kPad * 2 + kLabelW + 8 + kValueW,
    kListH    = 132,
};

// Label plus the text that produces its value. Tokens are resolved by
// FieldValue, so adding a row here needs no other change.
const struct { const wchar_t* label; const char* format; } kFields[] = {
    {L"Link",          "{link}"},
    {L"Character",     "{player.name}"},
    {L"DPS (rolling)", "{dps.rolling} /s   ({window}s average)"},
    {L"DPS (session)", "{dps.session} /s   over {dps.span}s"},
    {L"Total damage",  "{dmg.total}"},
    {L"Hits",          "{hits.total}"},
    {L"Crit rate",     "{crit.rate}   ({crit.hits} crits)"},
    {L"Crit damage",   "{crit.damage}"},
    {L"Biggest hit",   "{hit.biggest}"},
    {L"DTPS (rolling)","{dtps.rolling} /s   ({window}s average)"},
    {L"Total taken",   "{taken.total}   ({taken.hits} hits, {taken.ticks} ticks)"},
    {L"Biggest taken", "{taken.biggest}"},
    {L"Dropped",       "{drops.total}   (5s {drops.5s} / 60s {drops.60s})"},
};

HWND MakeStatic(HWND parent, const wchar_t* text, int x, int y, int w, DWORD extra)
{
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | extra,
                           x, y, w, kLineH - 3, parent, nullptr, nullptr, nullptr);
}

// Read-only multiline edits are the simplest control that shows a block of
// text and never scrolls on its own: the whole block is replaced each refresh.
HWND MakeBox(HWND parent, int x, int y, int w, int h)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                           x, y, w, h, parent, nullptr, nullptr, nullptr);
}

} // namespace

AppWindow::AppWindow(Stats& stats, PipeClient& pipe, Overlay& overlay,
                     Layout& layout, ElementTable& elements)
    : stats_(stats), pipe_(pipe), overlay_(overlay),
      layout_(layout), elements_(elements) {}

void AppWindow::SetConfigPaths(const std::wstring& layoutPath,
                               const std::wstring& elementsPath)
{
    layoutPath_   = layoutPath;
    elementsPath_ = elementsPath;
}

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }

    auto* self = (AppWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    self->hwnd_ = hwnd;
    return self->OnMessage(msg, wp, lp);
}

bool AppWindow::Create(HINSTANCE instance, int show)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    const int rows   = (int)(sizeof(kFields) / sizeof(kFields[0]));
    const int height = kPad + rows * kLineH + kPad + 20 + kListH + 8 +
                       20 + kListH + kPad + 34 + 24;

    RECT rc = {0, 0, kClientW, height};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);

    hwnd_ = CreateWindowExW(0, kClassName, L"Grim Numbers",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    return true;
}

void AppWindow::CreateControls()
{
    // The shell's own UI font, so the window does not look like 1995.
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);

    int y = kPad;
    for (const auto& f : kFields) {
        MakeStatic(hwnd_, f.label, kPad, y, kLabelW, 0);
        HWND value = MakeStatic(hwnd_, L"-", kPad + kLabelW + 8, y, kValueW, 0);
        fields_.push_back(Field{f.label, f.format, value});
        y += kLineH;
    }

    y += kPad;
    MakeStatic(hwnd_, L"Elements (rolling DPS, and session share of total damage)", kPad, y, kClientW - kPad * 2, 0);
    y += 20;
    elementBox_ = MakeBox(hwnd_, kPad, y, kClientW - kPad * 2, kListH);
    y += kListH + 8;

    MakeStatic(hwnd_, L"Top targets", kPad, y, kClientW - kPad * 2, 0);
    y += 20;
    targetBox_ = MakeBox(hwnd_, kPad, y, kClientW - kPad * 2, kListH);
    y += kListH + kPad;

    overlayBtn_ = CreateWindowExW(0, L"BUTTON",
                                  overlay_.Visible() ? L"Overlay: on (F9)"
                                                     : L"Overlay (F9)",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kPad, y, 130, 26, hwnd_,
                                  (HMENU)kIdOverlayBtn, nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Reload layout (F10)",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    kPad + 138, y, 150, 26, hwnd_,
                    (HMENU)kIdReloadBtn, nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Reset",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    kPad + 296, y, 70, 26, hwnd_,
                    (HMENU)kIdResetBtn, nullptr, nullptr);
    y += 34;

    statusBar_ = MakeStatic(hwnd_, L"", kPad, y, kClientW - kPad * 2, 0);

    if (font_) {
        // EnumChildWindows rather than a list: every control wants the same
        // font, including the ones added later.
        EnumChildWindows(hwnd_, [](HWND child, LPARAM param) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)param, TRUE);
            return TRUE;
        }, (LPARAM)font_);
    }
}

void AppWindow::ToggleOverlay()
{
    overlay_.Toggle();
    SetWindowTextW(overlayBtn_, overlay_.Visible() ? L"Overlay: on (F9)"
                                                   : L"Overlay (F9)");
}

void AppWindow::ReloadConfig()
{
    const bool gotLayout   = layout_.Load(layoutPath_.c_str());
    const bool gotElements = elements_.Load(elementsPath_.c_str());
    overlay_.OnLayoutChanged();

    wchar_t buf[256];
    _snwprintf_s(buf, _TRUNCATE, L"layout: %s   elements: %s (%zu named)",
                 gotLayout ? L"loaded" : L"built-in",
                 gotElements ? L"loaded" : L"none",
                 elements_.Count());
    SetWindowTextW(statusBar_, buf);
}

void AppWindow::Refresh()
{
    const Snapshot s = stats_.Take();
    overlay_.SetGamePid(s.gamePid);

    for (const Field& f : fields_) {
        const std::wstring text = Widen(ExpandTokens(f.format, s));
        // Only write when it changed: SetWindowText repaints unconditionally,
        // and a window of static controls repainting four times a second
        // flickers for no reason.
        wchar_t current[256] = {};
        GetWindowTextW(f.value, current, 256);
        if (text != current) SetWindowTextW(f.value, text.c_str());
    }

    std::string elems;
    if (s.elements.empty()) {
        elems = "no damage yet\r\n";
    } else {
        for (const ElemRow& e : s.elements) {
            const double share = s.totalDamage > 0.0
                               ? 100.0 * e.total / s.totalDamage : 0.0;
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%-16s %12s/s  %5.1f%%  %14s total\r\n",
                        elements_.Label(e.type).c_str(),
                        FormatShort(e.window / s.windowSec).c_str(),
                        share, FormatCount(e.total).c_str());
            elems += line;
        }
    }
    SetWindowTextW(elementBox_, Widen(elems).c_str());

    std::string targets;
    for (size_t i = 0; i < s.targets.size() && i < 12; ++i) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%-28.28s %14s  %6llu hits\r\n",
                    s.targets[i].name.empty() ? "?" : s.targets[i].name.c_str(),
                    FormatCount(s.targets[i].total).c_str(),
                    (unsigned long long)s.targets[i].hits);
        targets += line;
    }
    SetWindowTextW(targetBox_, Widen(targets).c_str());

    overlay_.Render(s);
}

LRESULT AppWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateControls();
        ReloadConfig();
        // Global hotkeys: the game has focus while they matter, so a keyboard
        // hook or a window-local accelerator would never see them.
        if (!RegisterHotKey(hwnd_, kHotkeyOverlay, 0, VK_F9))
            SetWindowTextW(statusBar_, L"F9 is already taken by another program");
        RegisterHotKey(hwnd_, kHotkeyReload, 0, VK_F10);
        SetTimer(hwnd_, kTimerRefresh, 250, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == kTimerRefresh) {
            Refresh();
            const std::wstring status = Widen(pipe_.Status());
            wchar_t current[256] = {};
            GetWindowTextW(statusBar_, current, 256);
            if (!status.empty() && status != current)
                SetWindowTextW(statusBar_, status.c_str());
        }
        return 0;

    case WM_HOTKEY:
        if (wp == kHotkeyOverlay) ToggleOverlay();
        else if (wp == kHotkeyReload) ReloadConfig();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdOverlayBtn: ToggleOverlay(); return 0;
        case kIdReloadBtn:  ReloadConfig();  return 0;
        case kIdResetBtn:   stats_.Reset();  Refresh(); return 0;
        default: break;
        }
        break;

    case WM_CTLCOLORSTATIC:
        // Static controls over the dialog-face background, so the value text
        // does not sit in a white box.
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kTimerRefresh);
        UnregisterHotKey(hwnd_, kHotkeyOverlay);
        UnregisterHotKey(hwnd_, kHotkeyReload);
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

int AppWindow::RunMessageLoop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

} // namespace grimnumbers
