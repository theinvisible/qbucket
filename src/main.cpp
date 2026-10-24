#include "accounts/AccountStore.h"
#include "accounts/QtKeychainBackend.h"
#include "accounts/SecretStore.h"
#include "controllers/SelfTest.h"
#include "controllers/SettingsController.h"
#include "diagnostics/Logging.h"
#include "diagnostics/StallWatchdog.h"
#include "platform/Console.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTextStream>
#include <QTimer>
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
        QCoreApplication::installTranslator(&plurals);
    static QTranslator appTranslator;
    if (appTranslator.load(QLocale(), QStringLiteral("qbucket"),
                           QStringLiteral("_"), QStringLiteral(":/i18n")))
        QCoreApplication::installTranslator(&appTranslator);

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
            QCoreApplication::installTranslator(&qtBaseTranslator);
            break;
        }
    }
}

void setIdentity()
{
    QCoreApplication::setApplicationName(QStringLiteral("qbucket"));
    QCoreApplication::setOrganizationName(QStringLiteral("qbucket"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QBUCKET_VERSION_STRING));
}

// --version, --help and --self-test run without a window (and without a
// display: the self-test is every package's smoke test). Anything else on
// the command line is left to QApplication, as before.
bool wantsCommandLine(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--self-test" || arg == "--version" || arg == "-v" || arg == "--help" || arg == "-h"
            || arg == "--help-all" || arg == "-?")
            return true;
    }
    return false;
}

int runCommandLine(int argc, char* argv[])
{
    qbucket::platform::attachParentConsole();
    QCoreApplication app(argc, argv);
    setIdentity();
    installTranslators();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Browse and manage S3-compatible object storage."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption selfTest(
        QStringLiteral("self-test"),
        QCoreApplication::translate("main", "Check the installation (Qt, sandbox, paths, keychain) and exit."));
    parser.addOption(selfTest);
    parser.process(app);   // --help / --version print and exit here
    if (!parser.isSet(selfTest))
        return 0;

    QTextStream out(stdout);
    qbucket::QtKeychainBackend keychain;
    qbucket::SelfTest test(&keychain, &out);
    int exitCode = 1;
    QObject::connect(&test, &qbucket::SelfTest::finished, &app, [&](int code) {
        exitCode = code;
        app.quit();
    });
    // A keychain that waits for an unlock prompt nobody answers.
    QTimer::singleShot(60000, &app, [&] {
        out << "Keychain:   no answer within 60 s\n";
        out.flush();
        app.quit();
    });
    QTimer::singleShot(0, &test, &qbucket::SelfTest::start);
    app.exec();
    return exitCode;
}

} // namespace

int main(int argc, char* argv[])
{
    if (wantsCommandLine(argc, argv))
        return runCommandLine(argc, argv);

    QApplication app(argc, argv);
    setIdentity();
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
