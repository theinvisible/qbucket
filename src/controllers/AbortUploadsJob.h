#pragma once

#include "controllers/Job.h"

#include <memory>

namespace qbucket {

// Aborts unfinished multipart uploads (the "Incomplete Uploads" view): the
// parts uploaded so far are deleted, no object is touched. Destructive —
// what the parts held is gone — so it is confirmed. Resume records of the
// aborted uploads are removed too.
class AbortUploadsJob : public Job {
    Q_OBJECT

public:
    struct Upload {
        QString key;
        QString uploadId;
    };
    static constexpr int kParallel = 4;

    AbortUploadsJob(const Account& account, const QString& bucket, const QList<Upload>& uploads,
                    QObject* parent = nullptr);
    ~AbortUploadsJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override { return {s3::Feature::MultipartUpload}; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    void pump_();

    QList<Upload> m_uploads;
    std::unique_ptr<QObject> m_run;
    qsizetype m_next = 0;
    int m_inFlight = 0;
    Report m_report;
};

} // namespace qbucket
