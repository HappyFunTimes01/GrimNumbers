#include "layout.h"
#include "strutil.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cctype>

namespace grimnumbers {
namespace {

bool ParseAnchor(const std::string& raw, Anchor& out)
{
    const std::string s = Lower(Trim(raw));
    if (s == "top-left"     || s == "topleft")     { out = Anchor::TopLeft;     return true; }
    if (s == "top-right"    || s == "topright")    { out = Anchor::TopRight;    return true; }
    if (s == "bottom-left"  || s == "bottomleft")  { out = Anchor::BottomLeft;  return true; }
    if (s == "bottom-right" || s == "bottomright") { out = Anchor::BottomRight; return true; }
    return false;
}

uint32_t ParseRgb(const std::string& s, uint32_t fallback)
{
    std::string v = Trim(s);
    if (!v.empty() && v[0] == '#') v.erase(0, 1);
    if (v.empty()) return fallback;

    char* end = nullptr;
    const unsigned long n = strtoul(v.c_str(), &end, 16);
    return (end && *end == '\0') ? (uint32_t)(n & 0xFFFFFFu) : fallback;
}

// "Label | {token}" splits on the first pipe. A row with no pipe is all
// label, which is how headings and notes are written.
LayoutRow TextRow(const std::string& spec)
{
    LayoutRow r;
    r.kind = LayoutRow::Kind::Text;

    const size_t bar = spec.find('|');
    if (bar == std::string::npos) {
        r.label = Trim(spec);
    } else {
        r.label = Trim(spec.substr(0, bar));
        r.value = Trim(spec.substr(bar + 1));
    }
    return r;
}

int ParseCount(const std::string& v, int fallback)
{
    const std::string s = Trim(v);
    if (s.empty()) return fallback;
    const long n = strtol(s.c_str(), nullptr, 10);
    return (n > 0 && n < 64) ? (int)n : fallback;
}

} // namespace

void Layout::Defaults()
{
    *this = Layout();   // reset every global to its declared default

    LayoutPanel dps;
    dps.id    = "dps";
    dps.title = "Grim Numbers";
    dps.rows.push_back(TextRow("DPS {window}s avg | {dps.rolling}/s"));
    dps.rows.push_back(TextRow("DPS session {dps.span}s | {dps.session}/s"));
    dps.rows.push_back(TextRow("Damage total | {dmg.total.s}  ({hits.total} hits)"));
    dps.rows.push_back(TextRow("Crit rate | {crit.rate}"));
    dps.rows.push_back(TextRow("Biggest hit | {hit.biggest.s}"));

    LayoutRow sep;
    sep.kind = LayoutRow::Kind::Separator;
    dps.rows.push_back(sep);

    dps.rows.push_back(TextRow("DTPS {window}s avg | {dtps.rolling}/s"));
    dps.rows.push_back(TextRow("Taken total | {taken.total.s}"));
    panels.push_back(dps);

    LayoutPanel elems;
    elems.id    = "elements";
    elems.title = "Elements (DPS)";
    LayoutRow er;
    er.kind  = LayoutRow::Kind::Elements;
    er.count = 6;
    elems.rows.push_back(er);
    panels.push_back(elems);
}

bool Layout::Load(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;

    Layout parsed;
    parsed.panels.clear();
    LayoutPanel* panel = nullptr;   // null until the first [panel:...] section

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;

        if (s[0] == '[') {
            const size_t close = s.find(']');
            if (close == std::string::npos) continue;
            const std::string name = Lower(Trim(s.substr(1, close - 1)));

            if (name == "overlay") {
                panel = nullptr;
            } else if (name.compare(0, 6, "panel:") == 0) {
                LayoutPanel p;
                p.id = Trim(name.substr(6));
                parsed.panels.push_back(p);
                panel = &parsed.panels.back();
            } else {
                panel = nullptr;      // unknown section: its keys are ignored
            }
            continue;
        }

        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(s.substr(0, eq)));
        const std::string val = Trim(s.substr(eq + 1));

        if (!panel) {
            // [overlay] globals.
            if      (key == "anchor")   ParseAnchor(val, parsed.anchor);
            else if (key == "margin")   parsed.margin   = atoi(val.c_str());
            else if (key == "gap")      parsed.gap      = atoi(val.c_str());
            else if (key == "padding")  parsed.padding  = atoi(val.c_str());
            else if (key == "width")    parsed.width    = atoi(val.c_str());
            else if (key == "opacity")  parsed.opacity  = (float)atof(val.c_str());
            else if (key == "fontsize") parsed.fontSize = (float)atof(val.c_str());
            else if (key == "font")     parsed.font     = Widen(val);
            else if (key == "bg")       parsed.bg        = ParseRgb(val, parsed.bg);
            else if (key == "border")   parsed.border    = ParseRgb(val, parsed.border);
            else if (key == "title")    parsed.titleText = ParseRgb(val, parsed.titleText);
            else if (key == "label")    parsed.labelText = ParseRgb(val, parsed.labelText);
            else if (key == "value")    parsed.valueText = ParseRgb(val, parsed.valueText);
            else if (key == "warn")     parsed.warnText  = ParseRgb(val, parsed.warnText);
            else if (key == "element")  parsed.elemFallback = ParseRgb(val, parsed.elemFallback);
            continue;
        }

        // Panel keys. row / separator / elements / targets append in file
        // order, and that order is what gives a panel its vertical layout.
        if      (key == "title")   panel->title = val;
        else if (key == "width")   panel->width = atoi(val.c_str());
        else if (key == "dx")      panel->dx = atoi(val.c_str());
        else if (key == "dy")      panel->dy = atoi(val.c_str());
        else if (key == "anchor")  panel->anchorSet = ParseAnchor(val, panel->anchor);
        else if (key == "row")     panel->rows.push_back(TextRow(val));
        else if (key == "visible") {
            const std::string v = Lower(val);
            panel->visible = !(v == "0" || v == "false" || v == "no" || v == "off");
        } else if (key == "separator") {
            LayoutRow r; r.kind = LayoutRow::Kind::Separator;
            panel->rows.push_back(r);
        } else if (key == "elements") {
            LayoutRow r; r.kind = LayoutRow::Kind::Elements;
            r.count = ParseCount(val, 6);
            panel->rows.push_back(r);
        } else if (key == "targets") {
            LayoutRow r; r.kind = LayoutRow::Kind::Targets;
            r.count = ParseCount(val, 5);
            panel->rows.push_back(r);
        }
    }
    fclose(f);

    // Clamp what a typo could make unusable, so a bad config still leaves a
    // readable overlay rather than an invisible one.
    if (parsed.width    < 120)   parsed.width    = 120;
    if (parsed.padding  < 0)     parsed.padding  = 0;
    if (parsed.gap      < 0)     parsed.gap      = 0;
    if (parsed.margin   < 0)     parsed.margin   = 0;
    if (parsed.fontSize < 7.0f)  parsed.fontSize = 7.0f;
    if (parsed.fontSize > 48.0f) parsed.fontSize = 48.0f;
    if (parsed.opacity  < 0.15f) parsed.opacity  = 0.15f;
    if (parsed.opacity  > 1.0f)  parsed.opacity  = 1.0f;
    if (parsed.font.empty())     parsed.font     = L"Consolas";

    *this = parsed;
    return true;
}

const LayoutPanel* Layout::Find(const std::string& id) const
{
    for (const LayoutPanel& p : panels)
        if (p.id == id) return &p;
    return nullptr;
}

} // namespace grimnumbers
