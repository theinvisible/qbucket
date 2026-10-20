#include "ui/ThemeManager.h"

#include "ui/DarkFusionStyle.h"
#include "ui/Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace qbucket {

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &ThemeManager::onColorSchemeChanged_);
}

bool ThemeManager::isDark() const
{
    return Theme::isDark();
}

void ThemeManager::apply(const QString& themeName)
{
    m_themeName = themeName;
    DarkFusionStyle::applyTheme(m_themeName);
    emit themeChanged();
}

void ThemeManager::onColorSchemeChanged_()
{
    // Only "System" follows the OS; an explicit Dark/Light choice stays put.
    if (m_themeName != QLatin1String("System"))
        return;
    if (DarkFusionStyle::resolvesToDark(m_themeName) == Theme::isDark())
        return;
    apply(m_themeName);
}

} // namespace qbucket
