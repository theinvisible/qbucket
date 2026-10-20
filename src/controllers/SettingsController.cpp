#include "controllers/SettingsController.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

namespace qbucket {
namespace {

constexpr auto kKeyTheme          = "ui/theme";
constexpr auto kKeyBrowseMode     = "ui/browse_mode";
constexpr auto kKeyWindowGeometry = "ui/window_geometry";
constexpr auto kKeyWindowState    = "ui/window_state";
constexpr auto kKeyMainSplitter   = "ui/main_splitter_state";

QString settingsPath()
{
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/qbucket.ini");
}

} // namespace

SettingsController::SettingsController(QObject* parent)
    : QObject(parent),
      m_store(new QSettings(settingsPath(), QSettings::IniFormat, this)),
      m_themeName(QStringLiteral("Dark")),
      m_browseMode(QStringLiteral("folders"))
{
    load_();
}

SettingsController::~SettingsController() = default;

QString SettingsController::normalizedThemeName(const QString& name)
{
    if (name == QLatin1String("Light") || name == QLatin1String("System"))
        return name;
    return QStringLiteral("Dark");
}

QString SettingsController::normalizedBrowseMode(const QString& mode)
{
    if (mode == QLatin1String("flat") || mode == QLatin1String("versions"))
        return mode;
    return QStringLiteral("folders");
}

QString SettingsController::fileName() const
{
    return m_store->fileName();
}

void SettingsController::load_()
{
    // Normalise on read too: a hand-edited INI must not put the UI into a
    // state no code path expects.
    m_themeName  = normalizedThemeName(
        m_store->value(QLatin1String(kKeyTheme)).toString());
    m_browseMode = normalizedBrowseMode(
        m_store->value(QLatin1String(kKeyBrowseMode)).toString());
    m_windowGeometry    = m_store->value(QLatin1String(kKeyWindowGeometry)).toString();
    m_windowState       = m_store->value(QLatin1String(kKeyWindowState)).toString();
    m_mainSplitterState = m_store->value(QLatin1String(kKeyMainSplitter)).toString();
}

void SettingsController::write_(const QString& key, const QVariant& value)
{
    m_store->setValue(key, value);
    m_store->sync();
}

void SettingsController::setThemeName(const QString& name)
{
    const QString next = normalizedThemeName(name);
    if (m_themeName == next) return;
    m_themeName = next;
    write_(QString::fromLatin1(kKeyTheme), m_themeName);
    emit themeNameChanged();
}

void SettingsController::setBrowseMode(const QString& mode)
{
    const QString next = normalizedBrowseMode(mode);
    if (m_browseMode == next) return;
    m_browseMode = next;
    write_(QString::fromLatin1(kKeyBrowseMode), m_browseMode);
    emit browseModeChanged();
}

void SettingsController::setWindowGeometry(const QString& base64)
{
    if (m_windowGeometry == base64) return;
    m_windowGeometry = base64;
    write_(QString::fromLatin1(kKeyWindowGeometry), m_windowGeometry);
}

void SettingsController::setWindowState(const QString& base64)
{
    if (m_windowState == base64) return;
    m_windowState = base64;
    write_(QString::fromLatin1(kKeyWindowState), m_windowState);
}

void SettingsController::setMainSplitterState(const QString& base64)
{
    if (m_mainSplitterState == base64) return;
    m_mainSplitterState = base64;
    write_(QString::fromLatin1(kKeyMainSplitter), m_mainSplitterState);
}

} // namespace qbucket
