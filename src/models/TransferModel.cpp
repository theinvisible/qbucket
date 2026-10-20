#include "models/TransferModel.h"

#include "controllers/DownloadJob.h"
#include "controllers/OperationController.h"
#include "controllers/UploadJob.h"

#include <QLocale>

namespace qbucket {

namespace {

constexpr qint64 kSpeedWindowMs = 5000;

} // namespace

TransferModel::TransferModel(OperationController* operations, QObject* parent)
    : QAbstractTableModel(parent),
      m_ops(operations)
{
    m_clock.start();
    m_tick.setInterval(500);
    connect(&m_tick, &QTimer::timeout, this, &TransferModel::tick_);
    if (m_ops) {
        connect(m_ops, &OperationController::jobStarted, this, [this](Job* job) {
            if (job->transferKind() != Job::TransferKind::None)
                add_(job);
        });
        connect(m_ops, &OperationController::jobFinished, this, &TransferModel::finished_);
    }
}

TransferModel::~TransferModel() = default;

int TransferModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int TransferModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QString TransferModel::formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond <= 0)
        return {};
    return tr("%1/s").arg(QLocale().formattedDataSize(qint64(bytesPerSecond)));
}

QString TransferModel::formatRemaining(qint64 seconds)
{
    if (seconds < 0)
        return {};
    if (seconds < 60)
        return tr("%n s", nullptr, int(seconds));
    if (seconds < 3600)
        return tr("%n min", nullptr, int((seconds + 30) / 60));
    return tr("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString TransferModel::stateText_(const Row& r) const
{
    if (r.finished)
        return r.outcomeText;
    const Job* job = r.job;
    if (!job)
        return {};
    if (job->isPaused())
        return tr("paused");
    switch (job->phase()) {
    case Job::Phase::Created:
    case Job::Phase::Planning:
        return r.itemsDone > 0 ? tr("planning (%1 found)").arg(r.itemsDone) : tr("planning");
    case Job::Phase::AwaitingConfirmation:
        return tr("waiting for confirmation");
    case Job::Phase::Executing:
        return tr("%1 of %2").arg(r.itemsDone).arg(r.itemsTotal);
    case Job::Phase::Verifying:
        return tr("verifying");
    case Job::Phase::Finished:
        break;
    }
    return {};
}

QVariant TransferModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows[index.row()];
    const bool running = !r.finished && r.job && r.job->phase() == Job::Phase::Executing;
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return r.title;
        case ProgressColumn:
            if (r.bytesTotal > 0)
                return tr("%1 of %2").arg(QLocale().formattedDataSize(r.bytesDone), QLocale().formattedDataSize(r.bytesTotal));
            return {};
        case SpeedColumn:
            return running ? formatSpeed(r.speed) : QString();
        case RemainingColumn:
            if (running && r.speed > 0 && r.bytesTotal > 0)
                return formatRemaining(qint64(double(r.bytesTotal - r.bytesDone) / r.speed));
            return {};
        case StateColumn:
            return stateText_(r);
        default:
            return {};
        }
    case Qt::ToolTipRole:
        return r.finished ? r.summary : (index.column() == StateColumn ? stateText_(r) : r.title);
    case ProgressRole:
        if (r.finished && r.outcome == Job::Outcome::Succeeded)
            return 1000;
        if (r.bytesTotal > 0)
            return int(std::clamp<qint64>(r.bytesDone * 1000 / r.bytesTotal, 0, 1000));
        return -1;
    case FinishedRole:
        return r.finished;
    case CanPauseRole:
        return running && r.job->canPause();
    case PausedRole:
        return !r.finished && r.job && r.job->isPaused();
    case CanRetryRole:
        return r.finished && bool(r.retry);
    case KindRole:
        return int(r.kind);
    default:
        return {};
    }
}

QVariant TransferModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn: return tr("Transfer");
    case ProgressColumn: return tr("Progress");
    case SpeedColumn: return tr("Speed");
    case RemainingColumn: return tr("Remaining");
    case StateColumn: return tr("State");
    default: return {};
    }
}

Job* TransferModel::jobAt(int row) const
{
    return row >= 0 && row < m_rows.size() && !m_rows[row].finished ? m_rows[row].job.data() : nullptr;
}

int TransferModel::runningCount() const
{
    return int(std::count_if(m_rows.begin(), m_rows.end(), [](const Row& r) { return !r.finished; }));
}

void TransferModel::pause(int row)
{
    if (Job* j = jobAt(row))
        j->pause();
}

void TransferModel::resume(int row)
{
    if (Job* j = jobAt(row))
        j->resume();
}

