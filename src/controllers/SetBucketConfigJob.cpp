#include "controllers/SetBucketConfigJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"

#include <QJsonDocument>
#include <QLocale>
#include <QTimer>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace qbucket {

namespace {

QString prettyXml(const QByteArray& document)
{
    QString out;
    QXmlStreamReader r(document);
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartDocument() || r.isEndDocument())
            continue;
        if (r.isCharacters() && r.isWhitespace())
            continue;
        w.writeCurrentToken(r);
    }
    if (r.hasError())
        return QString::fromUtf8(document);
    return out.trimmed();
}

QString onOff(bool on)
{
    return on ? SetBucketConfigJob::tr("on") : SetBucketConfigJob::tr("off");
}

} // namespace

SetBucketConfigJob::SetBucketConfigJob(const Account& account, const QString& bucket, s3::BucketConfig kind,
                                       bool remove)
    : Job(account, bucket),
      m_kind(kind),
      m_remove(remove)
{
}

SetBucketConfigJob::~SetBucketConfigJob() = default;

SetBucketConfigJob* SetBucketConfigJob::put(const Account& account, const QString& bucket, s3::BucketConfig kind,
                                            const QByteArray& document, const QString& cannedAcl)
{
    auto* job = new SetBucketConfigJob(account, bucket, kind, false);
    job->m_document = document;
    job->m_cannedAcl = cannedAcl;
    return job;
}

SetBucketConfigJob* SetBucketConfigJob::remove(const Account& account, const QString& bucket, s3::BucketConfig kind)
{
    return new SetBucketConfigJob(account, bucket, kind, true);
}

QString SetBucketConfigJob::kindText(s3::BucketConfig kind)
{
    using K = s3::BucketConfig;
    switch (kind) {
    case K::Policy: return tr("the bucket policy");
    case K::PolicyStatus: return tr("the policy status");
    case K::Cors: return tr("the CORS rules");
    case K::Lifecycle: return tr("the lifecycle rules");
    case K::ObjectLock: return tr("the default retention");
    case K::Acl: return tr("the bucket ACL");
    case K::PublicAccessBlock: return tr("the public access block");
    case K::Tagging: return tr("the bucket tags");
    case K::Website: return tr("the website configuration");
    case K::Logging: return tr("the access logging");
    case K::Replication: return tr("the replication rules");
    case K::Encryption: return tr("the default encryption");
    case K::Notifications: return tr("the event notifications");
    case K::OwnershipControls: return tr("the ownership controls");
    }
    return {};
}

QString SetBucketConfigJob::title() const
{
    return m_remove ? tr("Remove %1 of “%2”").arg(kindText(m_kind), bucket())
                    : tr("Change %1 of “%2”").arg(kindText(m_kind), bucket());
}

QList<s3::Feature> SetBucketConfigJob::requiredFeatures() const
{
    if (const std::optional<s3::Feature> write = s3::bucketConfigWriteFeature(m_kind))
        return {*write};
    return {s3::bucketConfigFeature(m_kind)};
}

QString SetBucketConfigJob::pretty(s3::BucketConfig kind, const QByteArray& document)
{
    if (document.trimmed().isEmpty())
        return {};
    if (kind == s3::BucketConfig::Policy) {
        const QJsonDocument json = QJsonDocument::fromJson(document);
        return json.isNull() ? QString::fromUtf8(document)
                             : QString::fromUtf8(json.toJson(QJsonDocument::Indented)).trimmed();
    }
    return prettyXml(document);
}

QString SetBucketConfigJob::problem(s3::BucketConfig kind, const QByteArray& document)
{
    using K = s3::BucketConfig;
    QString detail;
    switch (kind) {
    case K::Policy:
        detail = s3::policyProblems(document).join(QStringLiteral("; "));
        break;
    case K::Cors: {
        const s3::Result<s3::CorsConfiguration> c = s3::parseCorsConfiguration(document);
        detail = c ? s3::corsProblem(*c) : c.error().message;
        break;
    }
    case K::Lifecycle: {
        const s3::Result<s3::LifecycleConfiguration> c = s3::parseLifecycleConfiguration(document);
        detail = c ? s3::lifecycleProblem(*c) : c.error().message;
        break;
    }
    case K::ObjectLock: {
        const s3::Result<s3::ObjectLockConfiguration> c = s3::parseObjectLockConfiguration(document);
        detail = c ? s3::objectLockProblem(*c) : c.error().message;
        break;
    }
    case K::PublicAccessBlock: {
        const s3::Result<s3::PublicAccessBlockConfiguration> c = s3::parsePublicAccessBlock(document);
        detail = c ? QString() : c.error().message;
        break;
    }
    case K::Acl: {
        const s3::Result<s3::AccessControlPolicy> c = s3::parseAccessControlPolicyDocument(document);
        detail = c ? QString() : c.error().message;
        break;
    }
    default:
        return tr("QBucket doesn't change %1.").arg(kindText(kind));
    }
    return detail.isEmpty() ? QString() : tr("The document is not valid: %1").arg(detail);
}

