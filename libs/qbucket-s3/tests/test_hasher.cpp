#include <qbucket/s3/transfer/Hasher.h>

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <optional>

using namespace qbucket::s3;
using namespace qbucket::s3::transfer;

class TestHasher : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QByteArray m_data;
    QString m_path;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_data = QByteArray("0123456789abcdef").repeated(300000);   // 4.8 MB, several chunks
        m_path = m_dir.filePath(QStringLiteral("data.bin"));
        QFile f(m_path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(m_data), m_data.size());
    }

    void hashesARange()
    {
        const auto r = hashRangeNow(m_path, 1000, 2'000'000, true);
        QVERIFY(r);
        QCOMPARE(r->bytes, 2'000'000);
        QCOMPARE(r->md5, QCryptographicHash::hash(m_data.mid(1000, 2'000'000), QCryptographicHash::Md5));
        QCOMPARE(r->sha256, QCryptographicHash::hash(m_data.mid(1000, 2'000'000), QCryptographicHash::Sha256));
        const auto noSha = hashRangeNow(m_path, 0, 10, false);
        QVERIFY(noSha);
        QVERIFY(noSha->sha256.isEmpty());
        const auto empty = hashRangeNow(m_path, 5, 0, false);
        QVERIFY(empty);
        QCOMPARE(empty->md5, QCryptographicHash::hash(QByteArray(), QCryptographicHash::Md5));
    }

    void shortOrMissingFilesAreLocalErrors()
    {
        const auto beyond = hashRangeNow(m_path, m_data.size() - 10, 20, false);
        QVERIFY(!beyond);
        QCOMPARE(beyond.error().category, ErrorCategory::LocalIo);
        const auto missing = hashRangeNow(m_dir.filePath(QStringLiteral("nope")), 0, 1, false);
        QVERIFY(!missing);
        QCOMPARE(missing.error().category, ErrorCategory::LocalIo);
    }

    void asyncOnTheContextThread()
    {
        QObject context;
        std::optional<Result<RangeHash>> out;
        bool rightThread = false;
        hashRange(m_path, 0, m_data.size(), false, &context, [&](Result<RangeHash> r) {
            rightThread = QThread::currentThread() == thread();
            out.emplace(std::move(r));
        });
        QVERIFY(!out.has_value());   // never synchronous
        QTRY_VERIFY(out.has_value());
        QVERIFY(rightThread);
        QVERIFY(*out);
        QCOMPARE((*out)->md5, QCryptographicHash::hash(m_data, QCryptographicHash::Md5));
    }

    void cancelledAndDeadContexts()
    {
        QThreadPool pool;
        pool.setMaxThreadCount(1);
        QObject context;
        std::optional<Result<RangeHash>> out;
        const CancelFlag flag = hashRange(m_path, 0, m_data.size(), false, &context,
                                          [&](Result<RangeHash> r) { out.emplace(std::move(r)); }, &pool);
        flag->store(true);
        QTRY_VERIFY(out.has_value());
        QVERIFY(!*out);
        QCOMPARE(out->error().category, ErrorCategory::Cancelled);

        int calls = 0;
        {
            QObject gone;
            hashRange(m_path, 0, m_data.size(), false, &gone, [&](const Result<RangeHash>&) { ++calls; }, &pool);
        }
        pool.waitForDone();
        QTest::qWait(50);
        QCOMPARE(calls, 0);
    }

    void etags()
    {
        const QByteArray a = QCryptographicHash::hash("a", QCryptographicHash::Md5);
        const QByteArray b = QCryptographicHash::hash("b", QCryptographicHash::Md5);
        QCOMPARE(md5Etag(a), QStringLiteral("\"0cc175b9c0f1b6a831c399e269772661\""));
        const QString composite = compositeEtag({a, b});
        QCOMPARE(composite, QLatin1Char('"') + QString::fromLatin1(QCryptographicHash::hash(a + b, QCryptographicHash::Md5).toHex())
                                + QStringLiteral("-2\""));
        QCOMPARE(etagPartCount(composite), 2);
        QCOMPARE(etagPartCount(md5Etag(a)), 0);
        QCOMPARE(etagPartCount(QStringLiteral("\"abc-x\"")), -1);
        QVERIFY(sameEtag(QStringLiteral("\"ABC\""), QStringLiteral("abc")));
        QVERIFY(!sameEtag(QStringLiteral("\"\""), QStringLiteral("")));
        QVERIFY(!sameEtag(QStringLiteral("a"), QStringLiteral("b")));
    }
};

QTEST_GUILESS_MAIN(TestHasher)
#include "test_hasher.moc"
