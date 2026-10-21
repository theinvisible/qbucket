#include "controllers/EditObjectsJob.h"

#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/TransferService.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/ProviderRegistry.h>
#include <qbucket/s3/transfer/Copier.h>

#include <QLocale>
#include <QTimer>

#include <algorithm>

namespace qbucket {

namespace {

bool isPrintableAscii(const QString& text)
{
    return std::all_of(text.begin(), text.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() <= 0x7E; });
}

bool headersAreAscii(const s3::ObjectHeaders& h)
{
    for (const QString* v : {&h.contentType, &h.cacheControl, &h.contentDisposition, &h.contentEncoding,
                             &h.contentLanguage, &h.expires, &h.websiteRedirectLocation}) {
        if (!isPrintableAscii(*v))
            return false;
    }
    for (auto it = h.userMetadata.cbegin(); it != h.userMetadata.cend(); ++it) {
        if (!isPrintableAscii(it.key()) || !isPrintableAscii(it.value()))
            return false;
    }
    return true;
}

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

// --- ChangeSpec -----------------------------------------------------------------

bool EditObjectsJob::ChangeSpec::isEmpty() const
{
    return !contentType && !cacheControl && !contentDisposition && !contentEncoding && !contentLanguage && !expires
        && !websiteRedirectLocation && metadata.isEmpty() && !storageClass;
}

s3::ObjectHeaders EditObjectsJob::ChangeSpec::applyTo(const s3::ObjectHeaders& headers) const
{
    s3::ObjectHeaders h = headers;
    const auto set = [](QString& field, const std::optional<QString>& value) {
        if (value)
            field = *value;
    };
    set(h.contentType, contentType);
    set(h.cacheControl, cacheControl);
    set(h.contentDisposition, contentDisposition);
    set(h.contentEncoding, contentEncoding);
    set(h.contentLanguage, contentLanguage);
    set(h.expires, expires);
    set(h.websiteRedirectLocation, websiteRedirectLocation);
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
        if (const std::optional<QString>& value = it.value())
            h.userMetadata.insert(it.key().toLower(), *value);
        else
            h.userMetadata.remove(it.key().toLower());
    }
    return h;
}

QStringList EditObjectsJob::ChangeSpec::describe() const
{
    QStringList out;
    const auto line = [&out](const QString& name, const std::optional<QString>& value) {
        if (!value)
            return;
        out.append(value->isEmpty() ? tr("%1: removed").arg(name) : tr("%1: %2").arg(name, *value));
    };
    line(QStringLiteral("Content-Type"), contentType);
    line(QStringLiteral("Cache-Control"), cacheControl);
    line(QStringLiteral("Content-Disposition"), contentDisposition);
    line(QStringLiteral("Content-Encoding"), contentEncoding);
    line(QStringLiteral("Content-Language"), contentLanguage);
    line(QStringLiteral("Expires"), expires);
    line(QStringLiteral("Website redirect"), websiteRedirectLocation);
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
        line(QStringLiteral("x-amz-meta-") + it.key(), it.value() ? it.value() : std::optional<QString>(QString()));
    if (storageClass)
        out.append(tr("Storage class: %1").arg(storageClass->isEmpty() ? QStringLiteral("STANDARD") : *storageClass));
    return out;
}

EditObjectsJob::ChangeSpec EditObjectsJob::ChangeSpec::between(const s3::ObjectHeaders& before, const s3::ObjectHeaders& after)
{
    ChangeSpec c;
    const auto diff = [](const QString& a, const QString& b) { return a == b ? std::nullopt : std::optional<QString>(b); };
    c.contentType = diff(before.contentType, after.contentType);
    c.cacheControl = diff(before.cacheControl, after.cacheControl);
    c.contentDisposition = diff(before.contentDisposition, after.contentDisposition);
    c.contentEncoding = diff(before.contentEncoding, after.contentEncoding);
    c.contentLanguage = diff(before.contentLanguage, after.contentLanguage);
    c.expires = diff(before.expires, after.expires);
    c.websiteRedirectLocation = diff(before.websiteRedirectLocation, after.websiteRedirectLocation);
    for (auto it = after.userMetadata.cbegin(); it != after.userMetadata.cend(); ++it) {
        if (before.userMetadata.value(it.key()) != it.value() || !before.userMetadata.contains(it.key()))
            c.metadata.insert(it.key(), it.value());
    }
    for (auto it = before.userMetadata.cbegin(); it != before.userMetadata.cend(); ++it) {
        if (!after.userMetadata.contains(it.key()))
            c.metadata.insert(it.key(), std::nullopt);
    }
    return c;
}

