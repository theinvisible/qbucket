// Listing benchmark (PLAN.md §13.4) — not part of ctest. Run a release
// build on a real platform for meaningful numbers:
//
//   build-release/bench/bench_listing [--keys 100000] [--flat-keys 1000000]
//
// A FakeS3Server on its own thread serves synthetic ListObjectsV2 pages;
// the GUI thread runs the real ObjectBrowser (ObjectTableModel, the list
// view, ObjectRowDelegate). A 1 ms probe timer measures the longest
// GUI-thread stall of each phase.

#include "accounts/AccountStore.h"
#include "accounts/SecretStore.h"
#include "controllers/AccountSessions.h"
#include "models/ObjectTableModel.h"
#include "support/FakeBucket.h"
#include "support/FakeKeychainBackend.h"
#include "controllers/BrowseController.h"
#include "ui/ObjectBrowser.h"
#include "ui/ThemeManager.h"

#include <qbucket/s3/testing/FakeS3Server.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTableView>

#include <cstdio>
#include <functional>

#if defined(Q_OS_LINUX)
#include <unistd.h>
#include <fstream>
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

// Longest gap between 1 ms timer ticks, minus the interval.
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

// The server and its key list live on their own thread: generating XML
// must not count as GUI work.
class ServerThread {
public:
    explicit ServerThread(int keys, const QString& prefix)
    {
        m_anchor.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(&m_anchor, [this, keys, prefix] {
            m_bucket = std::make_unique<testing::FakeBucket>();
            for (int i = 0; i < keys; ++i) {
                m_bucket->add(QStringLiteral("%1obj-%2.bin").arg(prefix).arg(i, 8, 10, QLatin1Char('0')),
                              (qint64(i) * 7919) % 100'000'000, i % 7 == 0 ? "STANDARD_IA" : "STANDARD");
            }
            m_bucket->sort();
            m_server = std::make_unique<FakeS3Server>();
            m_server->setFallbackHandler([this](const RecordedRequest& r) { return m_bucket->list(r); });
            m_server->listen();
            m_url = m_server->endpointUrl();
        }, Qt::BlockingQueuedConnection);
    }
    ~ServerThread()
    {
        QMetaObject::invokeMethod(&m_anchor, [this] {
            m_server.reset();
            m_bucket.reset();
        }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    [[nodiscard]] QUrl url() const { return m_url; }

private:
    QThread m_thread;
    QObject m_anchor;
    std::unique_ptr<testing::FakeBucket> m_bucket;
    std::unique_ptr<FakeS3Server> m_server;
    QUrl m_url;
};

struct Row {
    const char* metric;
    QString value;
    QString budget;
    bool ok;
};
QList<Row> g_rows;

void report(const char* metric, double value, const char* unit, double budget)
{
    g_rows.append({metric, QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(unit)),
                   QStringLiteral("< %1 %2").arg(budget).arg(QLatin1String(unit)), value < budget});
}

void waitFor(const std::function<bool()>& done)
{
    while (!done())
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
}

struct Harness {
    QTemporaryDir dir;
    std::unique_ptr<AccountStore> accounts;
    std::unique_ptr<SecretStore> secrets;
    std::unique_ptr<AccountSessions> sessions;
    QUuid account;

    explicit Harness(const QUrl& endpoint)
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
        backend->entries.insert(QString::fromLatin1(SecretStore::kService) + u'|'
                                    + SecretStore::keyFor(a.id, SecretStore::Kind::SecretAccessKey),
                                QStringLiteral("bench"));
        secrets = std::make_unique<SecretStore>(std::move(backend));
        accounts->save(a);
        account = a.id;
        sessions = std::make_unique<AccountSessions>(accounts.get(), secrets.get());
    }
};

