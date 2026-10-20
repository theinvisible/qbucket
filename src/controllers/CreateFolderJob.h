#pragma once

#include "controllers/Job.h"

#include <memory>

namespace qbucket {

// Creates a folder: the zero-byte marker object "<parent><name>/"
// (PLAN.md §6 M2). Plan: the name must be a single, sendable segment; a
// folder that exists already — as a marker or implicitly as a key prefix —
// is left alone (skipped). Execute: PUT with Content-MD5. Verify: HEAD.
//
// Between the check and the PUT there is a window in which another client
// could create the same key; without conditional writes that window
// stays (a marker over a marker is harmless).
class CreateFolderJob : public Job {
    Q_OBJECT

public:
    CreateFolderJob(const Account& account, const QString& bucket, const QString& parentPrefix,
                    const QString& name, QObject* parent = nullptr);

    // Empty if usable as a folder name, else why not (translated).
    [[nodiscard]] static QString validateName(const QString& name);

    [[nodiscard]] QString key() const { return m_key; }
    [[nodiscard]] QString title() const override;
    [[nodiscard]] bool isDestructive() const override { return false; }
    [[nodiscard]] bool needsConfirmation() const override { return false; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    void skip_(const QString& why);

    QString m_name;
    QString m_key;
    std::unique_ptr<QObject> m_run;   // context of the pending request
};

} // namespace qbucket
