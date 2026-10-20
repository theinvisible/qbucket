#include "controllers/CreateFolderJob.h"

#include "controllers/ConnectionTester.h"

namespace qbucket {

CreateFolderJob::CreateFolderJob(const Account& account, const QString& bucket, const QString& parentPrefix,
                                 const QString& name, QObject* parent)
    : Job(account, bucket, parent),
      m_name(name),
      m_key(parentPrefix + name + u'/')
{
}

QString CreateFolderJob::validateName(const QString& name)
{
    if (name.isEmpty())
        return tr("The name is empty.");
    if (name.contains(u'/'))
        return tr("A folder name cannot contain “/”.");
    // QNAM normalises these path segments on the wire (CLAUDE.md).
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return tr("“.” and “..” cannot be used as names.");
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return tr("The name contains control characters.");
    }
    if (name.toUtf8().size() > 1000)
        return tr("The name is too long.");
    return {};
}

QString CreateFolderJob::title() const
{
    return tr("Create folder “%1”").arg(m_name);
}

void CreateFolderJob::plan()
{
    if (const QString problem = validateName(m_name); !problem.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, problem] { fail_(problem); }, Qt::QueuedConnection);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Not connected.")); }, Qt::QueuedConnection);
        return;
    }
    m_run = std::make_unique<QObject>();
    session->headObject(bucket(), m_key, QString(), m_run.get(), [this](const s3::Result<s3::ObjectMeta>& head) {
        if (head) {
            skip_(tr("the folder exists already"));
            return;
        }
        if (head.error().category != s3::ErrorCategory::NotFound) {
            fail_(ConnectionTester::describe(head.error()));
            return;
        }
        // No marker — but keys below it make the folder exist anyway.
        s3::ListObjectsParams params;
        params.bucket = bucket();
        params.prefix = m_key;
        params.maxKeys = 1;
        s3::Session* now = session_();
        if (!now) {
            fail_(tr("Not connected."));
            return;
        }
        now->listObjects(params, m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
            if (!page) {
                fail_(ConnectionTester::describe(page.error()));
                return;
            }
            if (!page->objects.isEmpty() || !page->commonPrefixes.isEmpty()) {
                skip_(tr("the folder exists already (objects are stored below it)"));
                return;
            }
            Preview p;
            p.action = tr("Create Folder");
            p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_key);
            p.objects = 1;
            p.bytes = 0;
            setPreview_(p);
        });
    });
}

void CreateFolderJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    setProgress_(0, 1);
    s3::PutObjectParams params;
    params.bucket = bucket();
    params.key = m_key;
    params.contentType = QStringLiteral("application/x-directory");
    m_run = std::make_unique<QObject>();
    session->putObject(params, m_run.get(), [this](const s3::Result<s3::PutObjectResult>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::PutObject), m_key);
        if (r) {
            rec.httpStatus = 200;
            rec.requestId = r->requestId;
            rec.versionId = r->versionId;
            journal_({rec});
            setProgress_(1, 1);
            executed_();
            return;
        }
        const s3::Error& e = r.error();
        rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
        rec.httpStatus = e.httpStatus;
        rec.requestId = e.requestId;
        rec.message = e.toString();
        journal_({rec});
        Report report;
        report.failed = 1;
        report.failures = {QStringLiteral("%1: %2").arg(m_key, ConnectionTester::describe(e))};
        if (e.outcomeUnknown)
            report.verification = tr("the answer was lost — the folder may or may not exist");
        finish_(report);
    });
}

void CreateFolderJob::verify()
{
    s3::Session* session = session_();
    Report report;
    report.succeeded = 1;
    if (!session) {
        report.verification = tr("not verified (not connected)");
        finish_(report);
        return;
    }
    session->headObject(bucket(), m_key, QString(), m_run.get(), [this, report](const s3::Result<s3::ObjectMeta>& head) mutable {
        report.verified = head && head->size == 0;
        report.verification = report.verified ? tr("verified") : tr("not verified: the folder marker could not be read back");
        finish_(report);
    });
}

void CreateFolderJob::onCancel()
{
    m_run.reset();   // answers still on their way are dropped
    if (phase() == Phase::Executing || phase() == Phase::Verifying) {
        Report report;
        report.verification = tr("cancelled while creating — the folder may exist");
        finish_(report);
    }
}

void CreateFolderJob::skip_(const QString& why)
{
    Report report;
    report.skipped = 1;
    report.verification = why;
    finish_(report);
}

} // namespace qbucket