// --- the job --------------------------------------------------------------------------

struct EditObjectsJob::Active {
    Entry entry;
    ObjectSnapshot snap;
    s3::ObjectHeaders headers;   // what the copy carries
    QString targetClass;
    QString canned;              // the ACL sent along (non-private)
    bool inParts = false;
    bool rechecked = false;
};

EditObjectsJob::EditObjectsJob(const Account& account, const QString& bucket, const QList<Item>& items,
                               ChangeSpec change, const QString& planDir, QObject* parent)
    : Job(account, bucket, parent),
      m_items(items),
      m_change(std::move(change)),
      m_planDir(planDir)
{
}

EditObjectsJob::~EditObjectsJob() = default;

QString EditObjectsJob::title() const
{
    const QString what = m_items.size() == 1 ? m_items.first().key : tr("%n items", nullptr, int(m_items.size()));
    return m_change.headersOnly() ? tr("Edit the headers of “%1”").arg(what)
                                  : tr("Change the storage class of “%1”").arg(what);
}

QList<s3::Feature> EditObjectsJob::requiredFeatures() const
{
    if (m_large)
        return {s3::Feature::MultipartUpload};
    return {};
}

// --- plan -----------------------------------------------------------------------------

void EditObjectsJob::plan()
{
    if (m_change.isEmpty() || m_items.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { fail_(tr("Nothing to change.")); }, Qt::QueuedConnection);
        return;
    }
    m_plan = std::make_unique<PlanFile>(m_planDir + u'/' + id() + QStringLiteral(".plan"));
    m_run = std::make_unique<QObject>();
    controller_()->capabilities()->ensureFresh(account().id, bucket(), 600, m_run.get(),
                                               [this](bool) { checkCapabilities_(); });
}

// Tags and ACLs a copy would lose must be readable — or not exist at all.
// Not probed yet (Unknown): each object's read decides ("not implemented":
// it has none; any other failure leaves that object alone).
void EditObjectsJob::checkCapabilities_()
{
    CapabilityController* caps = controller_()->capabilities();
    const auto state = [&](s3::Feature f) { return caps->entry(account().id, bucket(), f).state; };
    const s3::CapabilityState tags = state(s3::Feature::ObjectTagging);
    const s3::CapabilityState acl = state(s3::Feature::ObjectAcl);
    if (tags == s3::CapabilityState::NotPermitted) {
        fail_(tr("These credentials may not read the objects' tags: an edit could lose them. Nothing was changed."));
        return;
    }
    if (acl == s3::CapabilityState::NotPermitted) {
        fail_(tr("These credentials may not read the objects' permissions: an edit could make public objects "
                 "private. Nothing was changed."));
        return;
    }
    m_readTags = tags != s3::CapabilityState::Unsupported;
    m_readAcl = acl != s3::CapabilityState::Unsupported;
    m_writeTags = caps->decide(account().id, bucket(), s3::Feature::ObjectTaggingWrite, true) == s3::Decision::Allowed;
    m_writeAcl = caps->decide(account().id, bucket(), s3::Feature::ObjectAclWrite, true) == s3::Decision::Allowed;
    nextItem_();
}

void EditObjectsJob::nextItem_()
{
    if (phase() != Phase::Planning)
        return;
    if (m_itemIndex >= m_items.size()) {
        planDone_();
        return;
    }
    const Item item = m_items[m_itemIndex++];
    s3::Session* session = session_();
    if (!session) {
        fail_(tr("Not connected."));
        return;
    }
    if (item.folder) {
        s3::ListObjectsParams params;
        params.bucket = bucket();
        params.prefix = item.key;
        params.maxKeys = 1000;
        m_pager = std::make_unique<s3::ListObjectsPager>(*session, params);
        fetchPage_();
        return;
    }
    session->headObject(bucket(), item.key, QString(), m_run.get(), [this, item](const s3::Result<s3::ObjectMeta>& head) {
        if (phase() != Phase::Planning)
            return;
        if (!head) {
            fail_(tr("Could not look up “%1”: %2").arg(item.key, ConnectionTester::describe(head.error())));
            return;
        }
        m_large = m_large || head->size > kCopyLimit;
        m_bytes += head->size;
        if (m_samples.size() < kSamples)
            m_samples.append(item.key);
        m_plan->appendRecords({{item.key, QString::number(head->size), head->etag}});
        nextItem_();
    });
}

