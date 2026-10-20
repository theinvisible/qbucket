#include <qbucket/s3/ListObjectsPager.h>

#include <QSet>

namespace qbucket::s3 {

struct ListObjectsPager::State {
    ListObjectsParams params;
    QString nextToken;
    QSet<QString> seenTokens;
    bool done = false;
    bool fetching = false;
    int pages = 0;
    RequestId current = RequestId::Invalid;
};

ListObjectsPager::ListObjectsPager(Session& session, ListObjectsParams params)
    : m_session(session)
    , m_state(std::make_shared<State>())
{
    m_state->params = std::move(params);
    m_state->nextToken = m_state->params.continuationToken;
    if (!m_state->nextToken.isEmpty())
        m_state->seenTokens.insert(m_state->nextToken);
}

ListObjectsPager::~ListObjectsPager() = default;

const ListObjectsParams& ListObjectsPager::params() const
{
    return m_state->params;
}

bool ListObjectsPager::hasMore() const
{
    return !m_state->done;
}

bool ListObjectsPager::isFetching() const
{
    return m_state->fetching;
}

int ListObjectsPager::pagesFetched() const
{
    return m_state->pages;
}

RequestId ListObjectsPager::fetchNext(QObject* context, Callback<ListPage> callback)
{
    if (m_state->done || m_state->fetching)
        return RequestId::Invalid;

    ListObjectsParams p = m_state->params;
    p.continuationToken = m_state->nextToken;
    if (!p.continuationToken.isEmpty())
        p.startAfter.clear();   // the token supersedes start-after

    const std::weak_ptr<State> weak = m_state;
    const RequestId id = m_session.listObjects(
        p, context, [weak, callback = std::move(callback)](Result<ListPage> r) mutable {
            const std::shared_ptr<State> s = weak.lock();
            if (s) {
                s->fetching = false;
                s->current = RequestId::Invalid;
                if (!r) {
                    s->done = r.error().category != ErrorCategory::Cancelled
                        && r.error().category != ErrorCategory::Transient;
                } else {
                    ++s->pages;
                    const QString& token = r->nextContinuationToken;
                    if (!r->isTruncated) {
                        s->done = true;
                    } else if (s->seenTokens.contains(token)) {
                        s->done = true;
                        r = Error::make(ErrorCategory::Protocol,
                                        QStringLiteral("listing returned a continuation token it "
                                                       "had returned before (listing loop)"));
                    } else {
                        s->seenTokens.insert(token);
                        s->nextToken = token;
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

void ListObjectsPager::cancel()
{
    if (m_state->current != RequestId::Invalid)
        m_session.cancel(m_state->current);
}

} // namespace qbucket::s3
