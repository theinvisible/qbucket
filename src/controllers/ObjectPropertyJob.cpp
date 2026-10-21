#include "controllers/ObjectPropertyJob.h"

#include "controllers/ConnectionTester.h"
#include "controllers/ObjectSnapshot.h"

#include <qbucket/s3/Session.h>

#include <algorithm>

namespace qbucket {

namespace {

bool sameTags(s3::TagSet a, s3::TagSet b)
{
    const auto byKey = [](const s3::Tag& x, const s3::Tag& y) { return x.key < y.key; };
    std::sort(a.begin(), a.end(), byKey);
    std::sort(b.begin(), b.end(), byKey);
    return a == b;
}

} // namespace

ObjectPropertyJob::ObjectPropertyJob(const Account& account, const QString& bucket, const QString& key,
                                     Property property, QObject* parent)
    : Job(account, bucket, parent),
      m_key(key),
      m_property(property)
{
}

ObjectPropertyJob* ObjectPropertyJob::tags(const Account& account, const QString& bucket, const QString& key,
                                           const s3::TagSet& tags, QObject* parent)
{
    auto* job = new ObjectPropertyJob(account, bucket, key, Property::Tags, parent);
    job->m_tags = tags;
    return job;
}

ObjectPropertyJob* ObjectPropertyJob::acl(const Account& account, const QString& bucket, const QString& key,
                                          const QString& cannedAcl, QObject* parent)
{
    auto* job = new ObjectPropertyJob(account, bucket, key, Property::Acl, parent);
    job->m_canned = cannedAcl;
    return job;
}

QString ObjectPropertyJob::title() const
{
    return m_property == Property::Tags ? tr("Set the tags of “%1”").arg(m_key)
                                        : tr("Set the access of “%1” to %2").arg(m_key, m_canned);
}

// The write feature decides (profile knowledge, never a read probe); the
// library refuses a write whose read feature is Unsupported anyway.
QList<s3::Feature> ObjectPropertyJob::requiredFeatures() const
{
    return {m_property == Property::Tags ? s3::Feature::ObjectTaggingWrite : s3::Feature::ObjectAclWrite};
}

QString ObjectPropertyJob::describeTags(const s3::TagSet& tags)
{
    if (tags.isEmpty())
        return tr("(none)");
    QStringList out;
    for (const s3::Tag& t : tags)
        out.append(t.value.isEmpty() ? t.key : t.key + u'=' + t.value);
    return out.join(QStringLiteral(", "));
}

// --- plan ---------------------------------------------------------------------------

void ObjectPropertyJob::plan()
{
    if (m_property == Property::Tags) {
        if (const QString problem = s3::tagSetProblem(m_tags); !problem.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, problem] { fail_(tr("These tags can't be set: %1").arg(problem)); },
                                      Qt::QueuedConnection);
            return;
        }
    } else if (m_canned.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("No access chosen.")); }, Qt::QueuedConnection);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Not connected.")); }, Qt::QueuedConnection);
        return;
    }
    m_run = std::make_unique<QObject>();
    const bool tags = m_property == Property::Tags;
    ObjectSnapshot::read(session, bucket(), m_key, tags, !tags, m_run.get(), [this, tags](s3::Result<ObjectSnapshot> snap) {
        if (phase() != Phase::Planning)
            return;
        if (!snap) {
            fail_(tr("Could not read “%1”: %2").arg(m_key, ConnectionTester::describe(snap.error())));
            return;
        }
        QString current;
        if (tags)
            current = describeTags(snap->tags.value_or(s3::TagSet()));
        else
            current = snap->acl ? s3::cannedAclOf(*snap->acl).value_or(tr("custom")) : tr("unknown");
        planned_(snap->meta, current);
    });
}

void ObjectPropertyJob::planned_(const s3::ObjectMeta& meta, const QString& current)
{
    m_planned = meta;
    Preview p;
    p.action = m_property == Property::Tags ? tr("Set Tags") : tr("Set Access");
    p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_key);
    p.objects = 1;
    p.samples = {m_key};
    if (m_property == Property::Tags) {
        p.details.append(tr("Now: %1").arg(current));
        p.details.append(tr("New: %1").arg(describeTags(m_tags)));
    } else {
        p.details.append(tr("Now: %1").arg(current));
        p.details.append(tr("New: %1").arg(m_canned));
        if (m_canned.startsWith(QLatin1String("public-read")))
            p.warnings.append(tr("Anyone who knows the address can then read this object, without credentials."));
        if (m_canned == QLatin1String("public-read-write"))
            p.warnings.append(tr("public-read-write also grants writing where the server applies it to objects."));
        if (current == tr("custom"))
            p.warnings.append(tr("The object has permissions for particular accounts; they are replaced."));
    }
    if (!meta.versionId.isEmpty() && meta.versionId != QLatin1String("null"))
        p.details.append(tr("Applies to the current version."));
    setPreview_(p);
}

