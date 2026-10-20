// Transfer benchmark (PLAN.md §13.3/§13.4) — not part of ctest. Run a
// release build; the files go to a temporary folder (default: the system's
// temp location, override with --dir):
//
//   build-release/bench/bench_transfer [--files 1000000] [--upload-mib 2048] [--dir /path]
//
// A FakeS3Server with a FakeObjectStore (discard mode: bodies are hashed,
// not kept) runs on its own thread; the GUI thread runs the real jobs. A
// 1 ms probe timer measures the longest GUI-thread stall of each phase:
//   1. planning a folder upload of --files files (scan + target listing),
//   2. one large upload (throughput),
//   3. four uploads while the browser lists 100 000 objects.

#include "accounts/AccountStore.h"
#include "accounts/CapabilityStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ActivityLog.h"
#include "controllers/BrowseController.h"
#include "controllers/CapabilityController.h"
#include "controllers/Journal.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"
#include "controllers/UploadJob.h"
#include "models/ObjectTableModel.h"
#include "support/FakeKeychainBackend.h"
#include "ui/ObjectBrowser.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeObjectStore.h>
#include <qbucket/s3/testing/FakeS3Server.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

#if defined(Q_OS_LINUX)
#include <fstream>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

using namespace qbucket;
using namespace qbucket::s3::testing;

namespace {

qint64 residentBytes()
{
#if defined(Q_OS_LINUX)
    std::ifstream statm("/proc/self/statm");
    long pages = 0;
    long resident = 0;
    statm >> pages >> resident;
    return qint64(resident) * sysconf(_SC_PAGESIZE);
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return qint64(pmc.WorkingSetSize);
#else
    return -1;
#endif
}

class StallProbe : public QObject {
public:
    StallProbe()
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 gap = m_clock.restart();
            m_longest = std::max(m_longest, gap - 1);
        });
    }
    void start()
    {
        m_longest = 0;
        m_clock.start();
        m_timer.start();
    }
    qint64 stop()
    {
        m_timer.stop();
        return std::max<qint64>(m_longest, m_clock.elapsed() - 1);
    }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_longest = 0;
};

// The store on its own thread: hashing request bodies is not GUI work.
class ServerThread {
public:
    explicit ServerThread(int browseObjects)
    {
        m_anchor.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(&m_anchor, [this, browseObjects] {
            m_server = std::make_unique<FakeS3Server>();
            m_server->setRecordBodies(false);
            m_store = std::make_unique<FakeObjectStore>(m_server.get());
            m_store->setDiscardData(true);
            m_store->createBucket(QStringLiteral("bkt"));
            for (int i = 0; i < browseObjects; ++i) {
                m_store->putSyntheticObject(QStringLiteral("bkt"),
                                            QStringLiteral("many/obj-%1.bin").arg(i, 8, 10, QLatin1Char('0')),
                                            (qint64(i) * 7919) % 100'000'000);
            }
            m_server->listen();
            m_url = m_server->endpointUrl();
        }, Qt::BlockingQueuedConnection);
    }
    ~ServerThread()
    {
        QMetaObject::invokeMethod(&m_anchor, [this] {
            m_store.reset();
            m_server.reset();
        }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    [[nodiscard]] QUrl url() const { return m_url; }

private:
    QThread m_thread;
    QObject m_anchor;
    std::unique_ptr<FakeS3Server> m_server;
    std::unique_ptr<FakeObjectStore> m_store;
    QUrl m_url;
};

struct Row {
    QString metric;
    QString value;
    QString budget;
    bool ok;
};
QList<Row> g_rows;

void report(const QString& metric, double value, const char* unit, double budget, bool lessIsBetter = true)
{
    const bool ok = lessIsBetter ? value < budget : value >= budget;
    g_rows.append({metric, QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(unit)),
                   budget > 0 ? QStringLiteral("%1 %2 %3").arg(lessIsBetter ? QStringLiteral("<") : QStringLiteral("≥"))
                                    .arg(budget)
                                    .arg(QLatin1String(unit))
                              : QStringLiteral("—"),
                   budget > 0 ? ok : true});
}

void waitFor(const std::function<bool()>& done)
{
    while (!done())
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
}

struct Stack {
    QTemporaryDir dir;
    std::unique_ptr<AccountStore> accounts;
    std::unique_ptr<SecretStore> secrets;
    std::unique_ptr<AccountSessions> sessions;
    std::unique_ptr<CapabilityStore> capStore;
    std::unique_ptr<ActivityLog> log;
    std::unique_ptr<CapabilityController> caps;
    std::unique_ptr<Journal> journal;
    std::unique_ptr<TransferService> transfers;
    std::unique_ptr<OperationController> ops;
    Account account;

