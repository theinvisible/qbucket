#include "controllers/VersionWalk.h"

namespace qbucket {

namespace {

s3::ListObjectVersionsParams paramsFor(const QString& bucket, const QString& prefix)
{
    s3::ListObjectVersionsParams p;
    p.bucket = bucket;
    p.prefix = prefix;   // flat: everything below
    p.maxKeys = 1000;
    return p;
}

s3::ListObjectsParams objectParamsFor(const QString& bucket, const QString& prefix)
{
    s3::ListObjectsParams p;
    p.bucket = bucket;
    p.prefix = prefix;
    p.maxKeys = 1000;
    return p;
}

} // namespace

VersionWalk::VersionWalk(s3::Session& session, const QString& bucket, const QString& prefixOrKey, bool exactKey,
                         QObject* context, Source source)
    : m_prefix(prefixOrKey),
      m_exact(exactKey),
      m_context(context)
{
    if (source == Source::CurrentOnly)
        m_objects = std::make_unique<s3::ListObjectsPager>(session, objectParamsFor(bucket, prefixOrKey));
    else
        m_versions = std::make_unique<s3::ListObjectVersionsPager>(session, paramsFor(bucket, prefixOrKey));
}

VersionWalk::~VersionWalk() = default;

void VersionWalk::run(Page page, Failed failed)
{
    m_page = std::move(page);
    m_failed = std::move(failed);
    next_();
}

void VersionWalk::next_()
{
    const std::weak_ptr<int> alive = m_alive;
    if (m_objects) {
        m_objects->fetchNext(m_context, [this, alive](const s3::Result<s3::ListPage>& r) {
            if (alive.expired())
                return;
            if (!r) {
                const Failed failed = m_failed;   // a copy: the callee may destroy this walk
                failed(r.error());
                return;
            }
            QList<s3::VersionEntry> entries;
            entries.reserve(r->objects.size());
            for (const s3::ObjectEntry& o : r->objects)
                entries.append({o.key, QString(), true, false, o.size, o.lastModified, o.etag, o.storageClass});
            deliver_(std::move(entries), !m_objects->hasMore());
        });
        return;
    }
    m_versions->fetchNext(m_context, [this, alive](const s3::Result<s3::ListObjectVersionsPage>& r) {
        if (alive.expired())
            return;
        if (!r) {
            const Failed failed = m_failed;
            failed(r.error());
            return;
        }
        deliver_(r->entries, !m_versions->hasMore());
    });
}

void VersionWalk::deliver_(QList<s3::VersionEntry> entries, bool last)
{
    if (m_exact) {
        QList<s3::VersionEntry> mine;
        for (const s3::VersionEntry& e : std::as_const(entries)) {
            if (e.key != m_prefix) {
                last = true;   // past the key
                break;
            }
            mine.append(e);
        }
        entries = std::move(mine);
    }
    const Page page = m_page;   // a copy: the callee may destroy this walk
    if (page(entries, last) && !last)
        next_();
}

} // namespace qbucket
