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

} // namespace

VersionWalk::VersionWalk(s3::Session& session, const QString& bucket, const QString& prefixOrKey, bool exactKey,
                         QObject* context)
    : m_pager(session, paramsFor(bucket, prefixOrKey)),
      m_prefix(prefixOrKey),
      m_exact(exactKey),
      m_context(context)
{
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
    m_pager.fetchNext(m_context, [this, alive = std::weak_ptr<int>(m_alive)](const s3::Result<s3::ListObjectVersionsPage>& r) {
        if (alive.expired())
            return;
        // Copies: the callee may destroy this walk.
        if (!r) {
            const Failed failed = m_failed;
            failed(r.error());
            return;
        }
        QList<s3::VersionEntry> entries;
        bool last = !m_pager.hasMore();
        if (!m_exact) {
            entries = r->entries;
        } else {
            for (const s3::VersionEntry& e : r->entries) {
                if (e.key != m_prefix) {
                    last = true;   // past the key
                    break;
                }
                entries.append(e);
            }
        }
        const Page page = m_page;
        if (page(entries, last) && !last)
            next_();
    });
}

} // namespace qbucket
