#include "controllers/ObjectLockJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"

#include <QLocale>

namespace qbucket {

namespace {

QString modeText(s3::ObjectLockMode mode)
{
    return s3::objectLockModeName(mode);
}

} // namespace

ObjectLockJob::ObjectLockJob(const Account& account, const QString& bucket, const QString& key,
                             const QString& versionId, Change change, QObject* parent)
    : Job(account, bucket, parent),
      m_key(key),
      m_versionId(versionId),
      m_change(change)
{
}

ObjectLockJob* ObjectLockJob::retention(const Account& account, const QString& bucket, const QString& key,
                                        const QString& versionId, s3::ObjectLockMode mode, const QDateTime& retainUntil,
                                        QObject* parent)
{
    auto* job = new ObjectLockJob(account, bucket, key, versionId, Change::Retention, parent);
    job->m_mode = mode;
    job->m_until = retainUntil.toUTC();
    return job;
}

ObjectLockJob* ObjectLockJob::removeRetention(const Account& account, const QString& bucket, const QString& key,
                                              const QString& versionId, QObject* parent)
{
    return new ObjectLockJob(account, bucket, key, versionId, Change::RemoveRetention, parent);
}

ObjectLockJob* ObjectLockJob::legalHold(const Account& account, const QString& bucket, const QString& key,
                                        const QString& versionId, bool on, QObject* parent)
{
    auto* job = new ObjectLockJob(account, bucket, key, versionId, Change::LegalHold, parent);
    job->m_on = on;
    return job;
}

QString ObjectLockJob::title() const
{
    const QString name = m_key.mid(m_key.lastIndexOf(u'/') + 1);
    switch (m_change) {
    case Change::Retention: return tr("Set the retention of “%1”").arg(name);
    case Change::RemoveRetention: return tr("Remove the retention of “%1”").arg(name);
    case Change::LegalHold:
        return m_on ? tr("Put “%1” under legal hold").arg(name) : tr("Lift the legal hold of “%1”").arg(name);
    }
    return {};
}

QList<s3::Feature> ObjectLockJob::requiredFeatures() const
{
    return {m_change == Change::LegalHold ? s3::Feature::LegalHoldWrite : s3::Feature::RetentionWrite};
}

QString ObjectLockJob::describe_(const s3::ObjectRetention& r) const
{
    if (!r.mode)
        return tr("no retention");
    return tr("%1 until %2").arg(modeText(*r.mode), QLocale().toString(r.retainUntil.toLocalTime(), QLocale::ShortFormat));
}

// --- plan ------------------------------------------------------------------------

void ObjectLockJob::plan()
{
    m_run = std::make_unique<QObject>();
    if (m_change == Change::Retention && (!m_until.isValid() || m_until <= QDateTime::currentDateTimeUtc())) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("A retention lasts until a moment in the future.")); },
                                  Qt::QueuedConnection);
        return;
    }
    s3::Session* session = session_();
    if (!session) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Not connected.")); }, Qt::QueuedConnection);
        return;
    }
    session->headObject(bucket(), m_key, m_versionId, m_run.get(), [this](const s3::Result<s3::ObjectMeta>& meta) {
        if (!meta) {
            fail_(tr("“%1” could not be read: %2").arg(m_key, ConnectionTester::describe(meta.error())));
            return;
        }
        s3::Session* s = session_();
        if (!s) {
            fail_(tr("Not connected."));
            return;
        }
        // Pinned to the version HEAD found: the one shown is the one changed.
        const QString version = m_versionId.isEmpty() ? meta->versionId : m_versionId;
        s->getObjectRetention(bucket(), m_key, version, m_run.get(),
                              [this, meta = *meta, version](const s3::Result<s3::ObjectRetention>& r) {
            if (!r) {
                fail_(tr("The retention could not be read: %1").arg(ConnectionTester::describe(r.error())));
                return;
            }
            s3::Session* s2 = session_();
            if (!s2) {
                fail_(tr("Not connected."));
                return;
            }
            s2->getObjectLegalHold(bucket(), m_key, version, m_run.get(),
                                   [this, meta, current = *r, version](const s3::Result<bool>& held) {
                if (!held) {
                    fail_(tr("The legal hold could not be read: %1").arg(ConnectionTester::describe(held.error())));
                    return;
                }
                s3::ObjectMeta pinned = meta;
                pinned.versionId = version;
                planWith_(pinned, current, *held);
            });
        });
    });
}

