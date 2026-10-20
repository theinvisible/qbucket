#include "controllers/LocalPathMapper.h"

#include <QtTest>

using namespace qbucket;
using Rules = LocalPathMapper::Rules;

class TestLocalPathMapper : public QObject {
    Q_OBJECT

private slots:
    void plainKeysStayAsTheyAre()
    {
        for (const Rules rules : {Rules::Posix, Rules::Windows}) {
            const auto r = LocalPathMapper::map(QStringLiteral("photos/2026/ä b+c.jpg"), rules);
            QVERIFY(r.ok);
            QVERIFY(!r.changed);
            QCOMPARE(r.path, QStringLiteral("photos/2026/ä b+c.jpg"));
            const auto marker = LocalPathMapper::map(QStringLiteral("photos/empty/"), rules);
            QCOMPARE(marker.path, QStringLiteral("photos/empty/"));
        }
    }

    void neverLeavesTheTarget()
    {
        for (const Rules rules : {Rules::Posix, Rules::Windows}) {
            for (const QString& k : {QStringLiteral("../etc/passwd"), QStringLiteral("a/../../x"),
                                     QStringLiteral("a/./b"), QStringLiteral(".."), QStringLiteral("./")}) {
                const auto r = LocalPathMapper::map(k, rules);
                QVERIFY2(!r.ok, qPrintable(k));
                QVERIFY(!r.reason.isEmpty());
            }
        }
        // Absolute-looking keys stay relative.
        const auto abs = LocalPathMapper::map(QStringLiteral("/etc/passwd"), Rules::Posix);
        QVERIFY(abs.ok);
        QCOMPARE(abs.path, QStringLiteral("_/etc/passwd"));
        const auto empty = LocalPathMapper::map(QStringLiteral("a//b"), Rules::Posix);
        QCOMPARE(empty.path, QStringLiteral("a/_/b"));
        QVERIFY(empty.changed);
        // A drive letter is just a name with ":" on Windows.
        const auto drive = LocalPathMapper::map(QStringLiteral("C:/Windows/x"), Rules::Windows);
        QCOMPARE(drive.path, QStringLiteral("C_/Windows/x"));
    }

    void windowsNames()
    {
        const auto chars = LocalPathMapper::map(QStringLiteral("q?a*b|c<d>e\"f\\g:h\tz"), Rules::Windows);
        QVERIFY(chars.ok);
        QCOMPARE(chars.path, QStringLiteral("q_a_b_c_d_e_f_g_h_z"));
        QVERIFY(chars.changed);
        QCOMPARE(LocalPathMapper::map(QStringLiteral("dir./file. "), Rules::Windows).path, QStringLiteral("dir_/file._"));
        QCOMPARE(LocalPathMapper::map(QStringLiteral("con"), Rules::Windows).path, QStringLiteral("_con"));
        QCOMPARE(LocalPathMapper::map(QStringLiteral("x/NUL.txt"), Rules::Windows).path, QStringLiteral("x/_NUL.txt"));
        QCOMPARE(LocalPathMapper::map(QStringLiteral("COM7"), Rules::Windows).path, QStringLiteral("_COM7"));
        QCOMPARE(LocalPathMapper::map(QStringLiteral("COM0"), Rules::Windows).path, QStringLiteral("COM0"));
        QCOMPARE(LocalPathMapper::map(QStringLiteral("console"), Rules::Windows).path, QStringLiteral("console"));
        // POSIX keeps them.
        QCOMPARE(LocalPathMapper::map(QStringLiteral("q?a\\b:c"), Rules::Posix).path, QStringLiteral("q?a\\b:c"));
        QVERIFY(!LocalPathMapper::map(QStringLiteral("con"), Rules::Posix).changed);
    }

    void tooLongSegmentsAreRefused()
    {
        const QString segment(256, u'x');
        QVERIFY(!LocalPathMapper::map(segment).ok);
        QVERIFY(LocalPathMapper::map(QString(255, u'x')).ok);
        QVERIFY(!LocalPathMapper::map(QString(128, u'ä')).ok);   // 256 UTF-8 bytes
    }

    void collisionsCompareLikeTheFileSystem()
    {
        QCOMPARE(LocalPathMapper::normalized(QStringLiteral("A.txt"), Rules::Windows),
                 LocalPathMapper::normalized(QStringLiteral("a.TXT"), Rules::Windows));
        QVERIFY(LocalPathMapper::normalized(QStringLiteral("A.txt"), Rules::Posix)
                != LocalPathMapper::normalized(QStringLiteral("a.txt"), Rules::Posix));
    }
};

QTEST_GUILESS_MAIN(TestLocalPathMapper)
#include "test_local_path_mapper.moc"
