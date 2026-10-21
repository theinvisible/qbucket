#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QSet>

#include <memory>
#include <vector>

namespace qbucket {

class ObjectCopyRunner;

// Renames an object or a folder in place (PLAN.md §12.3: move = copy →
// verify → delete source; never a blind delete after a copy).
//   Plan    — the new name is one sendable segment; nothing may exist
//             under the target name (no overwriting in M2); the source is
//             looked up afresh — a folder listed completely into a
//             PlanFile with each key's size, ETag and storage class.
//   Execute — per object: CopyObject with x-amz-copy-source-if-match (a
//             concurrent change aborts instead of being renamed), the
//             storage class kept — above CopyObject's 5 GiB a multipart
//             copy (transfer::Copier, UploadPartCopy, each part guarded the
//             same way); HEAD of the copy (size, and the ETag where it is
//             the content's MD5); only then DeleteObject of the source.
//             Failures leave the source alone and are reported.
//   Verify  — the old name holds nothing but what failed, the new name
//             everything that was renamed.
class RenameJob : public Job {
    Q_OBJECT

public:
    static constexpr int kParallel = 4;
    static constexpr int kSamples = 10;

    // `key`: an object key, or a folder prefix ending with "/".
    RenameJob(const Account& account, const QString& bucket, const QString& key, bool folder,
              const QString& newName, const QString& planDir, QObject* parent = nullptr);
    ~RenameJob() override;

    [[nodiscard]] QString title() const override;
    // Objects above 5 GiB are copied in parts.
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] QString sourceKey() const { return m_source; }
    [[nodiscard]] QString targetKey() const { return m_target; }
    // Old keys whose rename failed (they are still there).
    [[nodiscard]] QStringList failedKeys() const { return m_failedKeys; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    struct Entry {
        QString key;
        qint64 size = 0;
        QString etag;
        QString storageClass;
    };
    void checkTarget_();
    void planSource_();
    void planFolderPage_();
    void planDone_();
    void pump_();
    void renameOne_(const Entry& e);
    void settled_();
    void addFailure_(const QString& key, const QString& reason);
    [[nodiscard]] QString targetFor_(const QString& key) const;
    [[nodiscard]] Report report_() const;

    bool m_folder;
    QString m_name;
    QString m_source;
    QString m_target;
    QString m_planDir;
    std::unique_ptr<PlanFile> m_plan;
    std::unique_ptr<QObject> m_run;
    std::unique_ptr<s3::ListObjectsPager> m_pager;
    ObjectCopyRunner* m_runner = nullptr;   // child
    bool m_large = false;   // something above 5 GiB is in the plan

    qint64 m_bytes = 0;
    QStringList m_samples;

    int m_inFlight = 0;
    bool m_reading = false;
    bool m_eof = false;
    qint64 m_succeeded = 0;
    QStringList m_failedKeys;
    QSet<QString> m_failedSet;
    QStringList m_failures;
    QString m_verification;
    bool m_verified = false;
};

} // namespace qbucket
