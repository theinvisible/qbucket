#pragma once

#include "controllers/Job.h"

#include <memory>

namespace qbucket {

// Enables or suspends a bucket's versioning (PLAN.md §6 M5) — a write to a
// bucket subresource, so only where Feature::BucketVersioningWrite is
// confirmed (profile or deep probe), never on the strength of a read probe.
// Refused while MFA delete is on (QBucket sends no MFA codes) and, for a
// suspend, on a bucket with object lock. Suspending is destructive in
// effect: overwrites and deletes are permanent from then on — the preview
// says so. Verify: the setting read back, a few times (gateways may answer
// stale for a while); then the bucket's capabilities are checked again.
class SetBucketVersioningJob : public Job {
    Q_OBJECT

public:
    static constexpr int kReadBackTries = 5;
    static constexpr int kReadBackDelayMs = 3000;

    SetBucketVersioningJob(const Account& account, const QString& bucket, s3::VersioningStatus status,
                           QObject* parent = nullptr);
    ~SetBucketVersioningJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    // Also enabling: Unknown must never unlock a write to a subresource.
    [[nodiscard]] bool isDestructive() const override { return true; }
    [[nodiscard]] s3::VersioningStatus status() const { return m_status; }
    void setReadBackDelayForTesting(int ms) { m_delayMs = ms; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;

private:
    void readBack_(int attempt);

    s3::VersioningStatus m_status;
    std::unique_ptr<QObject> m_run;
    int m_delayMs = kReadBackDelayMs;
};

} // namespace qbucket
