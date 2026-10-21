#pragma once

#include <qbucket/s3/Export.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/Types.h>

#include <memory>

namespace qbucket::s3 {

// Walks ListObjectVersions page by page, like ListObjectsPager: a marker
// pair (key, version id) seen before aborts with a Protocol error ("listing
// loop") instead of looping forever.
//
// A key's versions may be split across pages; entries arrive in server
// order (key, then newest first).
//
// Thread affinity: use it from one thread. It may be destroyed while a page
// is in flight; the callback is then still delivered to its context.
class QBUCKET_S3_EXPORT ListObjectVersionsPager {
public:
    ListObjectVersionsPager(Session& session, ListObjectVersionsParams params);
    ~ListObjectVersionsPager();
    ListObjectVersionsPager(const ListObjectVersionsPager&) = delete;
    ListObjectVersionsPager& operator=(const ListObjectVersionsPager&) = delete;

    [[nodiscard]] const ListObjectVersionsParams& params() const;
    // True until the last page has arrived (or an error ended the walk).
    [[nodiscard]] bool hasMore() const;
    [[nodiscard]] bool isFetching() const;
    [[nodiscard]] int pagesFetched() const;

    // Requests the next page. Returns Invalid while a page is in flight or
    // when there is nothing more.
    RequestId fetchNext(QObject* context, Callback<ListObjectVersionsPage> callback);
    void cancel();

private:
    struct State;
    Session& m_session;
    std::shared_ptr<State> m_state;
};

} // namespace qbucket::s3