    explicit Stack(const QUrl& endpoint)
    {
        accounts = std::make_unique<AccountStore>(dir.filePath(QStringLiteral("accounts.ini")));
        auto backend = std::make_unique<testing::FakeKeychainBackend>();
        Account a;
        a.id = QUuid::createUuid();
        a.name = QStringLiteral("bench");
        a.providerId = QStringLiteral("generic");
        a.region = QStringLiteral("bench-1");
        a.endpoint = endpoint.toString();
        a.accessKeyId = QStringLiteral("AKIDBENCH");
        a.environment = Environment::Development;
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                QStringLiteral("bench"));
        secrets = std::make_unique<SecretStore>(std::move(backend));
        accounts->save(a);
        account = a;
        sessions = std::make_unique<AccountSessions>(accounts.get(), secrets.get());
        capStore = std::make_unique<CapabilityStore>(dir.filePath(QStringLiteral("caps")));
        log = std::make_unique<ActivityLog>();
        caps = std::make_unique<CapabilityController>(accounts.get(), sessions.get(), capStore.get(), log.get());
        journal = std::make_unique<Journal>(dir.filePath(QStringLiteral("journal")));
        transfers = std::make_unique<TransferService>(dir.filePath(QStringLiteral("transfers")), nullptr);
        ops = std::make_unique<OperationController>(accounts.get(), sessions.get(), caps.get(), journal.get(), log.get());
        ops->setPlanDir(dir.filePath(QStringLiteral("plans")));
        ops->setTransfers(transfers.get());
    }
};

bool writeFile(const QString& path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QByteArray chunk(qsizetype(4) * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype i = 0; i < chunk.size(); ++i)
        chunk[i] = char((i * 2654435761u) >> 24);
    for (qint64 left = size; left > 0;) {
        const qint64 n = std::min<qint64>(left, chunk.size());
        if (f.write(chunk.constData(), n) != n)
            return false;
        left -= n;
    }
    return true;
}

