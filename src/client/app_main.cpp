// grimnumbers_app - the desktop window and the in-game overlay.
//
//   grimnumbers_app [--window SECONDS] [--overlay]
//
//     --window N   rolling window in seconds (default 5)
//     --overlay    start with the in-game overlay already showing
//
// F9 toggles the overlay, F10 re-reads overlay.ini and elements.ini. Both are
// global hotkeys, because Grim Dawn has focus when they are pressed.
//
// This file is wiring only: the pipe reader, the stats model, the window and
// the overlay are each their own module and none of them knows about main().
#include "appwindow.h"
#include "fields.h"
#include "layout.h"
#include "overlay.h"
#include "pipe_client.h"
#include "stats.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

// Config lives next to the executable, so the build can copy the shipped
// defaults there and an edit does not need a rebuild.
std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n == MAX_PATH) return L".";

    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

struct Options {
    double windowSec   = 5.0;
    bool   showOverlay = false;
};

Options ParseCommandLine()
{
    Options o;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--window") == 0 && i + 1 < argc) {
            o.windowSec = _wtof(argv[++i]);
            if (o.windowSec < 0.5) o.windowSec = 0.5;
        } else if (wcscmp(argv[i], L"--overlay") == 0) {
            o.showOverlay = true;
        }
    }
    LocalFree(argv);
    return o;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    using namespace grimnumbers;

    const Options opt = ParseCommandLine();
    const std::wstring dir = ExeDir();
    const std::wstring layoutPath   = dir + L"\\overlay.ini";
    const std::wstring elementsPath = dir + L"\\elements.ini";

    // Built-in layout first, so a missing or unreadable overlay.ini still
    // leaves a usable overlay rather than an empty window.
    Layout layout;
    layout.Defaults();
    layout.Load(layoutPath.c_str());

    ElementTable elements;
    elements.Load(elementsPath.c_str());

    Stats stats(opt.windowSec);

    Overlay overlay(layout, elements);
    if (!overlay.Create(instance)) {
        MessageBoxW(nullptr, L"Could not create the overlay window.",
                    L"Grim Numbers", MB_ICONERROR | MB_OK);
        return 1;
    }

    PipeClient pipe(stats);

    // Set before the window exists so its overlay button comes up labelled
    // to match.
    if (opt.showOverlay) overlay.SetVisible(true);

    AppWindow window(stats, pipe, overlay, layout, elements);
    window.SetConfigPaths(layoutPath, elementsPath);
    if (!window.Create(instance, show)) {
        MessageBoxW(nullptr, L"Could not create the main window.",
                    L"Grim Numbers", MB_ICONERROR | MB_OK);
        return 1;
    }

    pipe.Start();
    const int rc = window.RunMessageLoop();

    pipe.Stop();
    overlay.Destroy();
    return rc;
}
