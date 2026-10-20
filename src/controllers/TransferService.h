#pragma once

#include <qbucket/s3/transfer/AdaptiveConcurrency.h>
#include <qbucket/s3/transfer/ResumeStore.h>

#include <QObject>
#include <QString>
#include <QUuid>

#include <map>
#include <memory>

namespace qbucket {

// What the transfer jobs of all accounts share (PLAN.md §13.3): one
// adaptive concurrency limit per account — every upload, download and copy
// of that account draws its parts from it — and the resume store under
// AppLocalDataLocation/transfers (never roaming, never secrets).
//
// Thread affinity: GUI thread.
class TransferService : public QObject {
    Q_OBJECT

public:
    explicit TransferService(QObject* parent = nullptr);
    // A store elsewhere (tests).
    TransferService(const QString& directory, QObject* parent);
    ~TransferService() override;

    // The account's limiter, created on first use.
    [[nodiscard]] s3::transfer::AdaptiveConcurrency* limiter(const QUuid& account);
    [[nodiscard]] std::shared_ptr<s3::transfer::ResumeStore> resumeStore() const { return m_store; }
    // AppLocalDataLocation/transfers: part records (the store) and the
    // records of the jobs that can be resumed (jobs/).
    [[nodiscard]] QString directory() const { return m_dir; }
    [[nodiscard]] QString jobDirectory() const;

    // The ceiling of every limiter (Settings → Transfers).
    void setMaxParallelParts(int parts);
    [[nodiscard]] int maxParallelParts() const { return m_maxParts; }

private:
    QString m_dir;
    std::shared_ptr<s3::transfer::ResumeStore> m_store;
    std::map<QUuid, std::unique_ptr<s3::transfer::AdaptiveConcurrency>> m_limiters;
    int m_maxParts = 16;
};

} // namespace qbucket
