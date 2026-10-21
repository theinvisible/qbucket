#include "controllers/DragStaging.h"

#include "accounts/AccountStore.h"
#include "controllers/DownloadJob.h"
#include "controllers/OperationController.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace qbucket {
namespace {

constexpr int kSweepMs = 10 * 60 * 1000;

QString defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/drag");
}

} // namespace

DragStaging::DragStaging(OperationController* operations, QString root, QObject* parent)
    : QObject(parent),
      m_ops(operations),
      m_root(root.isEmpty() ? defaultRoot() : std::move(root)),
      m_sweep(new QTimer(this))
{
    m_sweep->setInterval(kSweepMs);
    connect(m_sweep, &QTimer::timeout, this, &DragStaging::cleanup);
    m_sweep->start();
    cleanup();   // what an earlier session left
}

DragStaging::~DragStaging() = default;

QString DragStaging::signature(const Selection& selection)
{
    QStringList keys;
    for (const Item& i : selection.items)
        keys.append((i.folder ? QStringLiteral("d:") : QStringLiteral("f:")) + i.key);
    keys.sort();
    return selection.account.toString(QUuid::WithoutBraces) + u'|' + selection.bucket + u'|' + keys.join(u'\n');
}

DragStaging::State DragStaging::state(const Selection& selection) const
{
    const auto it = m_stages.constFind(signature(selection));
    if (it == m_stages.constEnd())
        return State::None;
    if (it->state == State::Ready && readyPaths(selection).isEmpty())
        return State::None;   // expired or taken away
    return it->state;
}

QStringList DragStaging::readyPaths(const Selection& selection) const
{
    const auto it = m_stages.constFind(signature(selection));
    if (it == m_stages.constEnd() || it->state != State::Ready || it->paths.isEmpty())
        return {};
    if (it->readyAt.secsTo(QDateTime::currentDateTimeUtc()) > m_maxAgeSecs)
        return {};
    for (const QString& p : it->paths) {
        if (!QFileInfo::exists(p))
            return {};   // the drop target moved it away
    }
    return it->paths;
}

bool DragStaging::stagesQuietly(const Selection& selection) const
{
    if (selection.items.isEmpty() || selection.items.size() > kQuietFiles)
        return false;
    qint64 bytes = 0;
    for (const Item& i : selection.items) {
        if (i.folder || i.size < 0)
            return false;   // a folder's size isn't known before it is listed
        bytes += i.size;
    }
    return m_confirmAbove < 0 || bytes <= m_confirmAbove;
}

void DragStaging::prepare(const Selection& selection, bool quiet)
{
    const QString sig = signature(selection);
    const State now = state(selection);
    if (now == State::Preparing || now == State::Ready)
        return;
    if (quiet && !stagesQuietly(selection))
        return;
    const std::optional<Account> account = m_ops->accounts()->account(selection.account);
    if (!account || selection.items.isEmpty())
        return;
    remove_(sig);   // a failed or expired one
    Stage s;
    s.id = QUuid::createUuid();
    s.selection = selection;
    s.state = State::Preparing;
    s.dir = m_root + u'/' + s.id.toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(s.dir)) {
        emit message(tr("Nothing can be prepared for dragging: %1 could not be created.").arg(QDir::toNativeSeparators(s.dir)));
        return;
    }
    m_stages.insert(sig, s);
    emit stateChanged(sig);

    QList<DownloadJob::Item> items;
    for (const Item& i : selection.items)
        items.append({i.key, i.folder, {}});
    DownloadJob::Options options;
    options.transient = true;
    options.confirmAboveBytes = m_confirmAbove;
    auto* job = new DownloadJob(*account, selection.bucket, items, s.dir, m_ops->planDir(), options);
    connect(job, &Job::finished, this, [this, sig, job] { finished_(sig, job); });
    m_ops->start(job);
}

void DragStaging::finished_(const QString& sig, Job* job)
{
    const auto it = m_stages.find(sig);
    if (it == m_stages.end())
        return;
    const Job::Report& r = job->report();
    if (job->outcome() != Job::Outcome::Succeeded || r.failed > 0 || r.skipped > 0) {
        const bool cancelled = job->outcome() == Job::Outcome::Cancelled;
        QString why = job->errorText();
        if (why.isEmpty())
            why = !r.failures.isEmpty() ? r.failures.first() : !r.notes.isEmpty() ? r.notes.first() : QString();
        it->state = State::Failed;
        QDir(it->dir).removeRecursively();
        if (!cancelled)
            emit message(why.isEmpty() ? tr("Preparing for dragging out failed.")
                                       : tr("Preparing for dragging out failed: %1").arg(why));
        emit stateChanged(sig);
        return;
    }
    QStringList paths;
    const QDir dir(it->dir);
    for (const QString& name : dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
        paths.append(dir.filePath(name));
    it->paths = paths;
    it->state = paths.isEmpty() ? State::Failed : State::Ready;
    it->readyAt = QDateTime::currentDateTimeUtc();
    if (it->state == State::Ready)
        emit message(tr("Ready to drag out: %n item(s).", nullptr, int(paths.size())));
    emit stateChanged(sig);
}

void DragStaging::reportNotReady(const Selection& selection)
{
    const State s = state(selection);
    if (s == State::Preparing)
        emit message(tr("Still downloading what you dragged — drag it again in a moment."));
    else if (s != State::Ready)
        emit message(tr("To drag this out, it is downloaded first: use “Prepare for Dragging Out”, then drag again."));
}

void DragStaging::remove_(const QString& sig)
{
    const auto it = m_stages.find(sig);
    if (it == m_stages.end())
        return;
    if (it->state != State::Preparing)
        QDir(it->dir).removeRecursively();
    m_stages.erase(it);
}

void DragStaging::cleanup()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QSet<QString> keep;
    for (auto it = m_stages.begin(); it != m_stages.end();) {
        const bool expired = it->state == State::Failed
                             || (it->state == State::Ready && it->readyAt.secsTo(now) > m_maxAgeSecs);
        if (expired) {
            QDir(it->dir).removeRecursively();
            const QString sig = it.key();
            it = m_stages.erase(it);
            emit stateChanged(sig);
            continue;
        }
        keep.insert(it->id.toString(QUuid::WithoutBraces));
        ++it;
    }
    // Only uuid-named folders of ours, never anything else.
    const QDir root(m_root);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QUuid::fromString(name).isNull() && !keep.contains(name))
            QDir(root.filePath(name)).removeRecursively();
    }
}

} // namespace qbucket
