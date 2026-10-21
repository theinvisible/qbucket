#pragma once

#include "controllers/ObjectSnapshot.h"

#include <qbucket/s3/Types.h>

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace qbucket {

class Job;

namespace s3::transfer {
class Copier;
}

// Rewrites one object onto itself — with new headers (an edit) or from an
// older version (a restore) — the safe way (PLAN.md §12.3), for
// EditObjectsJob and RestoreVersionsJob. The caller decides what the copy
// carries, from a fresh ObjectSnapshot of the source; this then:
//   - copies with MetadataDirective REPLACE (a multipart copy above 5 GiB),
//     guarded by the source's ETag (x-amz-copy-source-if-match), the canned
//     ACL and SSE-S3 sent along;
//   - writes the tags back where the server dropped them (RGW does);
//   - checks the object: size, the content's ETag where it is an MD5, every
//     header, the class, tags and ACL — once more after a moment (Hetzner's
//     reads lag), then a mismatch is a failure.
// Every request is journaled through the job.
//
// Thread affinity: the job's (GUI) thread.
class InPlaceCopy : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kCopyLimit = Q_INT64_C(5) * 1024 * 1024 * 1024;

    struct Request {
        QString key;
        QString sourceVersionId;     // a restore: the version copied; empty: the current object
        ObjectSnapshot source;       // the state carried over (of that version)
        s3::ObjectHeaders headers;   // what the copy sends
        QString targetClass;
        QString canned;              // a non-private canned ACL sent along
        QString journalNote;         // e.g. "edit in place"
    };
    // An empty failure: copied and checked. Translated otherwise.
    using Done = std::function<void(const QString& failure)>;

    explicit InPlaceCopy(Job* job);
    ~InPlaceCopy() override;

    // Why a copy of this object (version) would lose something it can't
    // carry over; empty: it can be copied safely. SSE-C / KMS encryption, an
    // archive class, a custom ACL, tags or a public ACL where writing them
    // isn't confirmed, headers that aren't plain ASCII — and with
    // `lockMatters` retention or a legal hold (the new version would not be
    // protected).
    [[nodiscard]] static QString cannotCarry(const ObjectSnapshot& snapshot, const QString& providerId, bool writeTags,
                                             bool writeAcl, bool lockMatters);

    // `done` is called once, later, on this thread.
    void run(const Request& request, Done done);
    // Stops multipart copies (their uploads are aborted, the object stays as
    // it was); single requests already sent finish.
    void cancel();
    void setRecheckDelay(int ms) { m_recheckMs = ms; }

private:
    struct State;
    void copyInParts_(const std::shared_ptr<State>& s);
    void copied_(const std::shared_ptr<State>& s);
    void restoreTags_(const std::shared_ptr<State>& s);
    void check_(const std::shared_ptr<State>& s);

    Job* m_job;
    std::vector<std::unique_ptr<s3::transfer::Copier>> m_copiers;
    int m_recheckMs = 2000;
};

} // namespace qbucket
