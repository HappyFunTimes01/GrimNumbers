// Overlay layout, loaded from a text file so panels can be moved, retitled
// and re-populated without a rebuild.
//
// The model is deliberately small: a panel is a titled box of rows, anchored
// to a screen corner, and a row is either literal text with {token}
// placeholders or one of the two dynamic expansions (elements, targets).
// Panels anchored to the same corner stack away from it in file order, so the
// common case needs no coordinates at all; dx/dy exist for the case that does.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace grimnumbers {

enum class Anchor { TopLeft, TopRight, BottomLeft, BottomRight };

struct LayoutRow {
    enum class Kind { Text, Separator, Elements, Targets };

    Kind        kind  = Kind::Text;
    std::string label;          // left column, tokens expanded
    std::string value;          // right column, right-aligned, tokens expanded
    int         count = 5;      // rows to show, for Elements / Targets
};

struct LayoutPanel {
    std::string id;
    std::string title;          // empty hides the title bar
    Anchor      anchor    = Anchor::BottomRight;
    bool        anchorSet = false;   // false means "inherit the overlay anchor"
    int         dx = 0;
    int         dy = 0;
    int         width = 0;      // 0 means "inherit the overlay width"
    bool        visible = true;
    std::vector<LayoutRow> rows;
};

struct Layout {
    Anchor   anchor     = Anchor::BottomRight;
    int      margin     = 28;   // px from the screen edges
    int      gap        = 8;    // px between stacked panels
    int      padding    = 10;   // px inside a panel
    int      width      = 320;
    float    opacity    = 0.88f;
    float    fontSize   = 14.0f;
    std::wstring font   = L"Consolas";

    uint32_t bg         = 0x0E1014;   // 0xRRGGBB
    uint32_t border     = 0x3A4250;
    uint32_t titleText  = 0xE8C27A;
    uint32_t labelText  = 0x9AA4B2;
    uint32_t valueText  = 0xF0F3F7;
    uint32_t warnText   = 0xE06C5A;
    uint32_t elemFallback = 0xB8C0CC;

    std::vector<LayoutPanel> panels;

    // Replaces the layout with the built-in one, so the overlay works with no
    // config file present.
    void Defaults();

    // Replaces the layout with the file's contents. Returns false and leaves
    // the current layout untouched if the file cannot be read.
    bool Load(const wchar_t* path);

    const LayoutPanel* Find(const std::string& id) const;
};

} // namespace grimnumbers
