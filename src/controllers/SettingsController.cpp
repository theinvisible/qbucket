#include "controllers/SettingsController.h"

#include <algorithm>

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
constexpr auto kKeyDownloadDir    = "transfers/download_directory";
constexpr auto kKeyParallelParts  = "transfers/parallel_parts";
constexpr auto kKeyThreads        = "transfers/threads";
constexpr auto kKeyBookmarksOpen  = "ui/bookmarks_expanded";

QString systemDownloadDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

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
    m_downloadDirectory = m_store->value(QLatin1String(kKeyDownloadDir)).toString();
    if (m_downloadDirectory.isEmpty())
        m_downloadDirectory = systemDownloadDirectory();
    m_parallelParts = std::clamp(m_store->value(QLatin1String(kKeyParallelParts), 16).toInt(),
                                 kMinParallelParts, kMaxParallelParts);
    m_transferThreads = std::clamp(m_store->value(QLatin1String(kKeyThreads), 4).toInt(),
                                   kMinTransferThreads, kMaxTransferThreads);
    m_bookmarksExpanded = m_store->value(QLatin1String(kKeyBookmarksOpen), true).toBool();
}

void SettingsController::setBookmarksExpanded(bool expanded)
{
    if (m_bookmarksExpanded == expanded) return;
    m_bookmarksExpanded = expanded;
    write_(QString::fromLatin1(kKeyBookmarksOpen), m_bookmarksExpanded);
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

void SettingsController::setDownloadDirectory(const QString& dir)
{
    const QString next = dir.trimmed().isEmpty() ? systemDownloadDirectory() : QDir::cleanPath(dir.trimmed());
    if (m_downloadDirectory == next) return;
    m_downloadDirectory = next;
    write_(QString::fromLatin1(kKeyDownloadDir), m_downloadDirectory);
    emit transferSettingsChanged();
}

void SettingsController::setTransferParallelParts(int parts)
{
    const int next = std::clamp(parts, kMinParallelParts, kMaxParallelParts);
    if (m_parallelParts == next) return;
    m_parallelParts = next;
    write_(QString::fromLatin1(kKeyParallelParts), m_parallelParts);
    emit transferSettingsChanged();
}

void SettingsController::setTransferThreads(int threads)
{
    const int next = std::clamp(threads, kMinTransferThreads, kMaxTransferThreads);
    if (m_transferThreads == next) return;
    m_transferThreads = next;
    write_(QString::fromLatin1(kKeyThreads), m_transferThreads);
    emit transferSettingsChanged();
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
