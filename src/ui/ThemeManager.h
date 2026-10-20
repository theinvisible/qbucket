#pragma once

#include <QObject>
#include <QString>

namespace qbucket {

// Owns the active theme choice ("Dark" / "Light" / "System") and applies it
// through DarkFusionStyle. With "System" it follows the OS colour scheme
// live. Custom painters and icon owners connect to themeChanged() and
// re-tint; they read the colours from Theme::current().
class ThemeManager : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);

    // Applies the theme and emits themeChanged() — also when re-applying the
    // same name, so a caller can force a refresh.
    void apply(const QString& themeName);

    QString themeName() const { return m_themeName; }
    bool    isDark()    const;

signals:
    void themeChanged();

private:
    void onColorSchemeChanged_();

    QString m_themeName = QStringLiteral("Dark");
};

} // namespace qbucket