void TransferModel::cancel(int row)
{
    if (Job* j = jobAt(row))
        j->cancel();
}

void TransferModel::retry(int row)
{
    if (row < 0 || row >= m_rows.size() || !m_rows[row].finished || !m_rows[row].retry || !m_ops)
        return;
    m_ops->start(m_rows[row].retry());
}

void TransferModel::clearFinished()
{
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (!m_rows[i].finished)
            continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }
}

int TransferModel::rowOf_(const Job* job) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].finished && m_rows[i].job == job)
            return i;
    }
    return -1;
}

void TransferModel::add_(Job* job)
{
    Row r;
    r.job = job;
    r.kind = job->transferKind();
    r.title = job->title();
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size()));
    m_rows.append(r);
    endInsertRows();
    const auto dirty = [this, job] {
        const int i = rowOf_(job);
        if (i >= 0)
            m_rows[i].dirty = true;
    };
    // Progress is shown with the next tick; a phase or pause change at once
    // (rare, and the panel's buttons depend on it).
    const auto changed = [this, job] {
        const int i = rowOf_(job);
        if (i >= 0)
            emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
    };
    connect(job, &Job::progressChanged, this, dirty);
    connect(job, &Job::phaseChanged, this, changed);
    connect(job, &Job::pausedChanged, this, changed);
    if (!m_tick.isActive())
        m_tick.start();
    emit transferStarted();
}

void TransferModel::finished_(Job* job)
{
    const int i = rowOf_(job);
    if (i < 0)
        return;
    Row& r = m_rows[i];
    r.finished = true;
    r.outcome = job->outcome();
    r.bytesDone = job->bytesDone();
    r.bytesTotal = job->bytesTotal();
    r.summary = OperationController::summary(*job);
    r.title = job->title();
    const Job::Report& rep = job->report();
    switch (r.outcome) {
    case Job::Outcome::Succeeded:
        r.outcomeText = rep.verification.isEmpty() ? tr("done") : tr("done — %1").arg(rep.verification);
        break;
    case Job::Outcome::PartlyFailed:
        r.outcomeText = tr("%1 failed, %2 done").arg(rep.failed).arg(rep.succeeded);
        break;
    case Job::Outcome::Failed:
        r.outcomeText = tr("failed: %1").arg(!job->errorText().isEmpty() ? job->errorText() : rep.failures.value(0));
        break;
    case Job::Outcome::Cancelled:
        r.outcomeText = job->isSuspended() ? tr("stopped — continues next time") : tr("cancelled");
        break;
    case Job::Outcome::Refused:
        r.outcomeText = tr("refused: %1").arg(job->errorText());
        break;
    case Job::Outcome::None:
        break;
    }
    // Retry exactly what failed, with the same settings.
    const Account account = job->account();
    const QString bucket = job->bucket();
    if (const auto* up = qobject_cast<const UploadJob*>(job); up && !up->failedItems().isEmpty() && m_ops) {
        const QList<UploadJob::Item> items = up->failedItems();
        const QString planDir = m_ops->planDir();
        r.retry = [account, bucket, items, planDir] { return new UploadJob(account, bucket, items, planDir); };
    } else if (const auto* down = qobject_cast<const DownloadJob*>(job); down && !down->failedItems().isEmpty() && m_ops) {
        const QList<DownloadJob::Item> items = down->failedItems();
        const QString target = down->targetDir();
        const QString planDir = m_ops->planDir();
        r.retry = [account, bucket, items, target, planDir] {
            return new DownloadJob(account, bucket, items, target, planDir, {});
        };
    }
    r.job = nullptr;
    emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
}

void TransferModel::tick_()
{
    const qint64 now = m_clock.elapsed();
    bool anyRunning = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (r.finished || !r.job)
            continue;
        anyRunning = true;
        const Job* job = r.job;
        r.bytesDone = job->bytesDone();
        r.bytesTotal = job->bytesTotal();
        r.itemsDone = job->progressDone();
        r.itemsTotal = job->progressTotal();
        // Speed over the last few seconds (paused: nothing moves).
        r.samples.emplace_back(now, r.bytesDone);
        while (r.samples.size() > 2 && now - r.samples.front().first > kSpeedWindowMs)
            r.samples.pop_front();
        const auto& [t0, b0] = r.samples.front();
        r.speed = now > t0 && !job->isPaused() ? double(r.bytesDone - b0) * 1000.0 / double(now - t0) : 0.0;
        emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
        r.dirty = false;
    }
    if (!anyRunning)
        m_tick.stop();
}

} // namespace qbucket