// Starts an upload job, confirms it, waits for the end.
Job* runUpload(Stack& s, const QStringList& sources, const QString& prefix)
{
    auto* job = new UploadJob(s.account, QStringLiteral("bkt"), prefix, sources, s.ops->planDir(), {});
    QObject::connect(s.ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
        if (j == job)
            j->confirm();
    });
    bool done = false;
    QObject::connect(job, &Job::finished, job, [&done] { done = true; });
    s.ops->start(job);
    waitFor([&] { return done; });
    return job;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("bench_transfer"));
    QCommandLineParser parser;
    const QCommandLineOption filesOpt(QStringLiteral("files"), QStringLiteral("Files in the folder upload."),
                                      QStringLiteral("n"), QStringLiteral("1000000"));
    const QCommandLineOption mibOpt(QStringLiteral("upload-mib"), QStringLiteral("Size of the large upload."),
                                    QStringLiteral("MiB"), QStringLiteral("2048"));
    const QCommandLineOption dirOpt(QStringLiteral("dir"), QStringLiteral("Where to put the local files."),
                                    QStringLiteral("path"));
    parser.addOptions({filesOpt, mibOpt, dirOpt});
    parser.addHelpOption();
    parser.process(app);
    const int files = parser.value(filesOpt).toInt();
    const qint64 uploadBytes = parser.value(mibOpt).toLongLong() * 1024 * 1024;

    ThemeManager theme;
    theme.apply(QStringLiteral("Dark"));
    std::printf("bench_transfer: %d files, %lld MiB upload\n", files, uploadBytes >> 20);
    std::fflush(stdout);

    std::unique_ptr<QTemporaryDir> local = parser.isSet(dirOpt)
        ? std::make_unique<QTemporaryDir>(parser.value(dirOpt) + QStringLiteral("/bench_transfer-XXXXXX"))
        : std::make_unique<QTemporaryDir>();
    ServerThread server(100'000);
    Stack s(server.url());
    StallProbe probe;

    // 1. Planning a folder upload of many files: scan on a worker, the
    //    merge with the (empty) target listing on the GUI thread.
    {
        const QString root = local->filePath(QStringLiteral("tree"));
        for (int d = 0; d < files / 1000 + 1; ++d)
            QDir().mkpath(QStringLiteral("%1/d%2").arg(root).arg(d, 4, 10, QLatin1Char('0')));
        for (int i = 0; i < files; ++i) {
            QFile f(QStringLiteral("%1/d%2/f%3").arg(root).arg(i / 1000, 4, 10, QLatin1Char('0')).arg(i % 1000, 3, 10, QLatin1Char('0')));
            if (!f.open(QIODevice::WriteOnly)) {
                std::fprintf(stderr, "cannot create %s\n", qPrintable(f.fileName()));
                return 1;
            }
        }
        const qint64 rssBefore = residentBytes();
        auto* job = new UploadJob(s.account, QStringLiteral("bkt"), QStringLiteral("up/"), {root}, s.ops->planDir(), {});
        bool previewed = false;
        QObject::connect(s.ops.get(), &OperationController::previewNeeded, job, [&previewed](Job*) { previewed = true; });
        QElapsedTimer clock;
        clock.start();
        probe.start();
        s.ops->start(job);
        waitFor([&] { return previewed || job->phase() == Job::Phase::Finished; });
        const qint64 stall = probe.stop();
        const double secs = double(clock.elapsed()) / 1000.0;
        report(QStringLiteral("plan %1 files: GUI stall").arg(files), double(stall), "ms", 50);
        report(QStringLiteral("plan %1 files: time").arg(files), secs, "s", 0);
        report(QStringLiteral("plan %1 files: memory growth").arg(files), double(residentBytes() - rssBefore) / 1e6, "MB", 0);
        bool cancelled = false;
        QObject::connect(job, &Job::finished, job, [&cancelled] { cancelled = true; });
        job->cancel();
        waitFor([&] { return cancelled; });
        QDir(root).removeRecursively();
    }

    // 2. One large upload: throughput through the transfer lanes.
    {
        const QString big = local->filePath(QStringLiteral("big.bin"));
        if (!writeFile(big, uploadBytes)) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(big));
            return 1;
        }
        QElapsedTimer clock;
        clock.start();
        probe.start();
        Job* job = runUpload(s, {big}, QStringLiteral("big/"));
        const qint64 stall = probe.stop();
        const double secs = double(clock.elapsed()) / 1000.0;
        const bool ok = job->outcome() == Job::Outcome::Succeeded;
        report(QStringLiteral("%1 MiB upload: GUI stall").arg(uploadBytes >> 20), double(stall), "ms", 50);
        report(QStringLiteral("%1 MiB upload: throughput%2").arg(uploadBytes >> 20).arg(ok ? QString() : QStringLiteral(" (FAILED)")),
               double(uploadBytes) / 1e6 / secs, "MB/s", 0);
        QFile::remove(big);
    }

    // 3. Four uploads while the browser lists 100 000 objects.
    {
        QStringList sources;
        for (int i = 0; i < 4; ++i) {
            const QString f = local->filePath(QStringLiteral("par%1.bin").arg(i));
            writeFile(f, Q_INT64_C(256) * 1024 * 1024);
            sources.append(f);
        }
        ObjectBrowser browser(s.accounts.get(), s.sessions.get());
        browser.resize(1000, 700);
        browser.show();
        int finished = 0;
        QList<Job*> jobs;
        probe.start();
        QElapsedTimer clock;
        clock.start();
        for (const QString& f : sources) {
            auto* job = new UploadJob(s.account, QStringLiteral("bkt"), QStringLiteral("par/"), {f}, s.ops->planDir(), {});
            QObject::connect(s.ops.get(), &OperationController::previewNeeded, job, [job](Job* j) {
                if (j == job)
                    j->confirm();
            });
            QObject::connect(job, &Job::finished, job, [&finished] { ++finished; });
            s.ops->start(job);
        }
        browser.navigation()->navigate({s.account.id, QStringLiteral("bkt"), QStringLiteral("many/")});
        waitFor([&] { return browser.model() != nullptr; });
        ObjectTableModel* model = browser.model();
        QObject::connect(model, &ObjectTableModel::pageLoaded, model, [model] { model->loadMore(); });
        model->loadMore();
        waitFor([&] {
            return model->state() == ObjectTableModel::State::Complete || model->state() == ObjectTableModel::State::Error;
        });
        const qint64 listed = model->rowCount();
        waitFor([&] { return finished == int(sources.size()); });
        const qint64 stall = probe.stop();
        report(QStringLiteral("4 uploads + listing %1 objects: GUI stall").arg(listed), double(stall), "ms", 50);
        report(QStringLiteral("4 × 256 MiB: aggregate throughput"), 4.0 * 256 * 1.048576 / (double(clock.elapsed()) / 1000.0),
               "MB/s", 0);
    }

    s3::transfer::flushResumeWrites(5000);
    std::printf("\n%-55s %-14s %-12s %s\n", "metric", "value", "budget", "ok");
    bool all = true;
    for (const Row& r : std::as_const(g_rows)) {
        // Padded in characters, not bytes ("—", "×" are multi-byte).
        std::printf("%s %s %s %s\n", qPrintable(r.metric.leftJustified(55)), qPrintable(r.value.leftJustified(14)),
                    qPrintable(r.budget.leftJustified(12)), r.ok ? "yes" : "NO");
        all = all && r.ok;
    }
    return all ? 0 : 1;
}
