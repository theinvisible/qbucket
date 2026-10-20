#include "ui/DarkFusionStyle.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStyle>
#include <QtTest>

using namespace qbucket;

// Guards the design-system port: tokens, stylesheet resolution, icons and
// theme switching. A broken token or icon path fails here instead of
// silently rendering wrong in the UI.
class TestUiFoundation : public QObject {
    Q_OBJECT

private slots:
    void palettesShareAccentButDifferInSurfaces()
    {
        const Palette& light = Theme::palette(false);
        const Palette& dark  = Theme::palette(true);
        QCOMPARE(light.accent, QColor(QLatin1String(Theme::kAccent)));
        QCOMPARE(dark.accent,  QColor(QLatin1String(Theme::kAccent)));
        QVERIFY(light.win  != dark.win);
        QVERIFY(light.text != dark.text);
        // Text must be readable on its window background in both variants.
        QVERIFY(light.text.lightness() < light.win.lightness());
        QVERIFY(dark.text.lightness()  > dark.win.lightness());
    }

    void styleSheetResolvesEveryToken_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark")  << true;
    }

    void styleSheetResolvesEveryToken()
    {
        QFETCH(bool, dark);
        const QString qss = DarkFusionStyle::styleSheet(dark);
        QVERIFY(!qss.isEmpty());
        // Any @token@ left over means a placeholder without a palette entry.
        static const QRegularExpression leftover(QStringLiteral("@[A-Za-z0-9]+@"));
        const auto match = leftover.match(qss);
        QVERIFY2(!match.hasMatch(), qPrintable(match.captured()));
        QVERIFY(qss.contains(Theme::palette(dark).win.name(QColor::HexRgb)));
    }

    void styleSheetHasNoCatchAllWidgetRule()
    {
        // xyz-profiler lesson: a bare `QWidget {` rule re-polishes the whole
        // tree on every theme switch.
        static const QRegularExpression catchAll(QStringLiteral("(^|\\n)\\s*QWidget\\s*\\{"));
        QVERIFY(!catchAll.match(DarkFusionStyle::styleSheet(true)).hasMatch());
    }

    void everyIconRenders()
    {
        const QStringList names = IconFactory::names();
        QVERIFY(names.size() >= 30);
        for (const QString& name : names) {
            const QPixmap pm = IconFactory::pixmap(name, Qt::black, 18, 1.6, 1.0);
            QVERIFY2(!pm.isNull(), qPrintable(name));
            QCOMPARE(pm.size(), QSize(18, 18));
            // A blank pixmap means the SVG path failed to draw anything.
            const QImage img = pm.toImage();
            bool anyInk = false;
            for (int y = 0; y < img.height() && !anyInk; ++y)
                for (int x = 0; x < img.width() && !anyInk; ++x)
                    anyInk = qAlpha(img.pixel(x, y)) > 0;
            QVERIFY2(anyInk, qPrintable(name));
        }
    }

    void unknownIconIsNull()
    {
        QVERIFY(IconFactory::pixmap(QStringLiteral("no-such-icon"), Qt::black).isNull());
        QVERIFY(IconFactory::icon(QStringLiteral("no-such-icon"), Qt::black).isNull());
    }

    void highDpiPixmapHasDoubleSize()
    {
        const QPixmap pm = IconFactory::pixmap(QStringLiteral("bucket"), Qt::black, 16, 1.6, 2.0);
        QCOMPARE(pm.size(), QSize(32, 32));
        QCOMPARE(pm.devicePixelRatio(), 2.0);
    }

    void storageClassBadges()
    {
        QCOMPARE(Theme::storageClassBadge(QString()).label, QStringLiteral("Standard"));
        QVERIFY(!Theme::storageClassBadge(QStringLiteral("STANDARD")).tinted);
        QVERIFY(Theme::storageClassBadge(QStringLiteral("standard_ia")).tinted);
        QCOMPARE(Theme::storageClassBadge(QStringLiteral("DEEP_ARCHIVE")).label,
                 QStringLiteral("Deep Archive"));
        // Unknown provider class keeps its label, neutral.
        const auto custom = Theme::storageClassBadge(QStringLiteral("COLD"));
        QCOMPARE(custom.label, QStringLiteral("COLD"));
        QVERIFY(!custom.tinted);
    }

    void fileTypeColours()
    {
        QVERIFY(Theme::fileTypeColor(QStringLiteral("ZST")) != Theme::fileTypeColor(QStringLiteral("png")));
        QCOMPARE(Theme::fileTypeColor(QStringLiteral("unknownext")), Theme::current().text2);
    }

    void themeManagerAppliesAndNotifies()
    {
        ThemeManager manager;
        QSignalSpy spy(&manager, &ThemeManager::themeChanged);

        manager.apply(QStringLiteral("Light"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(!Theme::isDark());
        QVERIFY(!manager.isDark());
        QCOMPARE(qApp->palette().color(QPalette::Window), Theme::palette(false).win);

        manager.apply(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 2);
        QVERIFY(Theme::isDark());
        QCOMPARE(qApp->palette().color(QPalette::Window), Theme::palette(true).win);
    }

    void themeSwitchKeepsTheStyleObject()
    {
        // Regression: the "install Fusion only once" check used style()->name(),
        // which is empty behind the stylesheet proxy — so every switch created
        // a new Fusion style and re-polished the whole widget tree.
        DarkFusionStyle::applyTheme(QStringLiteral("Dark"));
        const QStyle* before = QApplication::style();
        DarkFusionStyle::applyTheme(QStringLiteral("Light"));
        DarkFusionStyle::applyTheme(QStringLiteral("Dark"));
        QCOMPARE(QApplication::style(), before);
    }

    void systemThemeResolvesToAVariant()
    {
        // Whatever the OS reports, "System" must resolve and apply cleanly.
        const bool dark = DarkFusionStyle::resolvesToDark(QStringLiteral("System"));
        QCOMPARE(DarkFusionStyle::applyTheme(QStringLiteral("System")), dark);
        QCOMPARE(Theme::isDark(), dark);
    }
};

QTEST_MAIN(TestUiFoundation)
#include "test_ui_foundation.moc"