void ObjectLockJob::planWith_(const s3::ObjectMeta& meta, const s3::ObjectRetention& current, bool held)
{
    m_planned = meta;
    Preview p;
    p.objects = 1;
    p.scope = QStringLiteral("s3://%1/%2").arg(bucket(), m_key);
    if (!meta.versionId.isEmpty() && meta.versionId != QLatin1String("null"))
        p.details.append(tr("Version %1.").arg(meta.versionId));
    p.details.append(tr("Now: %1; legal hold %2.").arg(describe_(current), held ? tr("on") : tr("off")));
    const bool active = current.mode && current.retainUntil > QDateTime::currentDateTimeUtc();
    const bool compliance = active && current.mode == s3::ObjectLockMode::Compliance;

    if (m_change == Change::LegalHold) {
        if (held == m_on) {
            finish_(Report{0, 1, 0, {}, m_on ? tr("the legal hold is on already") : tr("there is no legal hold"), true, {}});
            return;
        }
        p.action = m_on ? tr("Put Under Legal Hold") : tr("Lift Legal Hold");
        p.details.append(m_on ? tr("Under legal hold the version can't be deleted or overwritten — whatever its "
                                   "retention — until the hold is lifted.")
                              : tr("Without the hold, only its retention (if any) protects the version."));
        setPreview_(p);
        return;
    }

    if (m_change == Change::RemoveRetention) {
        if (!current.mode) {
            finish_(Report{0, 1, 0, {}, tr("there is no retention"), true, {}});
            return;
        }
        if (compliance) {
            fail_(tr("A COMPLIANCE retention can't be removed — by anyone — until %1.")
                      .arg(QLocale().toString(current.retainUntil.toLocalTime(), QLocale::ShortFormat)));
            return;
        }
        p.action = tr("Remove Retention");
        m_needsBypass = active;
    } else {
        const bool shorter = active && m_until < current.retainUntil;
        if (compliance && (shorter || m_mode != s3::ObjectLockMode::Compliance)) {
            fail_(tr("A COMPLIANCE retention can only be extended, by nobody shortened or weakened."));
            return;
        }
        if (current.mode == m_mode && current.retainUntil == m_until) {
            finish_(Report{0, 1, 0, {}, tr("the retention is set like that already"), true, {}});
            return;
        }
        p.action = tr("Set Retention");
        p.details.append(tr("New: %1.").arg(describe_({m_mode, m_until})));
        m_needsBypass = active && current.mode == s3::ObjectLockMode::Governance && shorter;
        if (m_mode == s3::ObjectLockMode::Compliance && !compliance) {
            p.warnings.append(tr("COMPLIANCE: nobody — not even the account owner — can delete this version, shorten "
                                 "or remove the retention until it has run out."));
            p.typeToConfirm = m_key.mid(m_key.lastIndexOf(u'/') + 1);
        }
    }
    if (m_needsBypass) {
        p.bypassOffer = true;
        p.warnings.append(tr("This shortens or removes a GOVERNANCE retention: it needs the governance bypass "
                             "below. Without it the server refuses."));
    }
    setPreview_(p);
}

// --- execute -------------------------------------------------------------------------