// --- plan ------------------------------------------------------------------------

void SetBucketConfigJob::plan()
{
    m_run = std::make_unique<QObject>();
    QString refusal;
    if (m_remove && !s3::bucketConfigDeleteOperation(m_kind))
        refusal = tr("%1 can't be removed this way.").arg(kindText(m_kind));
    else if (!m_remove && !s3::bucketConfigPutOperation(m_kind))
        refusal = tr("QBucket doesn't change %1.").arg(kindText(m_kind));
    else if (!m_remove && m_kind == s3::BucketConfig::Acl && m_document.isEmpty() && m_cannedAcl.isEmpty())
        refusal = tr("No ACL chosen.");
    else if (!m_remove && !(m_kind == s3::BucketConfig::Acl && m_document.isEmpty()))
        refusal = problem(m_kind, m_document);
    if (!refusal.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, refusal] { fail_(refusal); }, Qt::QueuedConnection);
        return;
    }
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 60, m_run.get(), [this](bool fresh) {
        if (!fresh) {
            fail_(tr("The bucket's current state could not be checked. Nothing was changed."));
            return;
        }
        s3::Session* session = session_();
        if (!session) {
            fail_(tr("Not connected."));
            return;
        }
        // What is set now — fresh, never what the dialog showed.
        session->getBucketConfig(bucket(), m_kind, m_run.get(), [this](const s3::Result<s3::BucketConfigDocument>& r) {
            if (!r) {
                fail_(tr("%1 could not be read: %2").arg(kindText(m_kind), ConnectionTester::describe(r.error())));
                return;
            }
            planWith_(*r);
        });
    });
}

bool SetBucketConfigJob::matches_(const s3::BucketConfigDocument& doc) const
{
    if (m_remove)
        return !doc.configured;
    if (!doc.configured && m_kind != s3::BucketConfig::Acl)
        return false;
    if (m_kind == s3::BucketConfig::Acl && m_document.isEmpty()) {
        const s3::Result<s3::AccessControlPolicy> acl = s3::parseAccessControlPolicyDocument(doc.body);
        return acl && s3::cannedAclOf(*acl) == m_cannedAcl;
    }
    return s3::equivalentBucketConfig(m_kind, m_document, doc.body);
}

void SetBucketConfigJob::planWith_(const s3::BucketConfigDocument& current)
{
    if (matches_(current)) {
        Report r;
        r.verified = true;
        r.verification = m_remove ? tr("%1: nothing is set").arg(kindText(m_kind)) : tr("%1: unchanged").arg(kindText(m_kind));
        finish_(r);
        return;
    }
    Preview p;
    p.objects = -1;
    p.scope = QStringLiteral("s3://%1 — %2").arg(bucket(), kindText(m_kind));
    p.action = m_remove ? tr("Remove") : tr("Apply");
    p.currentDocument = current.configured ? pretty(m_kind, current.body) : QString();
    if (m_kind == s3::BucketConfig::Acl && m_document.isEmpty())
        p.document = tr("Canned ACL: %1").arg(m_cannedAcl);
    else
        p.document = pretty(m_kind, m_document);
    if (!current.configured && m_kind != s3::BucketConfig::Acl)
        p.details.append(tr("Nothing is set now."));
    p.warnings = warnings_(current, &p);
    setPreview_(p);
}

