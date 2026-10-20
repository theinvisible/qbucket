#pragma once

#include "controllers/Job.h"
#include "controllers/TransferJobStore.h"

#include <qbucket/s3/transfer/ResumeStore.h>

#include <memory>

namespace qbucket {

// Throws away what an unfinished upload / download of an earlier session
// left behind (the user chose "Discard"): its multipart uploads are aborted
// on the server, partial download files removed, every record deleted.
// Nothing that was finished is touched.
class DiscardTransfersJob : public Job {
    Q_OBJECT

public:
    DiscardTransfersJob(const Account& account, const TransferJobStore::Record& record, QObject* parent = nullptr);
    ~DiscardTransfersJob() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] bool isDestructive() const override { return false; }   // only our own unfinished uploads
    [[nodiscard]] bool isMutating() const override { return m_record.kind == TransferKind::Upload; }
    [[nodiscard]] bool needsConfirmation() const override { return false; }   // asked in the resume dialog

protected:
    void plan() override;
    void execute() override;
    void verify() override;

private:
    void next_();

    TransferJobStore::Record m_record;
    std::unique_ptr<QObject> m_run;
    QList<s3::transfer::ResumeState> m_states;
    qsizetype m_index = 0;
    Report m_report;
};

} // namespace qbucket
