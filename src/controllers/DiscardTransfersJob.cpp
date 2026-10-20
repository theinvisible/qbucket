#include "controllers/DiscardTransfersJob.h"

#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <QFile>
#include <QPointer>
#include <QThreadPool>

namespace qbucket {

DiscardTransfersJob::DiscardTransfersJob(const Account& account, const TransferJobStore::Record& record, QObject* parent)
    : Job(account, record.bucket, parent),
      m_record(record)
{
}

DiscardTransfersJob::~DiscardTransfersJob() = default;

QString DiscardTransfersJob::title() const
{
    return tr("Discard: %1").arg(m_record.title);
}

void DiscardTransfersJob::plan()
{
    m_run = std::make_unique<QObject>();
    TransferService* transfers = controller_()->transfers();
    if (!transfers) {
        fail_(tr("Nothing to discard."));
        return;
    }
    // The job's files under way: part records named "<job id>-<n>".
    auto store = transfers->resumeStore();
    auto* relay = new QObject;
    relay->moveToThread(thread());
    const QPointer<DiscardTransfersJob> self(this);
    const QString prefix = m_record.id + u'-';
    QThreadPool::globalInstance()->start([relay, self, store, prefix] {
        QList<s3::transfer::ResumeState> states;
        for (const QString& id : store->ids()) {
            if (!id.startsWith(prefix))
                continue;
            if (const auto s = store->load(id))
                states.append(*s);
            else
                (void)store->remove(id);
        }
        QMetaObject::invokeMethod(relay, [relay, self, states] {
            if (self && self->phase() == Phase::Planning) {
                self->m_states = states;
                Preview p;
                p.action = tr("Discard");
                p.scope = self->m_record.title;
                p.objects = states.size();
                self->setPreview_(p);
            }
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void DiscardTransfersJob::execute()
{
    m_index = 0;
    setProgress_(0, m_states.size());
    next_();
}

void DiscardTransfersJob::next_()
{
    if (phase() != Phase::Executing)
        return;
    TransferService* transfers = controller_()->transfers();
    if (isCancelled() || m_index >= m_states.size() || !transfers) {
        if (TransferJobStore* jobs = transfers ? transfers->jobs() : nullptr)
            jobs->remove(m_record.id);
        executed_();
        return;
    }
    const s3::transfer::ResumeState state = m_states[m_index++];
    const s3::transfer::TransferRecord& r = state.record;
    auto store = transfers->resumeStore();
    setProgress_(m_index, m_states.size());
    if (r.direction == s3::transfer::Direction::Download) {
        const QString partial = r.localPath;
        QThreadPool::globalInstance()->start([partial, store, id = r.id] {
            QFile::remove(partial);
            (void)store->remove(id);
        });
        ++m_report.succeeded;
        QMetaObject::invokeMethod(this, &DiscardTransfersJob::next_, Qt::QueuedConnection);
        return;
    }
    s3::Session* session = session_();
    if (!session || r.uploadId.isEmpty()) {
        QThreadPool::globalInstance()->start([store, id = r.id] { (void)store->remove(id); });
        ++m_report.skipped;
        QMetaObject::invokeMethod(this, &DiscardTransfersJob::next_, Qt::QueuedConnection);
        return;
    }
    session->abortMultipartUpload(r.bucket, r.key, r.uploadId, m_run.get(), [this, r, store](const s3::Result<void>& done) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::AbortMultipartUpload), r.key);
        rec.message = r.uploadId;
        if (done || done.error().category == s3::ErrorCategory::NotFound) {
            ++m_report.succeeded;
            QThreadPool::globalInstance()->start([store, id = r.id] { (void)store->remove(id); });
        } else {
            ++m_report.failed;
            rec.result = Journal::Result::Failed;
            rec.httpStatus = done.error().httpStatus;
            rec.message += u' ' + done.error().toString();
            if (m_report.failures.size() < kMaxListed)
                m_report.failures.append(QStringLiteral("%1: %2").arg(r.key, ConnectionTester::describe(done.error())));
        }
        journal_({rec});
        next_();
    });
}

void DiscardTransfersJob::verify()
{
    m_report.verified = m_report.failed == 0;
    finish_(m_report);
}

} // namespace qbucket