// Loads every page; returns the longest GUI stall while doing so.
qint64 loadAll(ObjectTableModel& model, StallProbe& probe, qint64* firstRowsMs)
{
    QElapsedTimer clock;
    clock.start();
    probe.start();
    bool first = true;
    QObject::connect(&model, &ObjectTableModel::pageLoaded, &model, [&] {
        if (first) {
            *firstRowsMs = clock.elapsed();
            first = false;
        }
        model.loadMore();
    });
    model.loadMore();
    waitFor([&] {
        return model.state() == ObjectTableModel::State::Complete || model.state() == ObjectTableModel::State::Error;
    });
    QObject::disconnect(&model, &ObjectTableModel::pageLoaded, &model, nullptr);
    return probe.stop();
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCommandLineParser args;
    args.addHelpOption();
    args.addOption({QStringLiteral("keys"), QStringLiteral("Keys in the folder listing."), QStringLiteral("n"),
                    QStringLiteral("100000")});
    args.addOption({QStringLiteral("flat-keys"), QStringLiteral("Keys in the flat listing (0: skip)."),
                    QStringLiteral("n"), QStringLiteral("1000000")});
    args.process(app);
    const int keys = args.value(QStringLiteral("keys")).toInt();
    const int flatKeys = args.value(QStringLiteral("flat-keys")).toInt();

    ThemeManager theme;
    theme.apply(QStringLiteral("dark"));
    StallProbe probe;

    std::printf("qbucket listing bench — %d keys, flat %d keys (%s build)\n", keys, flatKeys,
#ifdef QT_DEBUG
                "DEBUG — numbers are meaningless"
#else
                "release"
#endif
    );

    {
        ServerThread server(keys, QStringLiteral("bench/"));
        Harness h(server.url());
        ObjectBrowser browser(h.accounts.get(), h.sessions.get());
        browser.resize(1100, 700);
        browser.show();
        ObjectTableModel& model = *browser.model();
        QTableView& view = *browser.view();

        const qint64 before = residentBytes();
        qint64 firstRows = 0;
        QElapsedTimer total;
        total.start();
        probe.start();
        browser.navigation()->navigate({h.account, QStringLiteral("bkt"), QStringLiteral("bench/")});
        const qint64 setStall = probe.stop();
        const qint64 loadStall = loadAll(model, probe, &firstRows);
        const qint64 loadMs = total.elapsed();
        const qint64 after = residentBytes();
        std::printf("  loaded %d rows in %lld ms (%d pages)\n", model.rowCount(), loadMs, model.pagesLoaded());

        report("First rows after setLocation (incl. local round trip)", double(firstRows), "ms", 100);
        report("Longest GUI stall while paging (≈ work per page)", double(std::max(setStall, loadStall)), "ms", 10);
        if (before > 0 && keys > 0)
            report("Memory per 100k loaded rows", double(after - before) / keys * 100000 / (1024.0 * 1024.0), "MB", 40);

        // Theme switch with the rows loaded: synchronous repolish + repaint.
        QElapsedTimer t;
        t.start();
        theme.apply(QStringLiteral("light"));
        view.viewport()->repaint();
        report("Theme switch with the rows loaded", double(t.elapsed()), "ms", 150);
        theme.apply(QStringLiteral("dark"));

        // Background sort by size: the GUI only applies the result.
        probe.start();
        t.restart();
        bool sorted = false;
        QObject::connect(&model, &ObjectTableModel::sortFinished, &model, [&] { sorted = true; });
        model.sort(ObjectTableModel::SizeColumn, Qt::DescendingOrder);
        waitFor([&] { return sorted; });
        const qint64 sortStall = probe.stop();
        std::printf("  sorted by size in %lld ms\n", t.elapsed());
        report("Longest GUI stall during a background sort", double(sortStall), "ms", 50);
    }

    if (flatKeys > 0) {
        ServerThread server(flatKeys, QStringLiteral("flat/a/b/"));
        Harness h(server.url());
        ObjectBrowser browser(h.accounts.get(), h.sessions.get());
        browser.resize(1100, 700);
        browser.show();
        browser.setMode(ObjectTableModel::Mode::Flat);
        ObjectTableModel& model = *browser.model();
        QTableView& view = *browser.view();
        const qint64 flatBefore = residentBytes();
        qint64 firstRows = 0;
        browser.navigation()->navigate({h.account, QStringLiteral("bkt"), QStringLiteral("flat/")});
        const qint64 stall = loadAll(model, probe, &firstRows);
        std::printf("  flat: loaded %d rows, %.1f MB\n", model.rowCount(),
                    double(residentBytes() - flatBefore) / (1024.0 * 1024.0));
        report("Longest GUI stall while paging a flat listing", double(stall), "ms", 50);

        // Scroll through it: each step a synchronous repaint.
        QScrollBar* sb = view.verticalScrollBar();
        qint64 worst = 0;
        double sum = 0;
        constexpr int kSteps = 300;
        for (int i = 0; i < kSteps; ++i) {
            QElapsedTimer t;
            t.start();
            sb->setValue(int(qint64(sb->maximum()) * i / kSteps));
            view.viewport()->repaint();
            const qint64 ms = t.elapsed();
            worst = std::max(worst, ms);
            sum += double(ms);
        }
        std::printf("  scrolling: %.2f ms per step on average\n", sum / kSteps);
        report("Worst scroll step through the flat listing", double(worst), "ms", 16);
    }

    std::printf("\n%-58s %14s %12s\n", "Metric", "Value", "Budget");
    bool allOk = true;
    for (const Row& r : std::as_const(g_rows)) {
        std::printf("%-58s %14s %12s  %s\n", r.metric, qPrintable(r.value), qPrintable(r.budget),
                    r.ok ? "ok" : "OVER");
        allOk = allOk && r.ok;
    }
    return allOk ? 0 : 1;
}
