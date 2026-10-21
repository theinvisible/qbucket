#include <qbucket/s3/ListObjectVersionsPager.h>

#include <QSet>

namespace qbucket::s3 {

namespace {

QString markerPair(const QString& key, const QString& versionId)
{
    return key + QChar(u'\0') + versionId;
}

} // namespace

struct ListObjectVersionsPager::State {
    ListObjectVersionsParams params;
    QString keyMarker;
    QString versionIdMarker;
    QSet<QString> seenMarkers;
    bool done = false;
    bool fetching = false;
    int pages = 0;
    RequestId current = RequestId::Invalid;
};

ListObjectVersionsPager::ListObjectVersionsPager(Session& session, ListObjectVersionsParams params)
    : m_session(session)
    , m_state(std::make_shared<State>())
{
    m_state->params = std::move(params);
    m_state->keyMarker = m_state->params.keyMarker;
    m_state->versionIdMarker = m_state->params.versionIdMarker;
    if (!m_state->keyMarker.isEmpty())
        m_state->seenMarkers.insert(markerPair(m_state->keyMarker, m_state->versionIdMarker));
}

ListObjectVersionsPager::~ListObjectVersionsPager() = default;

const ListObjectVersionsParams& ListObjectVersionsPager::params() const
{
    return m_state->params;
}

bool ListObjectVersionsPager::hasMore() const
{
    return !m_state->done;
}

bool ListObjectVersionsPager::isFetching() const
{
    return m_state->fetching;
}

int ListObjectVersionsPager::pagesFetched() const
{
    return m_state->pages;
}

RequestId ListObjectVersionsPager::fetchNext(QObject* context, Callback<ListObjectVersionsPage> callback)
{
    if (m_state->done || m_state->fetching)
        return RequestId::Invalid;

    ListObjectVersionsParams p = m_state->params;
    p.keyMarker = m_state->keyMarker;
    p.versionIdMarker = m_state->versionIdMarker;

    const std::weak_ptr<State> weak = m_state;
    const RequestId id = m_session.listObjectVersions(
        p, context, [weak, callback = std::move(callback)](Result<ListObjectVersionsPage> r) mutable {
            const std::shared_ptr<State> s = weak.lock();
            if (s) {
                s->fetching = false;
                s->current = RequestId::Invalid;
                if (!r) {
                    s->done = r.error().category != ErrorCategory::Cancelled
                        && r.error().category != ErrorCategory::Transient;
                } else {
                    ++s->pages;
                    const QString pair = markerPair(r->nextKeyMarker, r->nextVersionIdMarker);
                    if (!r->isTruncated) {
                        s->done = true;
                    } else if (s->seenMarkers.contains(pair)) {
                        s->done = true;
                        r = Error::make(ErrorCategory::Protocol,
                                        QStringLiteral("version listing returned markers it had "
                                                       "returned before (listing loop)"));
                    } else {
                        s->seenMarkers.insert(pair);
                        s->keyMarker = r->nextKeyMarker;
                        s->versionIdMarker = r->nextVersionIdMarker;
                    }
                }
            }
            callback(std::move(r));
        });
    if (id != RequestId::Invalid) {
        m_state->fetching = true;
        m_state->current = id;
    }
    return id;
}

void ListObjectVersionsPager::cancel()
{
    if (m_state->current != RequestId::Invalid)
        m_session.cancel(m_state->current);
}

} // namespace qbucket::s3