void ObjectLockJob::execute()
{
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    if (m_needsBypass && !bypassGovernance()) {
        fail_(tr("Shortening or removing a GOVERNANCE retention needs the bypass — nothing was sent."));
        return;
    }
    auto done = [this](const s3::Result<s3::SubresourceWriteResult>& r) {
        const s3::Operation op =
            m_change == Change::LegalHold ? s3::Operation::PutObjectLegalHold : s3::Operation::PutObjectRetention;
        Journal::Record rec = record_(s3::operationName(op), m_key);
        rec.versionId = m_planned.versionId;
        rec.bypassGovernance = m_needsBypass && bypassGovernance();
        switch (m_change) {
        case Change::LegalHold: rec.message = m_on ? QStringLiteral("legal hold ON") : QStringLiteral("legal hold OFF"); break;
        case Change::RemoveRetention: rec.message = QStringLiteral("retention removed"); break;
        case Change::Retention:
            rec.message = QStringLiteral("retention %1 until %2")
                              .arg(s3::objectLockModeName(m_mode), m_until.toString(Qt::ISODate));
            break;
        }
        if (!r) {
            const s3::Error& e = r.error();
            rec.result = Journal::Result::Failed;
            rec.httpStatus = e.httpStatus;
            rec.requestId = e.requestId;
            rec.message += QStringLiteral(": ") + e.toString();
            journal_({rec});
            fail_(e.category == s3::ErrorCategory::NotPermitted
                      ? tr("The server refused: protected by object lock, or not permitted (%1)").arg(ConnectionTester::describe(e))
                      : tr("The server refused: %1").arg(ConnectionTester::describe(e)));
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        journal_({rec});
        executed_();
    };
    if (m_change == Change::LegalHold) {
        session->putObjectLegalHold({bucket(), m_key, m_planned.versionId, m_on}, m_run.get(), std::move(done));
        return;
    }
    s3::PutObjectRetentionParams p;
    p.bucket = bucket();
    p.key = m_key;
    p.versionId = m_planned.versionId;
    if (m_change == Change::Retention) {
        p.mode = m_mode;
        p.retainUntil = m_until;
    }
    p.bypassGovernanceRetention = m_needsBypass && bypassGovernance();
    session->putObjectRetention(p, m_run.get(), std::move(done));
}

// --- verify ---------------------------------------------------------------------------

void ObjectLockJob::verify()
{
    readBack_();
}

void ObjectLockJob::readBack_()
{
    s3::Session* session = session_();
    if (!session) {
        finish_(Report{1, 0, 0, {}, tr("not checked (not connected)"), false, {}});
        return;
    }
    session->headObject(bucket(), m_key, m_planned.versionId, m_run.get(), [this](const s3::Result<s3::ObjectMeta>& meta) {
        const bool same = meta && meta->size == m_planned.size && meta->etag == m_planned.etag;
        const auto report = [this, same](bool ok, const QString& value) {
            Report r;
            r.succeeded = 1;
            r.verified = ok && same;
            r.verification = same ? value : tr("%1 — but the object reads differently now").arg(value);
            finish_(r);
        };
        s3::Session* s = session_();
        if (!s) {
            report(false, tr("not checked (not connected)"));
            return;
        }
        if (m_change == Change::LegalHold) {
            s->getObjectLegalHold(bucket(), m_key, m_planned.versionId, m_run.get(), [this, report](const s3::Result<bool>& held) {
                const bool ok = held && *held == m_on;
                report(ok, ok ? (m_on ? tr("read back: under legal hold") : tr("read back: no legal hold"))
                              : tr("the legal hold doesn't read back as set"));
            });
            return;
        }
        s->getObjectRetention(bucket(), m_key, m_planned.versionId, m_run.get(),
                              [this, report](const s3::Result<s3::ObjectRetention>& r) {
            bool ok = false;
            if (r && m_change == Change::RemoveRetention)
                ok = !r->mode;
            else if (r)   // servers keep seconds or fractions of them
                ok = r->mode == m_mode && std::abs(r->retainUntil.secsTo(m_until)) <= 1;
            report(ok, ok ? (r->mode ? tr("read back: %1").arg(describe_(*r)) : tr("read back: no retention"))
                          : tr("the retention doesn't read back as set"));
        });
    });
}

void ObjectLockJob::onCancel()
{
    // A write under way is answered and journaled; only planning stops.
    if (phase() == Phase::Planning)
        m_run.reset();
}

} // namespace qbucket