void EditObjectsJob::fetchPage_()
{
    m_pager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) {
        if (phase() != Phase::Planning)
            return;
        if (!page) {
            fail_(tr("Could not list the folder: %1").arg(ConnectionTester::describe(page.error())));
            return;
        }
        QList<QStringList> records;
        for (const s3::ObjectEntry& e : page->objects) {
            if (e.key.endsWith(u'/'))
                continue;   // a folder marker: nothing to edit
            m_large = m_large || e.size > kCopyLimit;
            m_bytes += e.size;
            if (m_samples.size() < kSamples)
                m_samples.append(e.key);
            records.append({e.key, QString::number(e.size), e.etag});
        }
        m_plan->appendRecords(records);
        setProgress_(m_plan->count(), 0);
        if (m_pager->hasMore()) {
            fetchPage_();
            return;
        }
        m_pager.reset();
        nextItem_();
    });
}

void EditObjectsJob::planDone_()
{
    m_plan->flush([this](bool ok) {
        if (phase() != Phase::Planning)
            return;
        if (!ok) {
            fail_(tr("The plan could not be written: %1").arg(m_plan->errorText()));
            return;
        }
        if (m_plan->count() == 0) {
            finish_(report_());
            return;
        }
        Preview p;
        p.action = m_change.headersOnly() ? tr("Edit") : tr("Change Class");
        p.scope = m_items.size() == 1 ? QStringLiteral("s3://%1/%2").arg(bucket(), m_items.first().key)
                                      : tr("%n items in s3://%1", nullptr, int(m_items.size())).arg(bucket());
        p.objects = m_plan->count();
        p.bytes = m_bytes;
        p.samples = m_samples;
        p.details = m_change.describe();
        p.details.append(tr("Each object is copied onto itself with the new headers; its tags and permissions are "
                            "carried over and everything is checked afterwards."));
        const std::optional<s3::CapabilitySet> state = controller_()->capabilities()->bucketSet(account().id, bucket());
        if (state && state->bucketState.versioning == QLatin1String("Enabled")) {
            p.details.append(tr("The bucket keeps versions: each object gets a new version, the previous ones stay."));
        } else {
            p.permanent = true;
            p.warnings.append(tr("The objects are rewritten in place: their previous headers are gone and their "
                                 "modification time changes."));
        }
        if (state && state->bucketState.objectLockEnabled.value_or(false)) {
            p.warnings.append(tr("Objects under retention or a legal hold are skipped: the new version would not be "
                                 "protected."));
        }
        if (m_readTags && !m_writeTags)
            p.warnings.append(tr("Objects with tags are skipped: writing tags isn't confirmed for this server."));
        if (m_readAcl && !m_writeAcl)
            p.warnings.append(tr("Public objects are skipped: changing ACLs isn't confirmed for this server."));
        const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(account().providerId);
        if (m_change.storageClass && profile->isArchiveStorageClass(*m_change.storageClass)) {
            p.warnings.append(tr("%1 is an archive class: the objects can't be read until they are restored (hours), "
                                 "and a minimum storage time is charged.").arg(*m_change.storageClass));
        }
        setProgress_(0, p.objects);
        setPreview_(p);
    });
}

// --- execute -----------------------------------------------------------------------

void EditObjectsJob::execute()
{
    m_plan->rewind();
    m_eof = false;
    setProgress_(0, m_plan->count());
    pump_();
}

void EditObjectsJob::pump_()
{
    if (isCancelled() || m_reading || m_eof || m_inFlight >= kParallel) {
        settled_();
        return;
    }
    m_reading = true;
    m_plan->readNextRecords(1, [this](const QList<QStringList>& records) {
        m_reading = false;
        if (phase() != Phase::Executing)
            return;
        if (records.isEmpty()) {
            m_eof = true;
            settled_();
            return;
        }
        if (isCancelled()) {
            settled_();
            return;
        }
        const QStringList& r = records.first();
        ++m_inFlight;
        start_({r.value(0), r.value(1).toLongLong(), r.value(2)});
        pump_();
    });
}

