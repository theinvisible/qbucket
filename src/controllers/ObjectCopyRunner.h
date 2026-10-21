#pragma once

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

// Copies one object server-side the safe way (PLAN.md §12.3), for the jobs
// that copy or move: CopyObject up to 5 GiB, above that a multipart copy
// (transfer::Copier, UploadPartCopy) — both guarded by the planned source
// ETag (x-amz-copy-source-if-match), the storage class carried along. Then
// HEAD of the copy: the size, and the ETag where it is the content's MD5.
// A move deletes the source only after that check. Every request is
// journaled through the job; a failure always says what happened to the
// source.
//
// Thread affinity: the job's (GUI) thread.
class ObjectCopyRunner : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kCopyLimit = Q_INT64_C(5) * 1024 * 1024 * 1024;

    struct Item {
        QString sourceBucket;
        QString sourceKey;
        qint64 size = 0;
        QString etag;            // as planned; guards the copy
        QString storageClass;    // carried along (a copy would land in STANDARD)
        QString bucket;          // target
        QString key;
        bool deleteSource = false;   // a move
    };
    struct Result {
        bool ok = false;
        QString failure;         // translated; says what became of the source
        bool cancelled = false;  // the multipart copy was cancelled (no failure to report)
    };
    using Done = std::function<void(const Item& item, const Result& result)>;

    explicit ObjectCopyRunner(Job* job);
    ~ObjectCopyRunner() override;

    // `done` is called once, later, on this thread.
    void run(const Item& item, Done done);
    // Stops multipart copies (their uploads are aborted, sources stay);
    // single requests already sent finish.
    void cancel();

    // Single-part, plainly encrypted objects: the ETag is the content's MD5
    // and survives a copy unchanged.
    [[nodiscard]] static bool etagIsContentHash(const QString& etag);

private:
    void copyInParts_(const Item& item, Done done);
    void copied_(const Item& item, bool inParts, Done done);
    void deleteSource_(const Item& item, Done done);

    Job* m_job;
    std::vector<std::unique_ptr<s3::transfer::Copier>> m_copiers;
};

} // namespace qbucket
