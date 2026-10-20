#pragma once

#include <qbucket/s3/transfer/ResumeStore.h>

#include <QMutex>

namespace qbucket::s3::transfer {

// ResumeStore in a directory: <id>.json (the record, written atomically)
// and <id>.parts (one JSON line per finished part, appended and flushed).
// A torn last line — the process died while writing it — is ignored. The
// directory is created on first use. Thread-safe.
class QBUCKET_S3_TRANSFER_EXPORT JsonFileResumeStore final : public ResumeStore {
public:
    explicit JsonFileResumeStore(QString directory);

    [[nodiscard]] QString directory() const { return m_dir; }

    Result<void> begin(const TransferRecord& record) override;
    Result<void> update(const TransferRecord& record) override;
    Result<void> addPart(const QString& id, const PartRecord& part) override;
    Result<ResumeState> load(const QString& id) const override;
    [[nodiscard]] QStringList ids() const override;
    Result<void> remove(const QString& id) override;

private:
    Result<void> writeRecord(const TransferRecord& record) const;
    [[nodiscard]] QString recordPath(const QString& id) const;
    [[nodiscard]] QString partsPath(const QString& id) const;

    QString m_dir;
    mutable QMutex m_mutex;
};

} // namespace qbucket::s3::transfer
