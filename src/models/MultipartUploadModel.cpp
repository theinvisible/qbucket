#include "models/MultipartUploadModel.h"

#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"
#include "controllers/TransferService.h"

#include <QLocale>
#include <QPointer>
#include <QThreadPool>

namespace qbucket {

MultipartUploadModel::MultipartUploadModel(AccountSessions* sessions, TransferService* transfers, const QUuid& account,
                                           const QString& bucket, QObject* parent)
    : QAbstractTableModel(parent),
      m_sessions(sessions),
      m_transfers(transfers),
      m_account(account),
      m_bucket(bucket)
{
}

MultipartUploadModel::~MultipartUploadModel() = default;

int MultipartUploadModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int MultipartUploadModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant MultipartUploadModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const s3::MultipartUploadInfo& u = m_rows[index.row()];
    const bool resumable = m_resumable.contains(u.uploadId);
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case KeyColumn: return u.key;
        case StartedColumn: return QLocale().toString(u.initiated.toLocalTime(), QLocale::ShortFormat);
        case ClassColumn: return u.storageClass;
        case ResumableColumn: return resumable ? tr("yes — this computer can continue it") : QString();
        default: return {};
        }
    case Qt::ToolTipRole:
        return tr("Upload id %1, started by %2").arg(u.uploadId, u.initiator.isEmpty() ? tr("unknown") : u.initiator);
    case UploadIdRole:
        return u.uploadId;
    case ResumableRole:
        return resumable;
    default:
        return {};
    }
}

QVariant MultipartUploadModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case KeyColumn: return tr("Key");
    case StartedColumn: return tr("Started");
    case ClassColumn: return tr("Class");
    case ResumableColumn: return tr("Resumable here");
    default: return {};
    }
}

void MultipartUploadModel::reload()
{
    m_run = std::make_unique<QObject>();   // drops answers of an earlier load
    ++m_generation;
    m_incoming.clear();
    m_truncated = false;
    m_error.clear();
    m_state = State::Loading;
    emit stateChanged();
    loadResumable_();
    fetch_({}, {}, m_generation);
}

void MultipartUploadModel::loadResumable_()
{
    if (!m_transfers)
        return;
    auto store = m_transfers->resumeStore();
    auto* relay = new QObject;
    relay->moveToThread(thread());
    const QPointer<MultipartUploadModel> self(this);
    QThreadPool::globalInstance()->start([relay, self, store] {
        QSet<QString> ids;
        for (const QString& id : store->ids()) {
            if (const auto s = store->load(id); s && !s->record.uploadId.isEmpty())
                ids.insert(s->record.uploadId);
        }
        QMetaObject::invokeMethod(relay, [relay, self, ids] {
            if (self) {
                self->m_resumable = ids;
                if (!self->m_rows.isEmpty())
                    emit self->dataChanged(self->index(0, 0), self->index(int(self->m_rows.size()) - 1, ColumnCount - 1));
            }
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void MultipartUploadModel::fetch_(const QString& keyMarker, const QString& uploadIdMarker, quint64 generation)
{
    const s3::Result<s3::Session*> session = m_sessions->session(m_account);
    if (!session) {
        m_state = State::Error;
        m_error = ConnectionTester::describe(session.error());
        emit stateChanged();
        return;
    }
    s3::ListMultipartUploadsParams p;
    p.bucket = m_bucket;
    p.keyMarker = keyMarker;
    p.uploadIdMarker = uploadIdMarker;
    (*session)->listMultipartUploads(p, m_run.get(), [this, generation](const s3::Result<s3::ListMultipartUploadsPage>& page) {
        if (generation != m_generation)
            return;
        if (!page) {
            m_state = State::Error;
            m_error = ConnectionTester::describe(page.error());
            emit stateChanged();
            return;
        }
        m_incoming += page->uploads;
        if (page->isTruncated && m_incoming.size() < kMaxUploads) {
            fetch_(page->nextKeyMarker, page->nextUploadIdMarker, generation);
            return;
        }
        m_truncated = page->isTruncated;
        beginResetModel();
        m_rows = std::exchange(m_incoming, {});
        endResetModel();
        m_state = State::Ready;
        emit stateChanged();
    });
}

} // namespace qbucket
