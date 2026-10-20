#pragma once

#include <QString>

namespace qbucket {

// Maps an object key (relative to what was selected) onto a relative local
// path that stays inside the target folder (PLAN.md §12: downloads never
// write outside the folder the user chose):
//   - segments "." and ".." are refused (such keys can't be requested
//     either: Qt normalises them away);
//   - empty segments ("a//b", a leading "/") become "_";
//   - characters the platform forbids become "_" — on Windows < > : " | ? *
//     \ and control characters, trailing dots and spaces, and the reserved
//     device names (CON, PRN, AUX, NUL, COM1–9, LPT1–9, with any extension)
//     get a leading "_";
//   - segments longer than 255 bytes are refused;
//   - a trailing "/" (a folder marker) maps to a folder.
// Pure; thread-safe.
class LocalPathMapper {
public:
    enum class Rules { Posix, Windows };

    struct Result {
        bool ok = false;
        QString path;       // relative, "/"-separated; folders end with "/"
        bool changed = false;
        QString reason;     // why refused, or what was changed
    };

    [[nodiscard]] static Rules nativeRules();
    [[nodiscard]] static Result map(const QString& relativeKey, Rules rules = nativeRules());
    // How two mapped paths compare on this file system (Windows: case-insensitive).
    [[nodiscard]] static QString normalized(const QString& path, Rules rules = nativeRules());
};

} // namespace qbucket
