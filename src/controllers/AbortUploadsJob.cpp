#include "controllers/AbortUploadsJob.h"

#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <QSet>
#include <QThreadPool>

namespace qbucket {

AbortUploadsJob::AbortUploadsJob(const Account& account, const QString& bucket, const QList<Upload>& uploads,
                                 QObject* parent)
    : Job(account, bucket, parent),
      m_uploads(uploads)
{
}

AbortUploadsJob::~AbortUploadsJob() = default;

QString AbortUploadsJob::title() const
{
    return tr("Abort %n unfinished upload(s) in s3://%1", nullptr, int(m_uploads.size())).arg(bucket());
}

void AbortUploadsJob::plan()
{
    m_run = std::make_unique<QObject>();
    QMetaObject::invokeMethod(this, [this] {
        if (phase() != Phase::Planning)
            return;
        Preview p;
        p.action = tr("Abort Uploads");
        p.scope = QStringLiteral("s3://%1").arg(bucket());
        p.objects = m_uploads.size();
        for (const Upload& u : std::as_const(m_uploads)) {
            if (p.samples.size() >= 10)
                break;
            p.samples.append(u.key);
        }
        p.details.append(tr("The parts uploaded so far are deleted. Objects already in the bucket are not touched."));
        p.permanent = true;
        setProgress_(0, m_uploads.size());
        setPreview_(p);
    }, Qt::QueuedConnection);
}

void AbortUploadsJob::execute()
{
    pump_();
}

void AbortUploadsJob::pump_()
{
    if (phase() != Phase::Executing)
        return;
    s3::Session* session = session_();
    while (!isCancelled() && session && m_inFlight < kParallel && m_next < m_uploads.size()) {
        const Upload u = m_uploads[m_next++];
        ++m_inFlight;
        session->abortMultipartUpload(bucket(), u.key, u.uploadId, m_run.get(), [this, u](const s3::Result<void>& r) {
            --m_inFlight;
            Journal::Record rec = record_(s3::operationName(s3::Operation::AbortMultipartUpload), u.key);
            rec.message = u.uploadId;
            if (r || r.error().category == s3::ErrorCategory::NotFound) {
                rec.httpStatus = r ? 204 : 404;
                ++m_report.succeeded;
            } else {
                rec.result = Journal::Result::Failed;
                rec.httpStatus = r.error().httpStatus;
                rec.requestId = r.error().requestId;
                rec.message += u' ' + r.error().toString();
                ++m_report.failed;
                if (m_report.failures.size() < kMaxListed)
                    m_report.failures.append(QStringLiteral("%1: %2").arg(u.key, ConnectionTester::describe(r.error())));
            }
            journal_({rec});
            setProgress_(m_report.succeeded + m_report.failed, m_uploads.size());
            pump_();
        });
    }
    if (!session && m_inFlight == 0 && m_next < m_uploads.size()) {
        fail_(tr("Not connected."));
        return;
    }
    if (m_inFlight == 0 && (m_next >= m_uploads.size() || isCancelled()))
        executed_();
}

void AbortUploadsJob::onCancel()
{
    // What is under way finishes; nothing new starts (pump_ checks).
}

void AbortUploadsJob::verify()
{
    // The resume records of these uploads are of no use any more.
    if (TransferService* transfers = controller_()->transfers()) {
        QSet<QString> ids;
        for (const Upload& u : std::as_const(m_uploads))
            ids.insert(u.uploadId);
        auto store = transfers->resumeStore();
        QThreadPool::globalInstance()->start([store, ids] {
            for (const QString& id : store->ids()) {
                const auto s = store->load(id);
                if (s && ids.contains(s->record.uploadId))
                    (void)store->remove(id);
            }
        });
    }
    m_report.verified = m_report.failed == 0;
    m_report.verification = tr("%n upload(s) aborted", nullptr, int(m_report.succeeded));
    finish_(m_report);
}

} // namespace qbucket