QStringList SetBucketConfigJob::warnings_(const s3::BucketConfigDocument& current, Preview* p) const
{
    using K = s3::BucketConfig;
    QStringList out;
    if (m_remove) {
        switch (m_kind) {
        case K::Policy:
            out.append(tr("Whatever the policy allowed or denied no longer applies."));
            break;
        case K::Cors:
            out.append(tr("Web pages on other sites can no longer read from this bucket through a browser."));
            break;
        case K::Lifecycle:
            out.append(tr("Nothing expires or moves automatically any more."));
            break;
        case K::PublicAccessBlock:
            out.append(tr("Public ACLs and public policies take effect again, where there are any."));
            break;
        default:
            break;
        }
        return out;
    }
    switch (m_kind) {
    case K::Policy: {
        const s3::PolicyFindings f = s3::analyzePolicy(m_document);
        if (f.publicWrite)
            out.append(tr("Anyone on the internet may write or delete objects with this policy."));
        else if (f.publicRead)
            out.append(tr("Anyone on the internet may read what the policy names, without credentials."));
        if (f.deny)
            out.append(tr("A Deny statement can lock out these credentials too — QBucket included, also from "
                          "changing the policy again."));
        if (f.publicWrite)
            p->typeToConfirm = bucket();
        break;
    }
    case K::Lifecycle: {
        const s3::Result<s3::LifecycleConfiguration> c = s3::parseLifecycleConfiguration(m_document);
        if (!c)
            break;
        const CapabilityController::Versioning v = controller_()->capabilities()->versioningOf(account().id, bucket());
        const bool versioned = v.status == s3::VersioningStatus::Enabled || v.status == s3::VersioningStatus::Suspended;
        for (const s3::LifecycleRule& rule : c->rules) {
            if (!rule.enabled)
                continue;
            const QString where = rule.prefix.isEmpty() && rule.tags.isEmpty()
                ? tr("every object")
                : (rule.prefix.isEmpty() ? tr("objects with the rule's tags") : tr("objects under “%1”").arg(rule.prefix));
            const QString name = rule.id.isEmpty() ? tr("A rule") : tr("Rule “%1”").arg(rule.id);
            if (rule.expirationDays) {
                out.append(versioned ? tr("%1 deletes %2 %n day(s) after they were written (a delete marker; "
                                          "the version stays).", nullptr, *rule.expirationDays).arg(name, where)
                                     : tr("%1 deletes %2 for good %n day(s) after they were written.", nullptr,
                                          *rule.expirationDays).arg(name, where));
            } else if (rule.expirationDate.isValid()) {
                out.append(tr("%1 deletes %2 on %3.").arg(name, where, QLocale().toString(rule.expirationDate)));
            }
            if (rule.noncurrentDays) {
                out.append(tr("%1 deletes earlier versions of %2 for good %n day(s) after they were replaced.",
                              nullptr, *rule.noncurrentDays).arg(name, where));
            }
        }
        if (!out.isEmpty())
            out.append(tr("Lifecycle rules apply to what exists now as well, from the server's next run on."));
        break;
    }
    case K::ObjectLock: {
        const s3::Result<s3::ObjectLockConfiguration> c = s3::parseObjectLockConfiguration(m_document);
        if (!c)
            break;
        if (c->mode == s3::ObjectLockMode::Compliance) {
            out.append(tr("COMPLIANCE: every version written from now on can't be deleted or overwritten by anyone "
                          "— not even the account owner — until its retention has run out."));
            p->typeToConfirm = bucket();
        } else if (c->mode) {
            out.append(tr("GOVERNANCE: new versions can't be deleted until their retention runs out, except by "
                          "someone allowed to bypass it."));
        }
        p->details.append(tr("It applies to versions written from now on; some servers take a few minutes to "
                             "pass it on to every upload."));
        break;
    }
    case K::Acl: {
        const QString canned = m_cannedAcl;
        if (canned == QLatin1String("public-read-write")) {
            out.append(tr("Anyone on the internet may list the bucket and write to it."));
            p->typeToConfirm = bucket();
        } else if (canned == QLatin1String("public-read")) {
            out.append(tr("Anyone on the internet may list the bucket."));
        } else if (canned == QLatin1String("authenticated-read")) {
            out.append(tr("Any account of this provider may list the bucket."));
        }
        const s3::Result<s3::AccessControlPolicy> now = s3::parseAccessControlPolicyDocument(current.body);
        if (now && !s3::cannedAclOf(*now))
            out.append(tr("The current ACL has grants a canned ACL can't express; they are replaced."));
        break;
    }
    case K::PublicAccessBlock: {
        const s3::Result<s3::PublicAccessBlockConfiguration> c = s3::parsePublicAccessBlock(m_document);
        if (c) {
            p->details.append(tr("Block public ACLs: %1; ignore public ACLs: %2; block public policies: %3; "
                                 "restrict public buckets: %4.")
                                  .arg(onOff(c->blockPublicAcls), onOff(c->ignorePublicAcls),
                                       onOff(c->blockPublicPolicy), onOff(c->restrictPublicBuckets)));
        }
        break;
    }
    default:
        break;
    }
    return out;
}

// --- execute -------------------------------------------------------------------------

void SetBucketConfigJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    const std::optional<s3::Operation> known =
        m_remove ? s3::bucketConfigDeleteOperation(m_kind) : s3::bucketConfigPutOperation(m_kind);
    if (!known) {   // checked in plan(); never sent
        fail_(tr("QBucket doesn't change %1.").arg(kindText(m_kind)));
        return;
    }
    const s3::Operation op = *known;
    auto done = [this, op](const s3::Result<s3::SubresourceWriteResult>& r) {
        Journal::Record rec = record_(s3::operationName(op));
        rec.message = m_remove ? QStringLiteral("%1 removed").arg(s3::bucketConfigName(m_kind))
                               : (m_cannedAcl.isEmpty() ? QStringLiteral("%1 written").arg(s3::bucketConfigName(m_kind))
                                                        : QStringLiteral("canned ACL %1").arg(m_cannedAcl));
        if (!r) {
            const s3::Error& e = r.error();
            rec.result = e.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message += QStringLiteral(": ") + e.toString();
            journal_({rec});
            if (e.category == s3::ErrorCategory::Protocol) {
                // An answer like CreateBucket's / DeleteBucket's: look at the bucket.
                checkBucket_(tr("The server's answer doesn't fit the request (%1).").arg(e.message), false);
                return;
            }
            fail_(tr("The server refused: %1").arg(ConnectionTester::describe(e)));
            return;
        }
        rec.httpStatus = m_remove ? 204 : 200;
        rec.requestId = r->requestId;
        journal_({rec});
        executed_();
    };
    if (m_remove) {
        session->deleteBucketConfig(bucket(), m_kind, m_run.get(), std::move(done));
        return;
    }
    s3::PutBucketConfigParams params;
    params.bucket = bucket();
    params.kind = m_kind;
    params.body = m_document;
    params.cannedAcl = m_cannedAcl;
    session->putBucketConfig(params, m_run.get(), std::move(done));
}

// --- verify --------------------------------------------------------------------------------

void SetBucketConfigJob::verify()
{
    readBack_(1);
}

// Gateways may answer with the old document for a while: a few tries before
// the mismatch is reported — and then it is reported, never assumed away.
void SetBucketConfigJob::readBack_(int attempt)
{
    s3::Session* session = session_();
    if (!session) {
        Report r;
        r.succeeded = 1;
        r.verification = tr("not checked (not connected)");
        finish_(r);
        return;
    }
    session->getBucketConfig(bucket(), m_kind, m_run.get(), [this, attempt](const s3::Result<s3::BucketConfigDocument>& doc) {
        if ((!doc || !matches_(*doc)) && attempt < kReadBackTries) {
            QTimer::singleShot(m_delayMs, m_run.get(), [this, attempt] { readBack_(attempt + 1); });
            return;
        }
        const bool ok = doc && matches_(*doc);
        QString readBack;
        if (!doc)
            readBack = tr("not read back: %1").arg(ConnectionTester::describe(doc.error()));
        else if (ok)
            readBack = m_remove ? tr("read back: nothing is set") : tr("read back: as sent");
        else
            readBack = tr("the server still reports something else — check again in a few minutes");
        if (m_remove) {
            checkBucket_(readBack, ok);   // a DeleteBucket in disguise would show here
            return;
        }
        controller_()->capabilities()->recheck(account().id, bucket());
        Report r;
        r.succeeded = 1;
        r.verified = ok;
        r.verification = readBack;
        finish_(r);
    });
}

void SetBucketConfigJob::checkBucket_(const QString& readBack, bool readBackOk)
{
    s3::Session* session = session_();
    if (!session) {
        fail_(readBack);
        return;
    }
    session->headBucket(bucket(), m_run.get(), [this, readBack, readBackOk](const s3::Result<void>& r) {
        if (!r && r.error().category == s3::ErrorCategory::NotFound) {
            fail_(tr("The bucket “%1” is gone — the server may have deleted it instead of %2. %3")
                      .arg(bucket(), kindText(m_kind), readBack));
            return;
        }
        controller_()->capabilities()->recheck(account().id, bucket());
        if (phase() != Phase::Verifying) {   // the write itself failed
            fail_(readBack + QLatin1Char(' ') + tr("The bucket is still there."));
            return;
        }
        Report rep;
        rep.succeeded = 1;
        rep.verified = r && readBackOk;
        rep.verification = readBack + (r ? QString() : tr("; the bucket could not be looked up: %1")
                                                             .arg(ConnectionTester::describe(r.error())));
        finish_(rep);
    });
}

} // namespace qbucket
