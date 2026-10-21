#pragma once

#include "controllers/Job.h"

#include <qbucket/s3/Types.h>

#include <memory>

namespace qbucket {

// Sets an object version's retention or legal hold (PLAN.md §6 M6, §12.3) —
// subresource writes, only where Feature::RetentionWrite / LegalHoldWrite
// is confirmed.
//   Plan    — HEAD (which version, size, ETag) and the current retention /
//             hold. A COMPLIANCE retention can only be extended; shortening
//             or removing a GOVERNANCE one needs the governance bypass,
//             which the preview offers for this one change (never preset).
//             Setting COMPLIANCE is typed: nobody can take it back.
//   Execute — on exactly the version planned.
//   Verify  — the new value reads back; the object's data is unchanged.
class ObjectLockJob : public Job {
    Q_OBJECT

public:
    enum class Change { Retention, RemoveRetention, LegalHold };

    static ObjectLockJob* retention(const Account& account, const QString& bucket, const QString& key,
                                    const QString& versionId, s3::ObjectLockMode mode, const QDateTime& retainUntil,
                                    QObject* parent = nullptr);
    static ObjectLockJob* removeRetention(const Account& account, const QString& bucket, const QString& key,
                                          const QString& versionId, QObject* parent = nullptr);
    static ObjectLockJob* legalHold(const Account& account, const QString& bucket, const QString& key,
                                    const QString& versionId, bool on, QObject* parent = nullptr);

    [[nodiscard]] QString title() const override;
    [[nodiscard]] QList<s3::Feature> requiredFeatures() const override;
    [[nodiscard]] bool isDestructive() const override { return true; }
    [[nodiscard]] Change change() const { return m_change; }
    // The plan found that the change needs the governance bypass.
    [[nodiscard]] bool needsBypass() const { return m_needsBypass; }

protected:
    void plan() override;
    void execute() override;
    void verify() override;
    void onCancel() override;

private:
    ObjectLockJob(const Account& account, const QString& bucket, const QString& key, const QString& versionId,
                  Change change, QObject* parent);
    void planWith_(const s3::ObjectMeta& meta, const s3::ObjectRetention& current, bool held);
    void readBack_();
    [[nodiscard]] QString describe_(const s3::ObjectRetention& r) const;

    QString m_key;
    QString m_versionId;          // asked for; empty: the current version
    Change m_change;
    s3::ObjectLockMode m_mode = s3::ObjectLockMode::Governance;
    QDateTime m_until;
    bool m_on = false;
    s3::ObjectMeta m_planned;     // the version written to
    bool m_needsBypass = false;
    std::unique_ptr<QObject> m_run;
};

} // namespace qbucket
