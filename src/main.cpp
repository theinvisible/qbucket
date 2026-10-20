#include "accounts/AccountStore.h"
#include "accounts/QtKeychainBackend.h"
#include "accounts/SecretStore.h"
#include "controllers/SettingsController.h"
#include "diagnostics/Logging.h"
#include "diagnostics/StallWatchdog.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <memory>

namespace {

void installTranslators()
{
    // Keep the translators alive for the application's lifetime.
    // English plural forms first ("1 object", not "1 object(s)"), for every
    // language without its own file; a translation installed later wins.
    static QTranslator plurals;
    if (plurals.load(QStringLiteral(":/i18n/qbucket_en")))
        QApplication::installTranslator(&plurals);
    static QTranslator appTranslator;
    if (appTranslator.load(QLocale(), QStringLiteral("qbucket"),
                           QStringLiteral("_"), QStringLiteral(":/i18n")))
        QApplication::installTranslator(&appTranslator);

    // Qt's own strings (standard dialog buttons …) live in qtbase_<locale>.qm.
    // Try the Qt install's translations dir first, then a translations/ folder
    // next to the executable (where windeployqt copies them).
    const QStringList qtTrDirs = {
        QLibraryInfo::path(QLibraryInfo::TranslationsPath),
        QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
    };
    static QTranslator qtBaseTranslator;
    for (const QString& dir : qtTrDirs) {
        if (qtBaseTranslator.load(QLocale(), QStringLiteral("qtbase"),
                                  QStringLiteral("_"), dir)) {
            QApplication::installTranslator(&qtBaseTranslator);
            break;
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbucket"));
    QCoreApplication::setOrganizationName(QStringLiteral("qbucket"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QBUCKET_VERSION_STRING));
    // Wayland / desktop integration: ties the window to the installed
    // .desktop file (icon, name, Flatpak id).
    QGuiApplication::setDesktopFileName(QStringLiteral(QBUCKET_APP_ID));
#ifdef Q_OS_WIN
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
#else
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/qbucket.svg")));
#endif

    installTranslators();

    qbucket::SettingsController settings;
    qbucket::ThemeManager theme;
    theme.apply(settings.themeName());
    QObject::connect(&settings, &qbucket::SettingsController::themeNameChanged,
                     &theme, [&] { theme.apply(settings.themeName()); });

    // Development guard for the "UI never blocks" rule (PLAN.md §3).
    qbucket::StallWatchdog watchdog;
    if (qbucket::StallWatchdog::enabledByDefault()) {
        watchdog.start();
        qCInfo(lcApp) << "GUI stall watchdog active, threshold"
                      << watchdog.thresholdMs() << "ms";
    }

    qbucket::AccountStore accounts;
    qbucket::SecretStore secrets{std::make_unique<qbucket::QtKeychainBackend>()};

    qbucket::MainWindow window(&settings, &theme, &accounts, &secrets);
    window.show();

    return app.exec();
}
