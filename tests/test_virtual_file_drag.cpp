// Windows only: the COM side of dragging out — VirtualFileDataObject's
// CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS against FakeObjectStore through
// a VirtualFileReader, the in-app formats, the asynchronous-copy protocol,
// a change while Explorer reads, and the data object created in the MTA and
// used through a proxy from this (STA) thread as DoDragDrop would.
// Explorer itself is checked by hand (devlog 010).

#include "controllers/AccountSessions.h"
#include "platform/VirtualFileReader.h"
#include "platform/win/VirtualFileDataObject.h"
#include "platform/win/VirtualFileDrag.h"
#include "support/AccountHarness.h"

#include <qbucket/s3/testing/FakeObjectStore.h>

#include <QMimeData>
#include <QScopeGuard>
#include <QtTest>

#include <atomic>
#include <functional>
#include <thread>

using namespace qbucket;
using namespace qbucket::s3::testing;
using qbucket::testing::AccountHarness;

namespace {

const QString kBucket = QStringLiteral("bkt");
const IID kIidAsyncCapability = {0x3D8B0590, 0xF691, 0x11d2, {0x8E, 0xA9, 0x00, 0x60, 0x97, 0xDF, 0x5B, 0xD4}};

QByteArray pattern(int size)
{
    QByteArray out(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        out[i] = char((i * 31 + 3) & 0xFF);
    return out;
}

FORMATETC formatOf(UINT cf, DWORD tymed, LONG index = -1)
{
    FORMATETC f{};
    f.cfFormat = CLIPFORMAT(cf);
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = index;
    f.tymed = tymed;
    return f;
}

QByteArray bytesOf(const STGMEDIUM& m)
{
    const void* p = GlobalLock(m.hGlobal);
    const QByteArray out(static_cast<const char*>(p), int(GlobalSize(m.hGlobal)));
    GlobalUnlock(m.hGlobal);
    return out;
}

struct Listed {
    QString name;
    bool folder = false;
    qint64 size = 0;
};

QList<Listed> parseDescriptor(const STGMEDIUM& m)
{
    QList<Listed> out;
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(m.hGlobal));
    for (UINT i = 0; i < group->cItems; ++i) {
        const FILEDESCRIPTORW& d = group->fgd[i];
        out.append({QString::fromWCharArray(d.cFileName), (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    (qint64(d.nFileSizeHigh) << 32) | d.nFileSizeLow});
    }
    GlobalUnlock(m.hGlobal);
    return out;
}

QByteArray readStream(IStream* s, HRESULT* last)
{
    QByteArray out;
    char buffer[700];
    for (;;) {
        ULONG got = 0;
        *last = s->Read(buffer, sizeof(buffer), &got);
        out.append(buffer, int(got));
        if (FAILED(*last) || got == 0)
            return out;
    }
}

} // namespace

class TestVirtualFileDrag : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<AccountHarness> m_h;
    std::unique_ptr<FakeObjectStore> m_objects;

    s3::Session* session()
    {
        const s3::Result<s3::Session*> s = m_h->sessions->session(m_h->account.id);
        return s ? *s : nullptr;
    }

