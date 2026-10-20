#include <QCoreApplication>
#include <QTranslator>
#include <QtTest>

// The compiled translations: English plural forms ("1 object", never
// "1 object(s)") and the German file.
class TestTranslations : public QObject {
    Q_OBJECT

private slots:
    void englishPlurals()
    {
        QTranslator en;
        QVERIFY(en.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")));
        QCOMPARE(en.translate("qbucket::ObjectBrowser", "%n object(s)", nullptr, 1), QStringLiteral("%n object"));
        QCOMPARE(en.translate("qbucket::ObjectBrowser", "%n object(s)", nullptr, 3), QStringLiteral("%n objects"));
        QCOMPARE(en.translate("qbucket::DeleteJob", "%n selected object(s) no longer exist and are skipped.", nullptr, 1),
                 QStringLiteral("%n selected object no longer exists and is skipped."));
    }

    void installedTheyReplaceTheSourceText()
    {
        QTranslator en;
        QVERIFY(en.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_en.qm")));
        QCoreApplication::installTranslator(&en);
        QCOMPARE(QCoreApplication::translate("qbucket::ObjectBrowser", "%n object(s)", nullptr, 1),
                 QStringLiteral("1 object"));
        QTranslator de;
        QVERIFY(de.load(QStringLiteral(QBUCKET_QM_DIR "/qbucket_de.qm")));
        QCoreApplication::installTranslator(&de);   // later wins
        QCOMPARE(QCoreApplication::translate("qbucket::ObjectBrowser", "%n object(s)", nullptr, 2),
                 QStringLiteral("2 Objekte"));
        QCoreApplication::removeTranslator(&de);
        QCoreApplication::removeTranslator(&en);
    }
};

QTEST_GUILESS_MAIN(TestTranslations)
#include "test_translations.moc"