// --- execute ------------------------------------------------------------------------

void ObjectPropertyJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        finish_(report_(false, tr("not changed (not connected)"), QString()));
        return;
    }
    // Right before writing: still the object the preview showed?
    session->headObject(bucket(), m_key, QString(), m_run.get(), [this](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Executing)
            return;
        if (!head) {
            finish_(report_(false, tr("not changed: %1").arg(ConnectionTester::describe(head.error())), QString()));
            return;
        }
        if (head->etag != m_planned.etag || head->size != m_planned.size) {
            finish_(report_(false, tr("changed since it was planned — left alone"), QString()));
            return;
        }
        write_();
    });
}

void ObjectPropertyJob::write_()
{
    s3::Session* session = session_();
    if (!session) {
        finish_(report_(false, tr("not changed (not connected)"), QString()));
        return;
    }
    const auto done = [this](const s3::Result<s3::SubresourceWriteResult>& r) {
        if (phase() != Phase::Executing)
            return;
        Journal::Record rec = record_(s3::operationName(m_property == Property::Tags ? s3::Operation::PutObjectTagging
                                                                                     : s3::Operation::PutObjectAcl),
                                      m_key);
        rec.message = m_property == Property::Tags ? describeTags(m_tags) : m_canned;
        if (!r) {
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message += QStringLiteral(": ") + err.toString();
            journal_({rec});
            finish_(report_(false, tr("not changed: %1").arg(ConnectionTester::describe(err)), QString()));
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        journal_({rec});
        m_written = true;
        setProgress_(1, 1);
        executed_();
    };
    if (m_property == Property::Tags) {
        s3::PutObjectTaggingParams p;
        p.bucket = bucket();
        p.key = m_key;
        p.tags = m_tags;
        session->putObjectTagging(p, m_run.get(), done);
    } else {
        s3::PutObjectAclParams p;
        p.bucket = bucket();
        p.key = m_key;
        p.cannedAcl = m_canned;
        session->putObjectAcl(p, m_run.get(), done);
    }
}

void ObjectPropertyJob::onCancel()
{
    if (phase() == Phase::Planning)
        m_run.reset();
}

// --- verify -------------------------------------------------------------------------

void ObjectPropertyJob::verify()
{
    s3::Session* session = session_();
    if (!session) {
        finish_(report_(true, QString(), tr("not checked (not connected)")));
        return;
    }
    const bool tags = m_property == Property::Tags;
    ObjectSnapshot::read(session, bucket(), m_key, tags, !tags, m_run.get(), [this, tags](s3::Result<ObjectSnapshot> now) {
        if (!now) {
            finish_(report_(true, QString(), tr("not checked: %1").arg(ConnectionTester::describe(now.error()))));
            return;
        }
        // The object's data must be what it was: a write handled as an
        // upload (a proxy dropping ?tagging / ?acl) would have replaced it.
        if (now->meta.etag != m_planned.etag || now->meta.size != m_planned.size) {
            finish_(report_(false,
                            tr("the object's data changed — the server may have treated the request as an upload; "
                               "check its versions"),
                            QString()));
            return;
        }
        const bool ok = tags ? (now->tags && sameTags(*now->tags, m_tags))
                             : (now->acl && s3::cannedAclOf(*now->acl) == std::optional<QString>(m_canned));
        finish_(report_(ok, ok ? QString() : tr("written, but it doesn't read back as set"),
                        ok ? tr("read back as set; the object's data unchanged") : QString(), ok));
    });
}

Job::Report ObjectPropertyJob::report_(bool ok, const QString& failure, const QString& verification, bool verified) const
{
    Report r;
    r.succeeded = ok ? 1 : 0;
    r.failed = ok ? 0 : 1;
    if (!ok)
        r.failures.append(QStringLiteral("%1: %2").arg(m_key, failure));
    r.verified = verified;
    r.verification = verification;
    return r;
}

} // namespace qbucket