    // Explorer's calls come on a COM thread of their own: so do these (this
    // thread serves the fake server meanwhile).
    static bool onMta(const std::function<void()>& work, int timeoutMs = 20000)
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            work();
            CoUninitialize();
            done = true;
        });
        const bool ok = QTest::qWaitFor([&] { return done.load(); }, timeoutMs);
        t.join();
        return ok;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    }

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
        m_objects->putObject(kBucket, QStringLiteral("docs/a.txt"), "aaa");
        m_objects->putObject(kBucket, QStringLiteral("photos/x.jpg"), pattern(5000));
    }

    void cleanup()
    {
        m_objects.reset();
        m_h.reset();
    }

    void descriptorThenStreamedContents()
    {
        auto reader = std::make_shared<VirtualFileReader>(
            session(), DragStaging::Selection{m_h->account.id, kBucket,
                                              {{QStringLiteral("docs/a.txt"), false, 3}, {QStringLiteral("photos/"), true, -1}}});
        reader->setChunkSizeForTesting(1000);
        QList<Listed> listed;
        QByteArray photo;
        HRESULT readResult = E_FAIL;
        ULONGLONG statSize = 0;
        QVERIFY(onMta([&] {
            auto* object = new VirtualFileDataObject(reader, {}, nullptr);
            FORMATETC fd = formatOf(object->descriptorFormat(), TYMED_HGLOBAL);
            STGMEDIUM m{};
            if (SUCCEEDED(object->GetData(&fd, &m))) {
                listed = parseDescriptor(m);
                ReleaseStgMedium(&m);
            }
            for (int i = 0; i < listed.size(); ++i) {
                if (listed.at(i).name != QLatin1String("photos\\x.jpg"))
                    continue;
                FORMATETC fc = formatOf(object->contentsFormat(), TYMED_ISTREAM, LONG(i));
                STGMEDIUM c{};
                if (SUCCEEDED(object->GetData(&fc, &c))) {
                    STATSTG st{};
                    c.pstm->Stat(&st, STATFLAG_NONAME);
                    statSize = st.cbSize.QuadPart;
                    photo = readStream(c.pstm, &readResult);
                    ReleaseStgMedium(&c);
                }
            }
            object->Release();
        }));
        QCOMPARE(listed.size(), 3);
        QCOMPARE(listed.at(0).name, QStringLiteral("photos"));   // folders first
        QVERIFY(listed.at(0).folder);
        QCOMPARE(listed.at(1).name, QStringLiteral("a.txt"));
        QCOMPARE(listed.at(1).size, 3);
        QCOMPARE(listed.at(2).name, QStringLiteral("photos\\x.jpg"));
        QCOMPARE(statSize, ULONGLONG(5000));
        QCOMPARE(photo, pattern(5000));
        QVERIFY(SUCCEEDED(readResult));
        QVERIFY(reader->summary().contains(QStringLiteral("photos/x.jpg: md5 verified")));
    }

    void inAppFormatsAndTheAsynchronousCopy()
    {
        auto reader = std::make_shared<VirtualFileReader>(
            session(), DragStaging::Selection{m_h->account.id, kBucket, {{QStringLiteral("docs/a.txt"), false, 3}}});
        const UINT objects = RegisterClipboardFormatW(L"application/x-qbucket-objects");
        int finished = 0;
        HRESULT finishedWith = E_FAIL;
        auto* object = new VirtualFileDataObject(reader, {{objects, std::string("{\"v\":1}")}},
                                                 [&](HRESULT r, DWORD) {
                                                     ++finished;
                                                     finishedWith = r;
                                                 });
        FORMATETC f = formatOf(objects, TYMED_HGLOBAL);
        QCOMPARE(object->QueryGetData(&f), S_OK);
        STGMEDIUM m{};
        QCOMPARE(object->GetData(&f, &m), S_OK);
        QVERIFY(bytesOf(m).startsWith("{\"v\":1}"));
        ReleaseStgMedium(&m);
        FORMATETC effect = formatOf(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), TYMED_HGLOBAL);
        QCOMPARE(object->GetData(&effect, &m), S_OK);
        QCOMPARE(*reinterpret_cast<const DWORD*>(bytesOf(m).constData()), DWORD(DROPEFFECT_COPY));
        ReleaseStgMedium(&m);
        IEnumFORMATETC* formats = nullptr;
        QCOMPARE(object->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
        FORMATETC listed[8];
        ULONG count = 0;
        formats->Next(8, listed, &count);
        formats->Release();
        QCOMPARE(count, ULONG(4));

        // What the shell sets comes back.
        const UINT performed = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
        FORMATETC pf = formatOf(performed, TYMED_HGLOBAL);
        STGMEDIUM set{};
        set.tymed = TYMED_HGLOBAL;
        set.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        *static_cast<DWORD*>(GlobalLock(set.hGlobal)) = DROPEFFECT_COPY;
        GlobalUnlock(set.hGlobal);
        QCOMPARE(object->SetData(&pf, &set, TRUE), S_OK);
        QCOMPARE(object->QueryGetData(&pf), S_OK);

        // The asynchronous copy: Explorer starts it, ends it, and that ends the drag.
        IDataObjectAsyncCapability* async = nullptr;
        QCOMPARE(object->QueryInterface(kIidAsyncCapability, reinterpret_cast<void**>(&async)), S_OK);
        BOOL on = FALSE;
        async->GetAsyncMode(&on);
        QVERIFY(on);
        async->StartOperation(nullptr);
        async->InOperation(&on);
        QVERIFY(on);
        async->EndOperation(S_OK, nullptr, DROPEFFECT_COPY);
        QCOMPARE(finished, 1);
        QCOMPARE(finishedWith, S_OK);
        async->Release();
        object->Release();
        QCOMPARE(finished, 1);   // once
    }

    void aChangeWhileExplorerReadsFailsTheRead()
    {
        auto reader = std::make_shared<VirtualFileReader>(
            session(), DragStaging::Selection{m_h->account.id, kBucket, {{QStringLiteral("photos/x.jpg"), false, 5000}}});
        reader->setChunkSizeForTesting(500);
        IStream* stream = nullptr;
        auto* object = new VirtualFileDataObject(reader, {}, nullptr);
        QVERIFY(onMta([&] {
            FORMATETC fd = formatOf(object->descriptorFormat(), TYMED_HGLOBAL);
            STGMEDIUM m{};
            object->GetData(&fd, &m);
            ReleaseStgMedium(&m);
            FORMATETC fc = formatOf(object->contentsFormat(), TYMED_ISTREAM, 0);
            STGMEDIUM c{};
            if (SUCCEEDED(object->GetData(&fc, &c)))
                stream = c.pstm;
            char head[100];
            ULONG got = 0;
            stream->Read(head, sizeof(head), &got);
        }));
        QVERIFY(stream);
        m_objects->putObject(kBucket, QStringLiteral("photos/x.jpg"), pattern(5000).replace(4000, 1, "!"));
        HRESULT last = S_OK;
        QVERIFY(onMta([&] { (void)readStream(stream, &last); }));
        QCOMPARE(last, STG_E_READFAULT);
        stream->Release();
        object->Release();
    }

    void createdInTheMtaUsedThroughAProxy()
    {
        // What VirtualFileDrag holds for the app's lifetime: without it the
        // MTA ends with the thread that created the object, and the proxy is
        // disconnected (RPC_E_DISCONNECTED).
        CO_MTA_USAGE_COOKIE mta = nullptr;
        QVERIFY(SUCCEEDED(CoIncrementMTAUsage(&mta)));
        const auto releaseMta = qScopeGuard([mta] { CoDecrementMTAUsage(mta); });
        auto reader = std::make_shared<VirtualFileReader>(
            session(), DragStaging::Selection{m_h->account.id, kBucket, {{QStringLiteral("docs/a.txt"), false, 3}}});
        QMimeData internal;
        internal.setData(QStringLiteral("application/x-qbucket-objects"), "{}");
        std::atomic<int> finished{0};
        IDataObject* proxy = VirtualFileDrag::createDataObject(reader, &internal, [&](HRESULT, DWORD) { ++finished; });
        QVERIFY(proxy);
        FORMATETC f = formatOf(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW), TYMED_HGLOBAL);
        QCOMPARE(proxy->QueryGetData(&f), S_OK);   // answered in the MTA
        FORMATETC objects = formatOf(RegisterClipboardFormatW(L"application/x-qbucket-objects"), TYMED_HGLOBAL);
        QCOMPARE(proxy->QueryGetData(&objects), S_OK);
        proxy->Release();
        QTRY_COMPARE(finished.load(), 1);   // the drag is over once the object goes
    }
};

QTEST_GUILESS_MAIN(TestVirtualFileDrag)
#include "test_virtual_file_drag.moc"
