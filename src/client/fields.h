// Named values a front end can show, plus the damage-type label table.
//
// Overlay rows are text with {token} placeholders, so a row can be edited in a
// config file without touching code. FieldValue is the one place that knows
// how each number is formatted.
#pragma once

#include "stats.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace grimnumbers {

// Maps a raw CombatAttributeType to a label and a colour.
//
// The game's numeric damage-type ids are not recoverable from the export
// table, so this table is filled in by observation: run
// with a known skill, watch which id moves, write it down. Unmapped ids are
// rendered as "type N" - never guessed at, because a wrong label is worse
// than a number.
class ElementTable {
public:
    struct Entry {
        uint32_t    type;
        std::string label;
        uint32_t    rgb;     // 0xRRGGBB
    };

    // Lines are "<id> = <label>[, RRGGBB]". Missing file is not an error.
    bool Load(const wchar_t* path);

    const Entry* Find(uint32_t type) const;
    std::string  Label(uint32_t type) const;
    uint32_t     Color(uint32_t type, uint32_t fallback) const;

    size_t Count() const { return entries_.size(); }

private:
    std::vector<Entry> entries_;
};

// Thousands-separated integer, e.g. 1,284,903.
std::string FormatCount(double v);
// Compact magnitude for tight columns, e.g. 1.28M, 94.1k, 812.
std::string FormatShort(double v);

// Returns the value for `token` (without braces), or an empty string when the
// token is unknown - so a typo in the config shows up as a blank, not a crash.
std::string FieldValue(const std::string& token, const Snapshot& s);

// Substitutes every {token} in `text`.
std::string ExpandTokens(const std::string& text, const Snapshot& s);

} // namespace grimnumbers
