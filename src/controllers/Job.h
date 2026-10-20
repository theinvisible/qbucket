#pragma once

#include "controllers/Journal.h"
#include "domain/Account.h"

#include <qbucket/s3/Capabilities.h>
#include <qbucket/s3/Session.h>

#include <QObject>
#include <QStringList>

namespace qbucket {

class AccountSessions;
class OperationController;

// One mutation on its way through the OperationController (PLAN.md §12.2):
// plan → preview → execute → verify → report. Subclasses implement the
// hooks; the controller calls them, checks the gate before planning and
// again before executing, and journals. Cancellable in every phase; a
// cancelled or partly failed run still reports what was done.
//
// Thread affinity: GUI thread. Owned by the OperationController; deleted
// after finished.
class Job : public QObject {
    Q_OBJECT

public:
    enum class Phase { Created, Planning, AwaitingConfirmation, Executing, Verifying, Finished };
    Q_ENUM(Phase)
    enum class Outcome { None, Succeeded, PartlyFailed, Failed, Cancelled, Refused };
    Q_ENUM(Outcome)

    // What the confirmation shows: the exact scope, from a fresh plan.
    struct Preview {
        QString action;          // the verb for title and button, e.g. "Delete"
        QString scope;           // e.g. "s3://bucket/photos/"
        qint64 objects = 0;
        qint64 bytes = -1;       // -1: not known
        QStringList samples;     // a few affected keys
        QStringList details;     // further lines (target name, …)
        QStringList warnings;
        bool permanent = false;  // nothing to recover afterwards (unversioned bucket)
        QString typeToConfirm;   // non-empty: has to be typed to confirm
    };
    struct Report {
        qint64 succeeded = 0;
        qint64 skipped = 0;
        qint64 failed = 0;
        QStringList failures;    // "key: reason", the first kMaxListed
        QString verification;    // what the verify step found
        bool verified = false;
    };
    static constexpr int kMaxListed = 50;

    Job(const Account& account, const QString& bucket, QObject* parent = nullptr);
    ~Job() override;

    [[nodiscard]] QString id() const { return m_id; }
    [[nodiscard]] const Account& account() const { return m_account; }
    [[nodiscard]] QString bucket() const { return m_bucket; }
    [[nodiscard]] Phase phase() const { return m_phase; }
    [[nodiscard]] Outcome outcome() const { return m_outcome; }
    [[nodiscard]] const Preview& preview() const { return m_preview; }
    [[nodiscard]] const Report& report() const { return m_report; }
    [[nodiscard]] QString errorText() const { return m_error; }   // Failed / Refused
    [[nodiscard]] qint64 progressDone() const { return m_done; }
    [[nodiscard]] qint64 progressTotal() const { return m_total; }

    // One line for menus, logs and the progress view.
    [[nodiscard]] virtual QString title() const = 0;
    // Features the operation needs; checked before planning and executing.
    [[nodiscard]] virtual QList<s3::Feature> requiredFeatures() const { return {}; }
    [[nodiscard]] virtual bool isDestructive() const { return true; }

    // The user's answer to the preview.
    void confirm();
    void cancel();
    [[nodiscard]] bool isCancelled() const { return m_cancelled; }
    [[nodiscard]] bool isConfirmed() const { return m_confirmed; }

signals:
    void phaseChanged(qbucket::Job::Phase phase);
    void previewReady();
    void progressChanged();
    void finished();

protected:
    // Hooks — each ends with one of the protected calls below, from the
    // event loop (never synchronously inside the hook for network work).
    virtual void plan() = 0;      // → setPreview_() | fail_()
    virtual void execute() = 0;   // → executed_()  | finish_()
    virtual void verify() = 0;    // → finish_()
    virtual void onCancel() {}    // stop what runs; then finish_() with what was done

    void setPreview_(const Preview& preview);
    void setProgress_(qint64 done, qint64 total);
    void executed_();
    void finish_(const Report& report);
    void fail_(const QString& error);
    void journal_(const QList<Journal::Record>& records);
    // A journal record of this job with account and bucket filled in.
    [[nodiscard]] Journal::Record record_(const QString& operation, const QString& key = {}) const;
    // The account's session; nullptr after shutdown.
    [[nodiscard]] s3::Session* session_() const;
    [[nodiscard]] OperationController* controller_() const { return m_controller; }

private:
    friend class OperationController;
    void setPhase_(Phase phase);
    void refuse_(const QString& reason);

    QString m_id;
    Account m_account;
    QString m_bucket;
    Phase m_phase = Phase::Created;
    Outcome m_outcome = Outcome::None;
    Preview m_preview;
    Report m_report;
    QString m_error;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelled = false;
    bool m_confirmed = false;
    OperationController* m_controller = nullptr;
};

} // namespace qbucket
