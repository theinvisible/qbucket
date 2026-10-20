#include <qbucket/s3/transfer/JsonFileResumeStore.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace qbucket::s3;
using namespace qbucket::s3::transfer;

namespace {

TransferRecord sampleRecord()
{
    TransferRecord r;
    r.id = QStringLiteral("t-0001");
    r.direction = Direction::Upload;
    r.account = QStringLiteral("{8c2f1f3e-0000-4000-8000-000000000001}");
    r.bucket = QStringLiteral("bkt");
    r.key = QStringLiteral("dir/ä file.bin");
    r.uploadId = QStringLiteral("upload-123");
    r.localPath = QStringLiteral("/home/user/ä file.bin");
    r.size = Q_INT64_C(53687091200);   // 50 GiB
    r.localModified = QDateTime(QDate(2026, 10, 20), QTime(12, 30, 1, 250), QTimeZone::UTC);
    r.partSize = Q_INT64_C(8) * 1024 * 1024;
    r.partCount = 6400;
    r.extra.insert(QStringLiteral("job"), QStringLiteral("abc"));
    return r;
}

PartRecord part(int n)
{
    PartRecord p;
    p.partNumber = n;
    p.offset = qint64(n - 1) * 8 * 1024 * 1024;
    p.length = Q_INT64_C(8) * 1024 * 1024;
    p.md5 = QCryptographicHash::hash(QByteArray::number(n), QCryptographicHash::Md5);
    p.etag = QLatin1Char('"') + QString::fromLatin1(p.md5.toHex()) + QLatin1Char('"');
    return p;
}

} // namespace

class TestResumeStore : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString storeDir() const { return m_dir->filePath(QStringLiteral("transfers/nested")); }

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
    }

    void roundTrip()
    {
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.ids().isEmpty());
        QVERIFY(store.begin(sampleRecord()));
        QVERIFY(store.addPart(QStringLiteral("t-0001"), part(2)));
        QVERIFY(store.addPart(QStringLiteral("t-0001"), part(1)));

        // A second instance reads what the first wrote (a restart).
        JsonFileResumeStore again(storeDir());
        QCOMPARE(again.ids(), QStringList{QStringLiteral("t-0001")});
        const auto state = again.load(QStringLiteral("t-0001"));
        QVERIFY(state);
        const TransferRecord& r = state->record;
        QCOMPARE(r.key, sampleRecord().key);
        QCOMPARE(r.size, sampleRecord().size);
        QCOMPARE(r.localModified, sampleRecord().localModified);
        QCOMPARE(r.partCount, 6400);
        QCOMPARE(r.uploadId, QStringLiteral("upload-123"));
        QCOMPARE(r.extra.value(QStringLiteral("job")).toString(), QStringLiteral("abc"));
        QCOMPARE(state->parts.size(), 2);
        QCOMPARE(state->parts[0].partNumber, 1);   // sorted
        QCOMPARE(state->parts[1].md5, part(2).md5);
        QCOMPARE(state->parts[1].offset, part(2).offset);
    }

    void tornLastLineIsIgnored()
    {
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.begin(sampleRecord()));
        QVERIFY(store.addPart(QStringLiteral("t-0001"), part(1)));
        QFile f(QDir(storeDir()).filePath(QStringLiteral("t-0001.parts")));
        QVERIFY(f.open(QIODevice::Append));
        f.write("{\"n\":2,\"offset\":\"83886");   // the process died here
        f.close();
        const auto state = store.load(QStringLiteral("t-0001"));
        QVERIFY(state);
        QCOMPARE(state->parts.size(), 1);
        // The next append after the restart starts on a fresh line: the torn
        // one stays unreadable, the new record counts.
        QVERIFY(store.addPart(QStringLiteral("t-0001"), part(3)));
        const auto after = store.load(QStringLiteral("t-0001"));
        QVERIFY(after);
        QCOMPARE(after->parts.size(), 2);
        QCOMPARE(after->parts[1].partNumber, 3);
    }

    void laterRecordOfAPartWins()
    {
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.begin(sampleRecord()));
        PartRecord p = part(1);
        QVERIFY(store.addPart(QStringLiteral("t-0001"), p));
        p.etag = QStringLiteral("\"second\"");
        QVERIFY(store.addPart(QStringLiteral("t-0001"), p));
        const auto state = store.load(QStringLiteral("t-0001"));
        QCOMPARE(state->parts.size(), 1);
        QCOMPARE(state->parts[0].etag, QStringLiteral("\"second\""));
    }

    void beginDropsOldPartsUpdateKeepsThem()
    {
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.begin(sampleRecord()));
        QVERIFY(store.addPart(QStringLiteral("t-0001"), part(1)));
        TransferRecord r = sampleRecord();
        r.uploadId = QStringLiteral("upload-456");
        QVERIFY(store.update(r));
        QCOMPARE(store.load(QStringLiteral("t-0001"))->parts.size(), 1);
        QCOMPARE(store.load(QStringLiteral("t-0001"))->record.uploadId, QStringLiteral("upload-456"));
        QVERIFY(store.begin(r));
        QVERIFY(store.load(QStringLiteral("t-0001"))->parts.isEmpty());
    }

    void removeAndMissing()
    {
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.begin(sampleRecord()));
        QVERIFY(store.remove(QStringLiteral("t-0001")));
        QVERIFY(store.ids().isEmpty());
        const auto gone = store.load(QStringLiteral("t-0001"));
        QVERIFY(!gone);
        QCOMPARE(gone.error().category, ErrorCategory::NotFound);
        QVERIFY(!store.addPart(QStringLiteral("t-0001"), part(1)));
        QVERIFY(!store.update(sampleRecord()));
        QVERIFY(store.remove(QStringLiteral("never-was")));
    }

    void idsAreFileNameSafe()
    {
        JsonFileResumeStore store(storeDir());
        TransferRecord r = sampleRecord();
        for (const QString& bad : {QStringLiteral("../escape"), QStringLiteral(""), QStringLiteral("a/b"),
                                   QStringLiteral("x.json")}) {
            r.id = bad;
            QVERIFY(!store.begin(r));
            QVERIFY(!store.load(bad));
        }
        QVERIFY(!QFile::exists(m_dir->filePath(QStringLiteral("escape.json"))));
    }

    void neverHoldsSecrets()
    {
        // The record has no field for one; the file holds what we gave it.
        JsonFileResumeStore store(storeDir());
        QVERIFY(store.begin(sampleRecord()));
        QFile f(QDir(storeDir()).filePath(QStringLiteral("t-0001.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll().toLower();
        QVERIFY(!text.contains("secret"));
        QVERIFY(!text.contains("password"));
    }
};

QTEST_GUILESS_MAIN(TestResumeStore)
#include "test_resume_store.moc"
