#include "controllers/InPlaceCopy.h"

#include "controllers/ConnectionTester.h"
#include "controllers/Job.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/transfer/Copier.h>

#include <QTimer>

#include <algorithm>

namespace qbucket {

namespace {

// "" and STANDARD are the same class.
bool sameClass(const QString& a, const QString& b)
{
    const auto norm = [](const QString& c) { return c.isEmpty() ? QStringLiteral("STANDARD") : c.toUpper(); };
    return norm(a) == norm(b);
}

// The class header a copy must send: the default needs none.
QString classToSend(const QString& target)
{
    return target.isEmpty() || target.compare(QLatin1String("STANDARD"), Qt::CaseInsensitive) == 0 ? QString() : target;
}

bool sameTags(s3::TagSet a, s3::TagSet b)
{
    const auto byKey = [](const s3::Tag& x, const s3::Tag& y) { return x.key < y.key; };
    std::sort(a.begin(), a.end(), byKey);
    std::sort(b.begin(), b.end(), byKey);
    return a == b;
}

// What differs between the headers a copy should have and has (names only).
QStringList headerDifferences(const s3::ObjectHeaders& want, const s3::ObjectHeaders& have)
{
    QStringList out;
    // An empty content type becomes the server's default.
    if (!want.contentType.isEmpty() && want.contentType != have.contentType)
        out.append(QStringLiteral("Content-Type"));
    // A struct, not std::pair: MSVC can't parse a pointer-to-member type as a
    // template argument here (C3083).
    struct Field {
        const char* name;
        QString s3::ObjectHeaders::*member;
    };
    const Field fields[] = {
        {"Cache-Control", &s3::ObjectHeaders::cacheControl},
        {"Content-Disposition", &s3::ObjectHeaders::contentDisposition},
        {"Content-Encoding", &s3::ObjectHeaders::contentEncoding},
        {"Content-Language", &s3::ObjectHeaders::contentLanguage},
        {"Expires", &s3::ObjectHeaders::expires},
        {"x-amz-website-redirect-location", &s3::ObjectHeaders::websiteRedirectLocation},
    };
    for (const auto& [name, member] : fields) {
        if (want.*member != have.*member)
            out.append(QLatin1String(name));
    }
    if (want.userMetadata != have.userMetadata)
        out.append(QStringLiteral("x-amz-meta-*"));
    return out;
}

} // namespace

struct InPlaceCopy::State {
    Request request;
    Done done;
    bool inParts = false;
    bool rechecked = false;
};

InPlaceCopy::InPlaceCopy(Job* job)
    : QObject(job),
      m_job(job)
{
}

InPlaceCopy::~InPlaceCopy() = default;

void InPlaceCopy::run(const Request& request, Done done)
{
    auto s = std::make_shared<State>();
    s->request = request;
    s->done = std::move(done);
    s3::Session* session = m_job->session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [s] { s->done(tr("not changed (not connected)")); }, Qt::QueuedConnection);
        return;
    }
    const s3::ObjectMeta& m = request.source.meta;
    if (m.size > kCopyLimit) {
        copyInParts_(s);
        return;
    }
    s3::CopyObjectParams p;
    p.sourceBucket = p.bucket = m_job->bucket();
    p.sourceKey = p.key = request.key;
    p.sourceVersionId = request.sourceVersionId;
    p.ifMatch = m.etag;
    p.metadataDirective = s3::MetadataDirective::Replace;
    p.headers = request.headers;
    p.storageClass = classToSend(request.targetClass);
    p.serverSideEncryption = m.serverSideEncryption == QLatin1String("AES256") ? m.serverSideEncryption : QString();
    p.cannedAcl = request.canned;
    session->copyObject(p, this, [this, s](const s3::Result<s3::CopyObjectResult>& r) {
        Journal::Record rec = m_job->record_(s3::operationName(s3::Operation::CopyObject), s->request.key);
        rec.message = s->request.journalNote;
        if (!r) {
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message += QStringLiteral(": ") + err.toString();
            m_job->journal_({rec});
            s->done(err.category == s3::ErrorCategory::PreconditionFailed
                        ? tr("changed meanwhile — left alone")
                        : tr("not changed: %1").arg(ConnectionTester::describe(err)));
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        m_job->journal_({rec});
        copied_(s);
    });
}

