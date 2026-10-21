#pragma once

#include "controllers/Job.h"

#include <memory>

namespace qbucket {

// Creates a bucket (PLAN.md §6 M6): a valid name (S3's rules, checked
// before anything is sent), the region, and optionally object lock (only
// possible now) and versioning (needs BucketVersioningWrite confirmed).
// A job on no existing bucket: the gate checks the account and the server.
// Plan: the name is looked up — taken (by this account or someone else)
// is refused before sending. Execute: CreateBucket is never retried after a
// lost answer; the bucket is looked up instead. Verify: the bucket answers
// (a new bucket may take a moment — Hetzner), with object lock and
// versioning as asked.
class CreateBucketJob : public Job {
    Q_OBJECT

public:
    struct Options {
        QString region;              // empty: the account's
        bool objectLock = false;     // also turns versioning on
        bool versioning = false;
    };

    static constexpr int kVerifyTries = 10;
    static constexpr int kVerifyDelayMs = 3000;

    CreateBucketJob(const Account& account, const QString& name, const Options& options, QObject* parent = nullptr);
    ~CreateBucketJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QStringList buckets() const override { return {}; }
    [[nodiscard]] bool isDestructive() const override { return false; }
    [[nodiscard]] const Options& options() const { return m_options; }
    void setVerifyDelayForTesting(int ms) { m_delayMs = ms; }

    // Why the name can't be a bucket's (translated); empty: it can.
    [[nodiscard]] static QString nameProblem(const QString& name);

protected:
    void plan() override;
    void execute() override;
    void verify() override;

private:
    void created_();
    void enableVersioning_();
    void verifyBucket_(int attempt);
    void verifySettings_(int attempt);

    Options m_options;
    std::unique_ptr<QObject> m_run;
    int m_delayMs = kVerifyDelayMs;
    bool m_versioningSent = false;
};

} // namespace qbucket
