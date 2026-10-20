#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>

#include <memory>

namespace qbucket::s3 {

// Walks a listing page by page (fetchMore-style) and guarantees it ends:
// a continuation token seen before aborts with a Protocol error
// ("listing loop") instead of looping forever — an error is never an empty
// or endless folder.
//
// Thread affinity: use it from one thread. It may be destroyed while a page
// is in flight; the callback is then still delivered to its context.
class QBUCKET_S3_EXPORT ListObjectsPager {
public:
    ListObjectsPager(Session& session, ListObjectsParams params);
    ~ListObjectsPager();
    ListObjectsPager(const ListObjectsPager&) = delete;
    ListObjectsPager& operator=(const ListObjectsPager&) = delete;

    [[nodiscard]] const ListObjectsParams& params() const;
    // True until the last page has arrived (or an error ended the walk).
    [[nodiscard]] bool hasMore() const;
    [[nodiscard]] bool isFetching() const;
    [[nodiscard]] int pagesFetched() const;

    // Requests the next page. Returns Invalid while a page is in flight or
    // when there is nothing more.
    RequestId fetchNext(QObject* context, Callback<ListPage> callback);
    void cancel();

private:
    struct State;
    Session& m_session;
    std::shared_ptr<State> m_state;
};

} // namespace qbucket::s3
