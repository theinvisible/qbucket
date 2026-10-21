#pragma once

#include "controllers/Job.h"

#include <qbucket/s3/BucketConfig.h>

#include <memory>

namespace qbucket {

// Writes or removes one of a bucket's configuration documents — policy,
// CORS, lifecycle, the object lock default, the ACL, the public access
// block (PLAN.md §6 M6). A write to a bucket subresource: only where the
// kind's write-only feature is confirmed (profile or deep probe), never on
// the strength of a read probe, so Unknown never unlocks it.
//
// Plan: the document checked locally, the current one read fresh; nothing
// to do when they are the same. The preview shows both and says what the
// change does (public access, lifecycle rules that delete, COMPLIANCE).
// Execute: PUT / DELETE ?<subresource>; the library refuses answers that
// look like CreateBucket / DeleteBucket. Verify: read back until it is
// equivalent (gateways may answer stale for a while), after a DELETE the
// bucket is looked up too — a proxy that ran DeleteBucket would show here;
// then the bucket's capabilities are checked again.
class SetBucketConfigJob : public Job {
    Q_OBJECT

public:
    static constexpr int kReadBackTries = 5;
    static constexpr int kReadBackDelayMs = 3000;

    // `document`: the new document; for the ACL a canned ACL instead
    // (`cannedAcl`, empty document).
    static SetBucketConfigJob* put(const Account& account, const QString& bucket, s3::BucketConfig kind,
                                   const QByteArray& document, const QString& cannedAcl = {});
    static SetBucketConfigJob* remove(const Account& account, const QString& bucket, s3::BucketConfig kind);
    ~SetBucketConfigJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] bool isDestructive() const override { return true; }
    [[nodiscard]] s3::BucketConfig kind() const { return m_kind; }
    [[nodiscard]] bool removes() const { return m_remove; }
    void setReadBackDelayForTesting(int ms) { m_delayMs = ms; }

    // The kind as the user knows it ("CORS rules", "the bucket policy").
    [[nodiscard]] static QString kindText(s3::BucketConfig kind);
    // Indented for reading; the document itself when it can't be parsed.
    [[nodiscard]] static QString pretty(s3::BucketConfig kind, const QByteArray& document);
    // Empty when the document may be sent, otherwise why not (translated
    // prefix, the library's English detail).
    [[nodiscard]] static QString problem(s3::BucketConfig kind, const QByteArray& document);

protected:
    void plan() override;
    void execute() override;
    void verify() override;

private:
    SetBucketConfigJob(const Account& account, const QString& bucket, s3::BucketConfig kind, bool remove);
    void planWith_(const s3::BucketConfigDocument& current);
    [[nodiscard]] bool matches_(const s3::BucketConfigDocument& doc) const;
    [[nodiscard]] QStringList warnings_(const s3::BucketConfigDocument& current, Preview* p) const;
    void readBack_(int attempt);
    // After a removal (or an answer that doesn't fit): is the bucket still there?
    void checkBucket_(const QString& readBack, bool readBackOk);

    s3::BucketConfig m_kind;
    bool m_remove;
    QByteArray m_document;
    QString m_cannedAcl;
    std::unique_ptr<QObject> m_run;
    int m_delayMs = kReadBackDelayMs;
};

} // namespace qbucket
