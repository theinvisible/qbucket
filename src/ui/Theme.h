#pragma once

#include <QColor>
#include <QString>

namespace qbucket {

// ---------------------------------------------------------------------------
// Theme — design tokens, ported 1:1 from xyz-profiler.
// ---------------------------------------------------------------------------
// `Palette` mirrors the CSS custom properties of the original Claude Design
// `theme.css` for one theme. The QSS template (DarkFusionStyle) and every
// custom painter read their colours from here, so the whole UI stays in sync
// with one source of truth — and in sync with xyz-profiler. Keep the token
// values identical between both projects.
//
// Accent is fixed to the canonical blue (#2563EB), density to "regular".
// ---------------------------------------------------------------------------
struct Palette {
    QColor accent;       // --accent
    QColor accentFg;     // --accent-fg
    QColor accentHover;  // mix(accent 88%, black)
    QColor bg;           // --bg     (desktop behind the window)
    QColor win;          // --win    (window body / object list background)
    QColor panel;        // --panel  (detail pane, dialogs, toolbar)
    QColor panel2;       // --panel-2
    QColor panel3;       // --panel-3 (chips)
    QColor border;       // --border
    QColor borderStrong; // --border-strong
    QColor text;         // --text
    QColor text2;        // --text-2
    QColor text3;        // --text-3
    QColor hover;        // --hover
    QColor sel;          // --sel
    QColor selLine;      // --sel-line
    QColor titlebar;     // --titlebar
    QColor inset;        // --inset  (input fields)
};

namespace Theme {

// Whether the currently-applied theme is the dark variant. Set by
// DarkFusionStyle::applyTheme(); read by every custom painter.
bool isDark();
void setDark(bool dark);

const Palette& palette(bool dark);
inline const Palette& current() { return palette(isDark()); }

constexpr const char* kAccent = "#2563EB";

// Semantic status colours (same in both themes; chosen to stay readable on
// either background).
inline QColor successColor() { return QColor(0x2f, 0x9e, 0x5b); }
inline QColor warningColor() { return QColor(0xd9, 0x8a, 0x1c); }
inline QColor dangerColor()  { return QColor(0xd1, 0x43, 0x43); }

// Storage-class badge for the object list. `label` is the short display
// text, `fg` the badge colour, `tinted` false for the neutral default class.
struct StorageClassBadge {
    QString label;
    QColor  fg;
    bool    tinted;
};
StorageClassBadge storageClassBadge(const QString& storageClass);

// Tint for the file icon in the object list, by file extension (without the
// dot, any case): archives, images, video/audio, text/code, other.
QColor fileTypeColor(const QString& extension);

} // namespace Theme
} // namespace qbucket
