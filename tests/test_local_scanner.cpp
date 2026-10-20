#include "controllers/LocalScanner.h"
#include "controllers/PlanFile.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket;

namespace {

void touch(const QString& path, const QByteArray& data = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

QList<QStringList> records(const QString& plan)
{
    QList<QStringList> out;
    QFile f(plan);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    while (!f.atEnd())
        out.append(PlanFile::decodeRecord(f.readLine()));
    return out;
}

QStringList keys(const QList<QStringList>& recs)
{
    QStringList out;
    for (const QStringList& r : recs)
        out.append(r.value(0));
    return out;
}

} // namespace

class TestLocalScanner : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    LocalScanner::Result scan(const QStringList& sources, const QString& prefix = QStringLiteral("up/"))
    {
        std::atomic<bool> cancel{false};
        std::atomic<qint64> found{0};
        return LocalScanner::scan(sources, prefix, m_dir.filePath(QStringLiteral("plan")), cancel, found);
    }

private slots:
    void walksInS3KeyOrder()
    {
        const QString root = m_dir.filePath(QStringLiteral("photos"));
        touch(root + QStringLiteral("/a-b"));
        touch(root + QStringLiteral("/a/x"));
        touch(root + QStringLiteral("/a0"));
        touch(root + QStringLiteral("/B"));
        touch(root + QStringLiteral("/ä"));
        touch(root + QStringLiteral("/a/y/z"), "12345");
        QDir().mkpath(root + QStringLiteral("/empty"));
        const LocalScanner::Result r = scan({root});
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.files, 6);
        QCOMPARE(r.folders, 1);
        QCOMPARE(r.bytes, 10);
        const QList<QStringList> recs = records(m_dir.filePath(QStringLiteral("plan")));
        const QStringList k = keys(recs);
        // Byte order: "B" < "a-b" < "a/" < "a0" < "empty/" < "ä".
        QCOMPARE(k, (QStringList{QStringLiteral("up/photos/B"), QStringLiteral("up/photos/a-b"),
                                 QStringLiteral("up/photos/a/x"), QStringLiteral("up/photos/a/y/z"),
                                 QStringLiteral("up/photos/a0"), QStringLiteral("up/photos/empty/"),
                                 QStringLiteral("up/photos/ä")}));
        QStringList sorted = k;
        std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.toUtf8() < b.toUtf8(); });
        QCOMPARE(k, sorted);
        // The fields: key, path, size, mtime, kind, source.
        const QStringList& z = recs[3];
        QCOMPARE(z.value(1), root + QStringLiteral("/a/y/z"));
        QCOMPARE(z.value(2), QStringLiteral("5"));
        QVERIFY(z.value(3).toLongLong() > 0);
        QCOMPARE(z.value(4), QStringLiteral("f"));
        QCOMPARE(recs[5].value(4), QStringLiteral("d"));
    }

    void severalSourcesSortedAndDeduplicated()
    {
        const QString a = m_dir.filePath(QStringLiteral("one/report.txt"));
        const QString b = m_dir.filePath(QStringLiteral("two/report.txt"));
        const QString c = m_dir.filePath(QStringLiteral("one/zeta"));
        touch(a);
        touch(b);
        touch(c + QStringLiteral("/inner"));
        const LocalScanner::Result r = scan({c, a, b}, QString());
        QCOMPARE(r.files, 2);
        QCOMPARE(r.skippedCount, 1);   // the second report.txt
        QVERIFY(r.skipped.first().contains(QLatin1String("report.txt")));
        QCOMPARE(keys(records(m_dir.filePath(QStringLiteral("plan")))),
                 (QStringList{QStringLiteral("report.txt"), QStringLiteral("zeta/inner")}));
    }

    void symlinkedFoldersAreNotFollowed()
    {
#ifdef Q_OS_UNIX
        const QString root = m_dir.filePath(QStringLiteral("links"));
        touch(root + QStringLiteral("/real/file"));
        touch(m_dir.filePath(QStringLiteral("target.txt")), "linked");
        QVERIFY(QFile::link(root + QStringLiteral("/real"), root + QStringLiteral("/loop")));
        QVERIFY(QFile::link(m_dir.filePath(QStringLiteral("target.txt")), root + QStringLiteral("/file-link")));
        const LocalScanner::Result r = scan({root});
        QCOMPARE(r.skippedCount, 1);
        QVERIFY(r.skipped.first().contains(QLatin1String("loop")));
        const QList<QStringList> recs = records(m_dir.filePath(QStringLiteral("plan")));
        QCOMPARE(keys(recs), (QStringList{QStringLiteral("up/links/file-link"), QStringLiteral("up/links/real/file")}));
        QCOMPARE(recs[0].value(2), QStringLiteral("6"));   // the target's size
#else
        QSKIP("symbolic links need a Unix-like system here");
#endif
    }

    void tooLongNamesAndMissingSources()
    {
        const QString deep = m_dir.filePath(QStringLiteral("long"));
        touch(deep + QStringLiteral("/ok"));
        const QString longPrefix = QString(1100, u'p') + u'/';
        const LocalScanner::Result r = scan({deep, m_dir.filePath(QStringLiteral("missing"))}, longPrefix);
        QCOMPARE(r.files, 0);
        QCOMPARE(r.skippedCount, 2);
    }

    void cancelStops()
    {
        const QString root = m_dir.filePath(QStringLiteral("many"));
        for (int i = 0; i < 50; ++i)
            touch(root + QStringLiteral("/f%1").arg(i));
        std::atomic<bool> cancel{true};
        std::atomic<qint64> found{0};
        const LocalScanner::Result r =
            LocalScanner::scan({root}, QString(), m_dir.filePath(QStringLiteral("plan")), cancel, found);
        QVERIFY(r.cancelled);
        QCOMPARE(r.files, 0);
    }
};

QTEST_GUILESS_MAIN(TestLocalScanner)
#include "test_local_scanner.moc"