void InPlaceCopy::copyInParts_(const std::shared_ptr<State>& s)
{
    s3::Session* session = m_job->session_();
    TransferService* transfers = m_job->controller_() ? m_job->controller_()->transfers() : nullptr;
    if (!session || !transfers) {
        QMetaObject::invokeMethod(this, [s] { s->done(tr("not changed (no transfer service)")); }, Qt::QueuedConnection);
        return;
    }
    s->inParts = true;
    const s3::ObjectMeta& m = s->request.source.meta;
    s3::transfer::CopyRequest req;
    req.sourceBucket = req.bucket = m_job->bucket();
    req.sourceKey = req.key = s->request.key;
    req.sourceVersionId = s->request.sourceVersionId;
    req.ifMatch = m.etag;
    req.storageClass = classToSend(s->request.targetClass);
    req.replaceHeaders = s->request.headers;
    req.serverSideEncryption = m.serverSideEncryption == QLatin1String("AES256") ? m.serverSideEncryption : QString();
    req.cannedAcl = s->request.canned;
    auto copier = std::make_unique<s3::transfer::Copier>(session, req, transfers->limiter(m_job->account().id));
    s3::transfer::Copier* raw = copier.get();
    connect(raw, &s3::transfer::Copier::finished, this, [this, raw, s] {
        const s3::Result<s3::transfer::CopyResult>& r = raw->result();
        Journal::Record rec = m_job->record_(QStringLiteral("MultipartCopy"), s->request.key);
        rec.message = s->request.journalNote;
        if (r) {
            rec.httpStatus = 200;
            rec.versionId = r->versionId;
            m_job->journal_({rec});
            copied_(s);
        } else {
            rec.result = Journal::Result::Failed;
            rec.httpStatus = r.error().httpStatus;
            rec.message += QStringLiteral(": ") + r.error().toString();
            m_job->journal_({rec});
            s->done(r.error().category == s3::ErrorCategory::PreconditionFailed
                        ? tr("changed meanwhile — left alone")
                        : tr("not changed: %1").arg(ConnectionTester::describe(r.error())));
        }
        for (auto it = m_copiers.begin(); it != m_copiers.end(); ++it) {
            if (it->get() == raw) {
                it->release()->deleteLater();
                m_copiers.erase(it);
                break;
            }
        }
    });
    m_copiers.push_back(std::move(copier));
    raw->start();
}

void InPlaceCopy::cancel()
{
    for (const auto& c : m_copiers)
        c->cancel();
}

void InPlaceCopy::copied_(const std::shared_ptr<State>& s)
{
    if (!s->request.source.tags.value_or(s3::TagSet()).isEmpty()) {
        restoreTags_(s);
        return;
    }
    check_(s);
}

// RGW drops an object's tags on such a copy: put them back (and only then
// check everything).
void InPlaceCopy::restoreTags_(const std::shared_ptr<State>& s)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        s->done(tr("changed, but its tags could not be checked (not connected)"));
        return;
    }
    session->getObjectTagging(m_job->bucket(), s->request.key, QString(), this, [this, s](const s3::Result<s3::ObjectTagging>& now) {
        const s3::TagSet want = s->request.source.tags.value_or(s3::TagSet());
        if (now && sameTags(now->tags, want)) {
            check_(s);
            return;
        }
        s3::Session* again = m_job->session_();
        if (!again) {
            s->done(tr("changed, but its tags could not be written back (not connected)"));
            return;
        }
        s3::PutObjectTaggingParams p;
        p.bucket = m_job->bucket();
        p.key = s->request.key;
        p.tags = want;
        again->putObjectTagging(p, this, [this, s, want](const s3::Result<s3::SubresourceWriteResult>& r) {
            Journal::Record rec = m_job->record_(s3::operationName(s3::Operation::PutObjectTagging), s->request.key);
            rec.message = QStringLiteral("tags carried over: %1").arg(s->request.journalNote);
            if (!r) {
                rec.result = Journal::Result::Failed;
                rec.httpStatus = r.error().httpStatus;
                rec.message += QStringLiteral(": ") + r.error().toString();
                m_job->journal_({rec});
                QStringList tags;
                for (const s3::Tag& t : want)
                    tags.append(t.key + u'=' + t.value);
                s->done(tr("changed, but its tags could not be written back (%1): they were %2")
                            .arg(ConnectionTester::describe(r.error()), tags.join(QStringLiteral(", "))));
                return;
            }
            rec.httpStatus = 200;
            rec.requestId = r->requestId;
            m_job->journal_({rec});
            check_(s);
        });
    });
}

void InPlaceCopy::check_(const std::shared_ptr<State>& s)
{
    s3::Session* session = m_job->session_();
    if (!session) {
        s->done(tr("changed, but not checked (not connected)"));
        return;
    }
    const ObjectSnapshot& source = s->request.source;
    const bool tags = !source.tags.value_or(s3::TagSet()).isEmpty();
    const bool acl = !s->request.canned.isEmpty();
    ObjectSnapshot::read(session, m_job->bucket(), s->request.key, tags, acl, this, [this, s, tags, acl](s3::Result<ObjectSnapshot> now) {
        const ObjectSnapshot& source = s->request.source;
        QStringList wrong;
        if (!now) {
            wrong.append(ConnectionTester::describe(now.error()));
        } else {
            const s3::ObjectMeta& m = now->meta;
            if (m.size != source.meta.size)
                wrong.append(tr("size"));
            const QString& before = source.meta.etag;
            if (!s->inParts && !before.contains(u'-') && m.etag != before)
                wrong.append(tr("content (ETag)"));
            wrong += headerDifferences(s->request.headers, s3::objectHeadersOf(m));
            if (!sameClass(m.storageClass, s->request.targetClass))
                wrong.append(tr("storage class (%1)").arg(m.storageClass.isEmpty() ? QStringLiteral("STANDARD") : m.storageClass));
            if (tags && (!now->tags || !sameTags(*now->tags, *source.tags)))
                wrong.append(tr("tags"));
            if (acl && (!now->acl || s3::cannedAclOf(*now->acl) != std::optional<QString>(s->request.canned)))
                wrong.append(tr("permissions"));
        }
        if (wrong.isEmpty()) {
            s->done({});
            return;
        }
        if (!s->rechecked) {
            // Reads may lag behind a write for a moment.
            s->rechecked = true;
            QTimer::singleShot(m_recheckMs, this, [this, s] { check_(s); });
            return;
        }
        s->done(tr("changed, but it doesn't match: %1").arg(wrong.join(QStringLiteral(", "))));
    });
}

} // namespace qbucket