void EditObjectsJob::start_(const Entry& e)
{
    s3::Session* session = session_();
    auto a = std::make_shared<Active>();
    a->entry = e;
    if (!session) {
        done_(a, tr("not changed (not connected)"));
        return;
    }
    ObjectSnapshot::read(session, bucket(), e.key, m_readTags, m_readAcl, m_run.get(), [this, a](s3::Result<ObjectSnapshot> snap) {
        if (!snap) {
            if (snap.error().category == s3::ErrorCategory::NotFound) {
                --m_inFlight;
                skipped_(a->entry.key, tr("deleted since it was planned"));
                return;
            }
            // Tags / permissions that can't be read could be lost: not changed.
            done_(a, tr("not changed: its current state could not be read completely (%1)")
                         .arg(ConnectionTester::describe(snap.error())));
            return;
        }
        a->snap = *snap;
        snapshotTaken_(a);
    });
}

QString EditObjectsJob::cannotEdit_(const ObjectSnapshot& s) const
{
    const s3::ObjectMeta& m = s.meta;
    if (!m.sseCustomerAlgorithm.isEmpty())
        return tr("encrypted with a customer key (SSE-C): QBucket has no key to copy it");
    if (m.serverSideEncryption.startsWith(QLatin1String("aws:kms")))
        return tr("encrypted with KMS: not carried over by QBucket yet");
    if (!m.objectLockMode.isEmpty() && m.objectLockRetainUntil > QDateTime::currentDateTimeUtc())
        return tr("under retention until %1: the new version would not be protected")
            .arg(QLocale().toString(m.objectLockRetainUntil.toLocalTime(), QLocale::ShortFormat));
    if (m.legalHold)
        return tr("under a legal hold: the new version would not be protected");
    const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(account().providerId);
    if (profile->isArchiveStorageClass(m.storageClass))
        return tr("in the archive class %1: restore it first").arg(m.storageClass);
    if (s.acl) {
        const std::optional<QString> canned = s3::cannedAclOf(*s.acl);
        if (!canned)
            return tr("has permissions for particular accounts that a copy would lose");
        if (*canned != QLatin1String("private") && !m_writeAcl)
            return tr("is %1, and changing ACLs isn't confirmed for this server").arg(*canned);
    }
    const bool tagged = s.tags ? !s.tags->isEmpty() : m.tagCount > 0;
    if (tagged && (!m_writeTags || !s.tags))
        return tr("has tags, and writing tags isn't confirmed for this server");
    if (!headersAreAscii(s3::objectHeadersOf(m)))
        return tr("has headers or metadata that aren't plain ASCII and can't be sent back unchanged");
    return {};
}

