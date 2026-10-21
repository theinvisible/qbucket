// VirtualFileReader (the data behind a virtual-file drag) against
// FakeObjectStore, called from a thread of its own as Explorer's would:
// folders listed into Windows-safe names with their folders first, bytes in
// chunks with read-ahead, the MD5 checked against the ETag at the end, an
// object changed while it is read is an error, cancel wakes a waiting call.

#include "controllers/AccountSessions.h"
#include "platform/VirtualFileReader.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QtTest>

#include <atomic>
#include <functional>
#include <thread>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");

QByteArray pattern(int size)
{
    QByteArray out(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        out[i] = char((i * 131 + 7) & 0xFF);
    return out;
}

QString path(const VirtualFileSource::File& f)
{
    return QString::fromUtf16(f.path.data(), qsizetype(f.path.size()));
}

} // namespace

class TestVirtualFileReader : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;

    s3::Session* session()
    {
        const s3::Result<s3::Session*> s = m_h->sessions->session(m_h->account.id);
        return s ? *s : nullptr;
    }

    // Runs `work` on a thread of its own while this one serves the fake server.
    static bool offThread(const std::function<void()>& work, int timeoutMs = 20000)
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            work();
            done = true;
        });
        const bool ok = QTest::qWaitFor([&] { return done.load(); }, timeoutMs);
        t.join();
        return ok;
    }

    static VirtualFileSource::Status readAll(VirtualFileSource& source, std::size_t index, QByteArray* out,
                                             std::uint32_t step = 1000)
    {
        out->clear();
        QByteArray buffer(int(step), Qt::Uninitialized);
        for (;;) {
            std::uint32_t got = 0;
            const VirtualFileSource::Status s = source.read(index, out->size(), buffer.data(), step, &got);
            if (s != VirtualFileSource::Status::Ok)
                return s;
            if (got == 0)
                return s;
            out->append(buffer.constData(), int(got));
        }
    }

