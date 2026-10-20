#include "controllers/SettingsController.h"

#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

using namespace qbucket;

class TestSettingsController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Redirects AppConfigLocation to a throwaway test directory.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-test"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
    }

    void init()
    {
        SettingsController probe;
        QFile::remove(probe.fileName());
    }

    void defaults()
    {
        SettingsController s;
        QCOMPARE(s.themeName(), QStringLiteral("Dark"));
        QCOMPARE(s.browseMode(), QStringLiteral("folders"));
        QVERIFY(s.windowGeometry().isEmpty());
    }

    void normalizesInvalidInput()
    {
        QCOMPARE(SettingsController::normalizedThemeName(QStringLiteral("Purple")),
                 QStringLiteral("Dark"));
        QCOMPARE(SettingsController::normalizedThemeName(QStringLiteral("System")),
                 QStringLiteral("System"));
        QCOMPARE(SettingsController::normalizedBrowseMode(QStringLiteral("tree")),
                 QStringLiteral("folders"));
        QCOMPARE(SettingsController::normalizedBrowseMode(QStringLiteral("versions")),
                 QStringLiteral("versions"));
    }

    void emitsOnlyOnRealChange()
    {
        SettingsController s;
        QSignalSpy themeSpy(&s, &SettingsController::themeNameChanged);
        QSignalSpy modeSpy(&s, &SettingsController::browseModeChanged);

        s.setThemeName(QStringLiteral("Light"));
        s.setThemeName(QStringLiteral("Light"));
        QCOMPARE(themeSpy.count(), 1);

        // Invalid input normalises to the current value → no signal.
        s.setBrowseMode(QStringLiteral("folders"));
        s.setBrowseMode(QStringLiteral("nonsense"));
        QCOMPARE(modeSpy.count(), 0);
        s.setBrowseMode(QStringLiteral("flat"));
        QCOMPARE(modeSpy.count(), 1);
    }

    void persistsAcrossInstances()
    {
        {
            SettingsController s;
            s.setThemeName(QStringLiteral("System"));
            s.setBrowseMode(QStringLiteral("versions"));
            s.setMainSplitterState(QStringLiteral("AAEC"));
        }
        SettingsController reloaded;
        QCOMPARE(reloaded.themeName(), QStringLiteral("System"));
        QCOMPARE(reloaded.browseMode(), QStringLiteral("versions"));
        QCOMPARE(reloaded.mainSplitterState(), QStringLiteral("AAEC"));
    }

    void handEditedIniIsNormalizedOnLoad()
    {
        QString path;
        {
            SettingsController s;
            path = s.fileName();
        }
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("ui/theme"), QStringLiteral("Neon"));
            raw.setValue(QStringLiteral("ui/browse_mode"), QStringLiteral("grid"));
        }
        SettingsController s;
        QCOMPARE(s.themeName(), QStringLiteral("Dark"));
        QCOMPARE(s.browseMode(), QStringLiteral("folders"));
    }

    void transferSettings()
    {
        SettingsController s;
        QVERIFY(!s.downloadDirectory().isEmpty());   // the system's download folder
        QCOMPARE(s.transferParallelParts(), 16);
        QCOMPARE(s.transferThreads(), 4);
        QSignalSpy changed(&s, &SettingsController::transferSettingsChanged);
        s.setTransferParallelParts(1000);
        QCOMPARE(s.transferParallelParts(), SettingsController::kMaxParallelParts);
        s.setTransferParallelParts(SettingsController::kMaxParallelParts);
        QCOMPARE(changed.count(), 1);
        s.setTransferThreads(0);
        QCOMPARE(s.transferThreads(), SettingsController::kMinTransferThreads);
        s.setDownloadDirectory(QStringLiteral("/tmp/x/../downloads/"));
        QCOMPARE(s.downloadDirectory(), QStringLiteral("/tmp/downloads"));
        QCOMPARE(changed.count(), 3);

        SettingsController reloaded;
        QCOMPARE(reloaded.transferParallelParts(), SettingsController::kMaxParallelParts);
        QCOMPARE(reloaded.transferThreads(), 1);
        QCOMPARE(reloaded.downloadDirectory(), QStringLiteral("/tmp/downloads"));
        reloaded.setDownloadDirectory(QString());   // back to the default
        QVERIFY(reloaded.downloadDirectory() != QStringLiteral("/tmp/downloads"));
    }
};

QTEST_GUILESS_MAIN(TestSettingsController)
#include "test_settings_controller.moc"