void EditObjectsJob::snapshotTaken_(const std::shared_ptr<Active>& a)
{
    const s3::ObjectMeta& m = a->snap.meta;
    if (m.etag != a->entry.etag) {
        --m_inFlight;
        skipped_(a->entry.key, tr("changed since it was planned"));
        return;
    }
    if (const QString why = cannotEdit_(a->snap); !why.isEmpty()) {
        --m_inFlight;
        skipped_(a->entry.key, why);
        return;
    }
    const s3::ObjectHeaders current = s3::objectHeadersOf(m);
    a->headers = m_change.applyTo(current);
    a->targetClass = m_change.storageClass.value_or(m.storageClass);
    if (a->headers == current && sameClass(a->targetClass, m.storageClass)) {
        --m_inFlight;
        skipped_(a->entry.key, tr("already as asked"));
        return;
    }
    if (const std::optional<s3::AccessControlPolicy>& acl = a->snap.acl) {
        const QString canned = s3::cannedAclOf(*acl).value_or(QString());
        if (canned != QLatin1String("private"))
            a->canned = canned;
    }
    s3::Session* session = session_();
    if (!session) {
        done_(a, tr("not changed (not connected)"));
        return;
    }
    const QString sse = m.serverSideEncryption == QLatin1String("AES256") ? m.serverSideEncryption : QString();
    if (m.size > kCopyLimit) {
        TransferService* transfers = controller_()->transfers();
        if (!transfers) {
            done_(a, tr("not changed (no transfer service)"));
            return;
        }
        a->inParts = true;
        s3::transfer::CopyRequest req;
        req.sourceBucket = req.bucket = bucket();
        req.sourceKey = req.key = a->entry.key;
        req.ifMatch = m.etag;
        req.storageClass = classToSend(a->targetClass);
        req.replaceHeaders = a->headers;
        req.serverSideEncryption = sse;
        req.cannedAcl = a->canned;
        auto copier = std::make_unique<s3::transfer::Copier>(session, req, transfers->limiter(account().id));
        s3::transfer::Copier* raw = copier.get();
        connect(raw, &s3::transfer::Copier::finished, this, [this, raw, a] {
            const s3::Result<s3::transfer::CopyResult>& r = raw->result();
            Journal::Record rec = record_(QStringLiteral("MultipartCopy"), a->entry.key);
            rec.message = QStringLiteral("edit in place");
            if (r) {
                rec.httpStatus = 200;
                rec.versionId = r->versionId;
                journal_({rec});
                copied_(a);
            } else {
                rec.result = Journal::Result::Failed;
                rec.httpStatus = r.error().httpStatus;
                rec.message += QStringLiteral(": ") + r.error().toString();
                journal_({rec});
                done_(a, r.error().category == s3::ErrorCategory::PreconditionFailed
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
        return;
    }
    s3::CopyObjectParams p;
    p.sourceBucket = p.bucket = bucket();
    p.sourceKey = p.key = a->entry.key;
    p.ifMatch = m.etag;
    p.metadataDirective = s3::MetadataDirective::Replace;
    p.headers = a->headers;
    p.storageClass = classToSend(a->targetClass);
    p.serverSideEncryption = sse;
    p.cannedAcl = a->canned;
    session->copyObject(p, m_run.get(), [this, a](const s3::Result<s3::CopyObjectResult>& r) {
        Journal::Record rec = record_(s3::operationName(s3::Operation::CopyObject), a->entry.key);
        rec.message = QStringLiteral("edit in place");
        if (!r) {
            const s3::Error& err = r.error();
            rec.result = err.outcomeUnknown ? Journal::Result::Unknown : Journal::Result::Failed;
            rec.httpStatus = err.httpStatus;
            rec.requestId = err.requestId;
            rec.message += QStringLiteral(": ") + err.toString();
            journal_({rec});
            done_(a, err.category == s3::ErrorCategory::PreconditionFailed
                         ? tr("changed meanwhile — left alone")
                         : tr("not changed: %1").arg(ConnectionTester::describe(err)));
            return;
        }
        rec.httpStatus = 200;
        rec.requestId = r->requestId;
        rec.versionId = r->versionId;
        journal_({rec});
        copied_(a);
    });
}

void EditObjectsJob::copied_(const std::shared_ptr<Active>& a)
{
    if (!a->snap.tags.value_or(s3::TagSet()).isEmpty()) {
        restoreTags_(a);
        return;
    }
    check_(a);
}

// RGW drops an object's tags on such a copy: put them back (and only then
// check everything).
void EditObjectsJob::restoreTags_(const std::shared_ptr<Active>& a)
{
    s3::Session* session = session_();
    if (!session) {
        done_(a, tr("changed, but its tags could not be checked (not connected)"));
        return;
    }
    session->getObjectTagging(bucket(), a->entry.key, QString(), m_run.get(), [this, a](const s3::Result<s3::ObjectTagging>& now) {
        if (now && sameTags(now->tags, *a->snap.tags)) {
            check_(a);
            return;
        }
        s3::Session* s = session_();
        if (!s) {
            done_(a, tr("changed, but its tags could not be written back (not connected)"));
            return;
        }
        s3::PutObjectTaggingParams p;
        p.bucket = bucket();
        p.key = a->entry.key;
        p.tags = *a->snap.tags;
        s->putObjectTagging(p, m_run.get(), [this, a](const s3::Result<s3::SubresourceWriteResult>& r) {
            Journal::Record rec = record_(s3::operationName(s3::Operation::PutObjectTagging), a->entry.key);
            rec.message = QStringLiteral("tags carried over the edit");
            if (!r) {
                rec.result = Journal::Result::Failed;
                rec.httpStatus = r.error().httpStatus;
                rec.message += QStringLiteral(": ") + r.error().toString();
                journal_({rec});
                QStringList tags;
                for (const s3::Tag& t : *a->snap.tags)
                    tags.append(t.key + u'=' + t.value);
                done_(a, tr("changed, but its tags could not be written back (%1): they were %2")
                             .arg(ConnectionTester::describe(r.error()), tags.join(QStringLiteral(", "))));
                return;
            }
            rec.httpStatus = 200;
            rec.requestId = r->requestId;
            journal_({rec});
            check_(a);
        });
    });
}

void EditObjectsJob::check_(const std::shared_ptr<Active>& a)
{
    s3::Session* session = session_();
    if (!session) {
        done_(a, tr("changed, but not checked (not connected)"));
        return;
    }
    const bool tags = !a->snap.tags.value_or(s3::TagSet()).isEmpty();
    const bool acl = !a->canned.isEmpty();
    ObjectSnapshot::read(session, bucket(), a->entry.key, tags, acl, m_run.get(), [this, a, tags, acl](s3::Result<ObjectSnapshot> now) {
        QStringList wrong;
        if (!now) {
            wrong.append(ConnectionTester::describe(now.error()));
        } else {
            const s3::ObjectMeta& m = now->meta;
            if (m.size != a->snap.meta.size)
                wrong.append(tr("size"));
            const QString& before = a->snap.meta.etag;
            if (!a->inParts && !before.contains(u'-') && m.etag != before)
                wrong.append(tr("content (ETag)"));
            wrong += headerDifferences(a->headers, s3::objectHeadersOf(m));
            if (!sameClass(m.storageClass, a->targetClass))
                wrong.append(tr("storage class (%1)").arg(m.storageClass.isEmpty() ? QStringLiteral("STANDARD") : m.storageClass));
            if (tags && (!now->tags || !sameTags(*now->tags, *a->snap.tags)))
                wrong.append(tr("tags"));
            if (acl && (!now->acl || s3::cannedAclOf(*now->acl) != std::optional<QString>(a->canned)))
                wrong.append(tr("permissions"));
        }
        if (wrong.isEmpty()) {
            done_(a, {});
            return;
        }
        if (!a->rechecked) {
            // Reads may lag behind a write for a moment.
            a->rechecked = true;
            QTimer::singleShot(m_recheckMs, m_run.get(), [this, a] { check_(a); });
            return;
        }
        done_(a, tr("changed, but it doesn't match: %1").arg(wrong.join(QStringLiteral(", "))));
    });
}

void EditObjectsJob::done_(const std::shared_ptr<Active>& a, const QString& failure)
{
    --m_inFlight;
    if (failure.isEmpty()) {
        ++m_succeeded;
    } else {
        m_failedKeys.append(a->entry.key);
        if (m_failures.size() < kMaxListed)
            m_failures.append(QStringLiteral("%1: %2").arg(a->entry.key, failure));
    }
    setProgress_(progressDone() + 1, progressTotal());
    pump_();
}

void EditObjectsJob::skipped_(const QString& key, const QString& why)
{
    ++m_skipped;
    if (m_notes.size() < kMaxListed)
        m_notes.append(QStringLiteral("%1: %2").arg(key, why));
    setProgress_(progressDone() + 1, progressTotal());
    pump_();
}

void EditObjectsJob::settled_()
{
    if (phase() != Phase::Executing || m_inFlight > 0 || m_reading)
        return;
    if (isCancelled()) {
        finish_(report_());
        return;
    }
    if (m_eof)
        executed_();
}

void EditObjectsJob::onCancel()
{
    switch (phase()) {
    case Phase::Planning:
        m_run.reset();
        m_pager.reset();
        break;
    case Phase::Executing:
        for (const auto& c : m_copiers)
            c->cancel();
        settled_();
        break;
    default:
        break;
    }
}

// Every object was checked on its own.
void EditObjectsJob::verify()
{
    finish_(report_());
}

Job::Report EditObjectsJob::report_() const
{
    Report r;
    r.succeeded = m_succeeded;
    r.skipped = m_skipped;
    r.failed = m_failedKeys.size();
    r.failures = m_failures;
    r.notes = m_notes;
    r.verified = m_failedKeys.isEmpty() && m_succeeded > 0;
    r.verification = m_succeeded > 0 ? tr("every changed object checked (headers, class, tags, permissions)") : QString();
    return r;
}

} // namespace qbucket