private slots:
    void init()
    {
        m_h = std::make_unique<AccountHarness>();
        QVERIFY(m_h->ok());
        Account a = m_h->account;
        a.addressing = s3::AddressingStyle::Path;
        QVERIFY(m_h->accounts->save(a));
        m_h->account = a;
        m_objects = std::make_unique<FakeObjectStore>(m_h->server.get());
        m_objects->createBucket(kBucket);
    }

    void cleanup()
    {
        m_objects.reset();
        m_h.reset();
    }

    void listsFoldersFirstWithWindowsNames()
    {
        m_objects->putObject(kBucket, QStringLiteral("docs/a.txt"), "aaa");
        m_objects->putObject(kBucket, QStringLiteral("photos/2026/b<1>.jpg"), "bb");
        m_objects->putObject(kBucket, QStringLiteral("photos/empty/"), QByteArray());
        m_objects->putObject(kBucket, QStringLiteral("photos/c.jpg"), "c");
        DragStaging::Selection sel{m_h->account.id, kBucket,
                                   {{QStringLiteral("docs/a.txt"), false, 3}, {QStringLiteral("photos/"), true, -1}}};
        VirtualFileReader reader(session(), sel);
        std::vector<VirtualFileSource::File> files;
        VirtualFileSource::Status status = VirtualFileSource::Status::Failed;
        QVERIFY(offThread([&] { status = reader.files(&files); }));
        QCOMPARE(status, VirtualFileSource::Status::Ok);
        QStringList paths;
        for (const auto& f : files)
            paths.append(path(f) + (f.folder ? QStringLiteral("\\") : QString()));
        QCOMPARE(paths, (QStringList{QStringLiteral("photos\\"), QStringLiteral("photos\\2026\\"),
                                     QStringLiteral("photos\\empty\\"), QStringLiteral("a.txt"),
                                     QStringLiteral("photos\\2026\\b_1_.jpg"), QStringLiteral("photos\\c.jpg")}));
        QCOMPARE(files.at(3).size, 3);
        QVERIFY(files.at(3).modifiedMsecs > 0);
    }

    void readsInChunksAndVerifiesTheMd5()
    {
        const QByteArray data = pattern(10000);
        m_objects->putObject(kBucket, QStringLiteral("big.bin"), data);
        VirtualFileReader reader(session(), {m_h->account.id, kBucket, {{QStringLiteral("big.bin"), false, 10000}}});
        reader.setChunkSizeForTesting(3000);
        QByteArray got;
        VirtualFileSource::Status status = VirtualFileSource::Status::Failed;
        QVERIFY(offThread([&] {
            std::vector<VirtualFileSource::File> files;
            if (reader.files(&files) == VirtualFileSource::Status::Ok)
                status = readAll(reader, 0, &got, 777);
        }));
        QCOMPARE(status, VirtualFileSource::Status::Ok);
        QCOMPARE(got, data);
        QCOMPARE(reader.summary(), QStringList{QStringLiteral("big.bin: md5 verified")});
        // Ranged GETs with If-Match, never the whole object at once.
        int ranged = 0;
        for (const RecordedRequest& r : m_h->server->requests()) {
            if (r.method == "GET" && r.path.endsWith("big.bin")) {
                QVERIFY(!r.header("range").isEmpty());
                QVERIFY(!r.header("if-match").isEmpty());
                ++ranged;
            }
        }
        QCOMPARE(ranged, 4);
    }

    void aChangeWhileReadingIsAnError()
    {
        m_objects->putObject(kBucket, QStringLiteral("doc.bin"), pattern(9000));
        VirtualFileReader reader(session(), {m_h->account.id, kBucket, {{QStringLiteral("doc.bin"), false, 9000}}});
        reader.setChunkSizeForTesting(1000);
        VirtualFileSource::Status first = VirtualFileSource::Status::Failed;
        QVERIFY(offThread([&] {
            std::vector<VirtualFileSource::File> files;
            reader.files(&files);
            QByteArray buffer(500, Qt::Uninitialized);
            std::uint32_t got = 0;
            first = reader.read(0, 0, buffer.data(), 500, &got);
        }));
        QCOMPARE(first, VirtualFileSource::Status::Ok);
        m_objects->putObject(kBucket, QStringLiteral("doc.bin"), pattern(9000).replace(0, 1, "!"));
        QByteArray got;
        VirtualFileSource::Status status = VirtualFileSource::Status::Ok;
        QVERIFY(offThread([&] { status = readAll(reader, 0, &got); }));
        QCOMPARE(status, VirtualFileSource::Status::Failed);
        QVERIFY(QString::fromStdU16String(reader.lastError()).contains(QLatin1String("changed")));
    }

    void cancelWakesAWaitingCall()
    {
        m_objects->putObject(kBucket, QStringLiteral("slow.bin"), pattern(100));
        m_objects->setDelay(QStringLiteral("HeadObject"), 3000);
        VirtualFileReader reader(session(), {m_h->account.id, kBucket, {{QStringLiteral("slow.bin"), false, 100}}});
        VirtualFileSource::Status status = VirtualFileSource::Status::Ok;
        std::atomic<bool> done{false};
        std::thread t([&] {
            std::vector<VirtualFileSource::File> files;
            status = reader.files(&files);
            done = true;
        });
        QTest::qWait(200);
        reader.cancel();
        QVERIFY(QTest::qWaitFor([&] { return done.load(); }, 2000));
        t.join();
        QCOMPARE(status, VirtualFileSource::Status::Cancelled);
    }

    void neverBlocksTheGuiThread()
    {
        m_objects->putObject(kBucket, QStringLiteral("a"), "a");
        VirtualFileReader reader(session(), {m_h->account.id, kBucket, {{QStringLiteral("a"), false, 1}}});
        std::vector<VirtualFileSource::File> files;
        QCOMPARE(reader.files(&files), VirtualFileSource::Status::Failed);   // refused, not a deadlock
    }
};

QTEST_GUILESS_MAIN(TestVirtualFileReader)
#include "test_virtual_file_reader.moc"
