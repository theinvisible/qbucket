#include "controllers/SettingsController.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/ThemeManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLineEdit>
#include <QStandardPaths>
#include <QToolButton>
#include <QtTest>

using namespace qbucket;

// Smoke test for the real main window: it builds in both themes, nothing
// that mutates data is reachable yet, and the theme / browse-mode controls
// round-trip through the settings.
//
// Set QBUCKET_SCREENSHOT_DIR to also save main-dark.png / main-light.png
// (run with QT_QPA_PLATFORM=windows or xcb/wayland for real font rendering).
class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("qbucket-test"));
        QCoreApplication::setOrganizationName(QStringLiteral("qbucket-test"));
        SettingsController probe;
        QFile::remove(probe.fileName());
    }

    void buildsAndKeepsMutationsDisabled()
    {
        SettingsController settings;
        ThemeManager theme;
        theme.apply(settings.themeName());

        MainWindow window(&settings, &theme);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // Every data-changing toolbar action stays disabled until it is wired
        // through the OperationController (PLAN.md §12.1).
        const auto primary = window.findChildren<QToolButton*>(QStringLiteral("tbPrimary"));
        QVERIFY(!primary.isEmpty());
        for (QToolButton* btn : primary)
            QVERIFY2(!btn->isEnabled(), qPrintable(btn->text()));

        int disabledIconButtons = 0;
        for (QToolButton* btn : window.findChildren<QToolButton*>(QStringLiteral("tbIcon")))
            if (!btn->isEnabled()) ++disabledIconButtons;
        QVERIFY(disabledIconButtons >= 7);   // download, folder, delete, back, fwd, up, refresh

        QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("transferDock")));
        saveScreenshot(window, QStringLiteral("main-dark.png"));
    }

    void themeToggleRoundTripsThroughSettings()
    {
        SettingsController settings;
        settings.setThemeName(QStringLiteral("Dark"));
        ThemeManager theme;
        theme.apply(settings.themeName());
        connect(&settings, &SettingsController::themeNameChanged,
                &theme, [&] { theme.apply(settings.themeName()); });

        MainWindow window(&settings, &theme);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QToolButton* toggle = nullptr;
        for (QToolButton* btn : window.findChildren<QToolButton*>(QStringLiteral("tbIcon")))
            if (btn->isEnabled() && btn->toolTip().contains(QStringLiteral("light")))
                toggle = btn;
        QVERIFY(toggle);

        toggle->click();
        QCOMPARE(settings.themeName(), QStringLiteral("Light"));
        QVERIFY(!Theme::isDark());
        saveScreenshot(window, QStringLiteral("main-light.png"));

        toggle->click();
        QCOMPARE(settings.themeName(), QStringLiteral("Dark"));
        QVERIFY(Theme::isDark());
    }

    void browseModeSegmentsFollowSettings()
    {
        SettingsController settings;
        settings.setBrowseMode(QStringLiteral("folders"));
        ThemeManager theme;
        theme.apply(QStringLiteral("Dark"));
        MainWindow window(&settings, &theme);

        const auto segments = window.findChildren<QToolButton*>(QStringLiteral("segText"));
        QCOMPARE(segments.size(), 3);
        const auto checkedMode = [&] {
            for (QToolButton* btn : segments)
                if (btn->isChecked()) return btn->property("browseMode").toString();
            return QString();
        };
        QCOMPARE(checkedMode(), QStringLiteral("folders"));

        settings.setBrowseMode(QStringLiteral("versions"));
        QCOMPARE(checkedMode(), QStringLiteral("versions"));

        for (QToolButton* btn : segments)
            if (btn->property("browseMode").toString() == QLatin1String("flat"))
                btn->click();
        QCOMPARE(settings.browseMode(), QStringLiteral("flat"));
        QCOMPARE(checkedMode(), QStringLiteral("flat"));
    }

private:
    static void saveScreenshot(QWidget& window, const QString& fileName)
    {
        const QString dir = qEnvironmentVariable("QBUCKET_SCREENSHOT_DIR");
        if (dir.isEmpty())
            return;
        window.resize(1280, 760);
        QTest::qWait(150);
        QDir().mkpath(dir);
        QVERIFY(window.grab().save(QDir(dir).filePath(fileName)));
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_main_window.moc"
