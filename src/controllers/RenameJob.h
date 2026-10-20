#pragma once

#include "controllers/Job.h"
#include "controllers/PlanFile.h"

#include <qbucket/s3/ListObjectsPager.h>

#include <QSet>

#include <memory>

namespace qbucket {

// Renames an object or a folder in place (PLAN.md §12.3: move = copy →
// verify → delete source; never a blind delete after a copy).
//   Plan    — the new name is one sendable segment; nothing may exist
//             under the target name (no overwriting in M2); the source is
//             looked up afresh — a folder listed completely into a
//             PlanFile with each key's size, ETag and storage class.
//             Objects above the CopyObject limit (5 GiB) stop the plan.
//   Execute — per object: CopyObject with x-amz-copy-source-if-match (a
//             concurrent change aborts instead of being renamed), the
//             storage class kept; HEAD of the copy (size, and the ETag
//             where it is the content's MD5); only then DeleteObject of the
//             source. Failures leave the source alone and are reported.
//   Verify  — the old name holds nothing but what failed, the new name
//             everything that was renamed.
class RenameJob : public Job {
    Q_OBJECT

public:
    static constexpr qint64 kCopyLimit = Q_INT64_C(5) * 1024 * 1024 * 1024;
    static constexpr int kParallel = 4;
    static constexpr int kSamples = 10;

    // `key`: an object key, or a folder prefix ending with "/".
    RenameJob(const Account& account, const QString& bucket, const QString& key, bool folder,
              const QString& newName, const QString& planDir, QObject* parent = nullptr);
    ~RenameJob() override;

    [[nodiscard]] QString title() const override;
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
    void copied_(const Entry& e, const QString& target);
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

    qint64 m_bytes = 0;
    QStringList m_samples;
    QStringList m_tooLarge;

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
