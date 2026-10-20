#pragma once

#include <QString>

class QPalette;

namespace qbucket {

// Applies the QBucket look (ported from xyz-profiler, which ported it from
// the Claude Design `theme.css`): Fusion base style + a themed QPalette + an
// application-wide QSS, for the Dark / Light / System variants. The accent is
// the canonical blue and the density is "regular".
//
// Prefer ThemeManager in application code — it also follows the OS colour
// scheme live and tells the custom painters to refresh.
class DarkFusionStyle {
public:
    DarkFusionStyle() = delete;

    // Sets the Fusion style, the matching palette/font, marks Theme::setDark,
    // and installs the app-wide stylesheet. "System" follows the OS scheme.
    // Returns whether the dark variant was applied.
    static bool applyTheme(const QString& themeName);

    // Resolves "Dark" / "Light" / "System" to the variant that would apply.
    static bool resolvesToDark(const QString& themeName);

    // The full themed stylesheet for the given variant.
    static QString styleSheet(bool dark);

    static QPalette lightPalette();
    static QPalette darkPalette();
};

} // namespace qbucket
