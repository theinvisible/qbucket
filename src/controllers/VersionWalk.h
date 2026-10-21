#pragma once

#include <qbucket/s3/ListObjectVersionsPager.h>
#include <qbucket/s3/ListObjectsPager.h>

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace qbucket {

// Walks ListObjectVersions for the version jobs: every version and delete
// marker below a prefix (flat), or of exactly one key. Page by page — the
// caller asks for the next page when it has dealt with this one, so plans
// of any size stay on disk, not in memory.
//
// Exactly one key: the listing's prefix is the key; every key listed starts
// with it and the key itself sorts first, so the first other key ends the
// walk.
//
// CurrentOnly: a server without ListObjectVersions (Garage) — ListObjectsV2
// instead, each object as its current entry without a version id.
//
// Thread affinity: the caller's thread. Answers come through `context` and
// are dropped with it. The walk may be destroyed inside its own callbacks —
// then they must return false.
class VersionWalk {
public:
    // `entries` of this page (only the key's, for an exact walk); `last`:
    // no page follows. Return true to go on with the next page.
    using Page = std::function<bool(const QList<s3::VersionEntry>& entries, bool last)>;
    using Failed = std::function<void(const s3::Error& error)>;

    enum class Source { Versions, CurrentOnly };

    VersionWalk(s3::Session& session, const QString& bucket, const QString& prefixOrKey, bool exactKey,
                QObject* context, Source source = Source::Versions);
    ~VersionWalk();
    VersionWalk(const VersionWalk&) = delete;
    VersionWalk& operator=(const VersionWalk&) = delete;

    [[nodiscard]] QString prefix() const { return m_prefix; }
    [[nodiscard]] bool exactKey() const { return m_exact; }
    void run(Page page, Failed failed);

private:
    void next_();
    void deliver_(QList<s3::VersionEntry> entries, bool last);

    std::unique_ptr<s3::ListObjectVersionsPager> m_versions;
    std::unique_ptr<s3::ListObjectsPager> m_objects;
    QString m_prefix;
    bool m_exact = false;
    Page m_page;
    Failed m_failed;
    QObject* m_context;
    // A page still on its way when the walk goes: its answer is ignored.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

} // namespace qbucket
